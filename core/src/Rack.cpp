#include "aicontact/Rack.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>

#include "aicontact/Analyze.h"
#include "aicontact/Ui.h"
#include "aicontact/Util.h"

namespace fs = std::filesystem;

namespace aicontact {

using json = nlohmann::json;

namespace {

constexpr double kCrossfadeSeconds = 0.03;
constexpr double kFadeHoldSeconds = 0.5;  // keep the old instance alive this long after a swap

// float -> double with 6 significant digits, so JSON shows 0.3 rather than 0.30000001.
double num(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return std::strtod(buf, nullptr);
}

// Instruments named by path (API `path`, drag & drop, project state). Checked before anything is
// opened: only regular .wasm files of a sane size, so a path cannot make the rack read from a device
// (/dev/zero), block on a FIFO or slurp arbitrary files.
constexpr uintmax_t kMaxInstrumentFileBytes = 64ull * 1024 * 1024;

std::vector<uint8_t> readInstrumentFile(const std::string& path) {
    std::string lower = path;
    for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
    if (lower.size() < 5 || lower.compare(lower.size() - 5, 5, ".wasm") != 0)
        throw std::runtime_error("'path' must name a .wasm file (send other content as wasm_base64)");
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    if (!fs::is_regular_file(p, ec)) throw std::runtime_error("no such regular file: " + path);
    const uintmax_t size = fs::file_size(p, ec);
    if (ec || size > kMaxInstrumentFileBytes)
        throw std::runtime_error("instrument file too large (limit 64 MiB): " + path);
    std::vector<uint8_t> bytes = readFile(path);
    if (bytes.size() > kMaxInstrumentFileBytes) throw std::runtime_error("instrument file too large (limit 64 MiB): " + path);
    return bytes;
}

float dbToGain(float db) { return db <= -80.0f ? 0.0f : std::pow(10.0f, db / 20.0f); }
float gainToDb(float g) { return g <= 1e-5f ? -100.0f : 20.0f * std::log10(g); }

// "C4" / "F#3" / "Bb-1" / "60" -> MIDI key (C4 = 60).
int parseKey(const json& v) {
    if (v.is_number_integer()) {
        int k = v.get<int>();
        if (k < 0 || k > 127) throw std::runtime_error("key out of range 0..127");
        return k;
    }
    if (!v.is_string()) throw std::runtime_error("key must be an integer or a note name like \"C4\"");
    std::string s = v.get<std::string>();
    if (s.empty()) throw std::runtime_error("empty note name");
    static const int base[] = {9, 11, 0, 2, 4, 5, 7};  // A B C D E F G
    char c = (char)std::toupper((unsigned char)s[0]);
    if (c < 'A' || c > 'G') {
        size_t used = 0;
        int k = std::stoi(s, &used);
        if (used != s.size() || k < 0 || k > 127) throw std::runtime_error("bad note name: " + s);
        return k;
    }
    int semitone = base[c - 'A'];
    size_t i = 1;
    while (i < s.size() && (s[i] == '#' || s[i] == 'b')) semitone += (s[i++] == '#') ? 1 : -1;
    if (i >= s.size()) throw std::runtime_error("note name needs an octave, e.g. C4: " + s);
    int octave = std::stoi(s.substr(i));
    int k = (octave + 1) * 12 + semitone;
    if (k < 0 || k > 127) throw std::runtime_error("note out of range: " + s);
    return k;
}

int jsonInt(const json& p, const char* key, int lo, int hi) {
    if (!p[key].is_number()) throw std::runtime_error(std::string("'") + key + "' must be a number");
    int v = (int)std::lround(p[key].get<double>());
    if (v < lo || v > hi)
        throw std::runtime_error(std::string("'") + key + "' must be in " + std::to_string(lo) + ".." + std::to_string(hi));
    return v;
}

float jsonFloat(const json& p, const char* key, float lo, float hi) {
    if (!p[key].is_number()) throw std::runtime_error(std::string("'") + key + "' must be a number");
    return std::min(hi, std::max(lo, p[key].get<float>()));
}

}  // namespace

// ----------------------------------------------------------------------------

Rack::Rack() {
    blockEvents_.reserve(4096);
    current_.store(new Snapshot());
}

Rack::~Rack() {
    delete current_.exchange(nullptr);
    for (auto& r : retired_) delete r.snapshot;
}

std::string Rack::name() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return name_;
}

void Rack::setName(const std::string& n) {
    std::lock_guard<std::mutex> lock(mutex_);
    name_ = n.empty() ? "AI Contact" : n;
    ++changeCounter_;
}

// ----------------------------------------------------------------------------
// Snapshot publishing.
//
// The audio thread increments epoch_ before reading current_ and again when
// it is done, so the epoch is odd exactly while a block is being rendered.
// A snapshot that was replaced at epoch E can be freed once the epoch has
// moved past E (or immediately if E was even: no block was in flight, and
// any later block reads the new pointer).

void Rack::publishLocked() {
    auto* snap = new Snapshot();
    snap->slots = slots_;
    Snapshot* old = current_.exchange(snap, std::memory_order_seq_cst);
    uint64_t e = epoch_.load(std::memory_order_seq_cst);
    if (old) retired_.push_back({old, e});
    ++changeCounter_;
    collectGarbageLocked();
}

void Rack::collectGarbageLocked() {
    uint64_t now = epoch_.load(std::memory_order_seq_cst);
    auto it = std::remove_if(retired_.begin(), retired_.end(), [&](const Retired& r) {
        if ((r.epoch & 1) == 0 || now != r.epoch) {
            delete r.snapshot;
            return true;
        }
        return false;
    });
    retired_.erase(it, retired_.end());
}

// ----------------------------------------------------------------------------
// Audio thread

void Rack::prepare(double sampleRate, int /*maxBlockSize*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sampleRate <= 0) return;
    bool changed = sampleRate != sampleRate_.load();
    sampleRate_.store(sampleRate);
    if (!changed) return;
    for (auto& s : slots_) {
        if (!s.inst) continue;
        try {
            auto inst = InstrumentInstance::create(s.inst->modulePtr(), sampleRate, s.inst->paramValues(),
                                                   &log_, s.slot->id);
            s.inst = std::shared_ptr<InstrumentInstance>(inst.release());
        } catch (const std::exception& e) {
            log_.log("error", s.slot->id, std::string("re-init at new sample rate failed: ") + e.what());
        }
        s.fading.reset();
    }
    publishLocked();
}

void Rack::process(float* left, float* right, int frames, const MidiEvent* events, int numEvents,
                   const MacroChange* macros, int numMacros) {
    epoch_.fetch_add(1, std::memory_order_seq_cst);
    Snapshot* snap = current_.load(std::memory_order_seq_cst);

    std::fill(left, left + frames, 0.0f);
    std::fill(right, right + frames, 0.0f);
    const double sr = sampleRate_.load(std::memory_order_relaxed);
    const auto t0 = std::chrono::steady_clock::now();
    lastProcessTime_.store(std::chrono::duration<double>(t0.time_since_epoch()).count(), std::memory_order_relaxed);

    // Host automation.
    for (int i = 0; i < numMacros; ++i) {
        const auto& m = macros[i];
        if (m.slotIndex < 0 || (size_t)m.slotIndex >= snap->slots.size()) continue;
        InstrumentInstance* inst = snap->slots[m.slotIndex].inst.get();
        if (!inst) continue;
        const auto& params = inst->meta().params;
        if (m.paramIndex < 0 || (size_t)m.paramIndex >= params.size()) continue;
        inst->setParam(m.paramIndex, params[m.paramIndex].fromNormalized(m.normalized));
    }

    // Injected events (preview notes, panic) -> pending queue with due time.
    uint32_t r = injectRead_.load(std::memory_order_relaxed);
    const uint32_t w = injectWrite_.load(std::memory_order_acquire);
    while (r != w && numPending_ < kInjectCapacity) {
        const Injected& ev = injectRing_[r % kInjectCapacity];
        pending_[numPending_++] = {framePosition_ + ev.delayFrames, ev};
        ++r;
    }
    injectRead_.store(r, std::memory_order_release);

    // Merge host events and due injected events into one sorted list.
    blockEvents_.clear();
    for (int i = 0; i < numEvents && blockEvents_.size() < blockEvents_.capacity(); ++i) {
        const auto& e = events[i];
        blockEvents_.push_back({std::min(std::max(e.offset, 0), std::max(frames - 1, 0)), e.status, e.data1, e.data2, 0});
    }
    for (int i = 0; i < numPending_;) {
        const Pending& p = pending_[i];
        if (p.dueFrame < framePosition_ + (uint64_t)frames && blockEvents_.size() < blockEvents_.capacity()) {
            int offset = p.dueFrame > framePosition_ ? (int)(p.dueFrame - framePosition_) : 0;
            blockEvents_.push_back({offset, p.ev.status, p.ev.data1, p.ev.data2, p.ev.targetSerial});
            pending_[i] = pending_[--numPending_];
        } else {
            ++i;
        }
    }
    // Insertion sort (stable, allocation free; the list is almost sorted).
    for (size_t i = 1; i < blockEvents_.size(); ++i) {
        BlockEvent tmp = blockEvents_[i];
        size_t j = i;
        while (j > 0 && blockEvents_[j - 1].offset > tmp.offset) { blockEvents_[j] = blockEvents_[j - 1]; --j; }
        blockEvents_[j] = tmp;
    }

    bool anySolo = false;
    for (auto& s : snap->slots) anySolo |= s.slot->solo.load(std::memory_order_relaxed);

    const double blockSeconds = frames / sr;
    const float peakDecay = (float)std::exp(-frames / (0.3 * sr));

    for (auto& s : snap->slots) {
        Slot& slot = *s.slot;
        InstrumentInstance* inst = s.inst.get();
        if (!inst) continue;
        const auto ts = std::chrono::steady_clock::now();

        if (inst != slot.audioInst) {
            // Hot swap: fade the previous instance out and re-strike held notes.
            if (s.fading && s.fading.get() == slot.audioInst) {
                slot.fadeInst = slot.audioInst;
                slot.fadePos = 0;
                slot.fadeLen = std::max(1, (int)(kCrossfadeSeconds * sr));
            } else {
                slot.fadeInst = nullptr;
            }
            slot.audioInst = inst;
            inst->applyPendingParams();
            for (int k = 0; k < 128; ++k)
                if (slot.heldVelocity[k]) inst->noteOn(slot.sentKey[k], slot.heldVelocity[k] / 127.0f);
        }
        inst->applyPendingParams();

        const bool audible = !slot.mute.load(std::memory_order_relaxed) &&
                             (!anySolo || slot.solo.load(std::memory_order_relaxed));
        float peak = 0.0f;

        int cursor = 0;
        for (const auto& ev : blockEvents_) {
            if (ev.targetSerial && ev.targetSerial != slot.serial) continue;
            if (ev.offset > cursor) {
                render(s, left, right, cursor, ev.offset, audible, peak);
                cursor = ev.offset;
            }
            deliver(slot, *inst, ev, ev.targetSerial != 0);
        }
        render(s, left, right, cursor, frames, audible, peak);

        slot.peak.store(std::max(peak, slot.peak.load(std::memory_order_relaxed) * peakDecay),
                        std::memory_order_relaxed);
        const double used = std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count();
        const float load = (float)(used / blockSeconds);
        slot.cpuLoad.store(slot.cpuLoad.load(std::memory_order_relaxed) * 0.95f + load * 0.05f,
                           std::memory_order_relaxed);
    }

    framePosition_ += (uint64_t)frames;
    epoch_.fetch_add(1, std::memory_order_release);
}

void Rack::render(SlotState& s, float* left, float* right, int start, int end, bool audible, float& peak) {
    Slot& slot = *s.slot;
    InstrumentInstance* inst = s.inst.get();
    const float gain = audible ? dbToGain(slot.gainDb.load(std::memory_order_relaxed)) : 0.0f;
    const float pan = slot.pan.load(std::memory_order_relaxed);
    const float gl = gain * (pan > 0.0f ? 1.0f - pan : 1.0f);
    const float gr = gain * (pan < 0.0f ? 1.0f + pan : 1.0f);

    InstrumentInstance* fade = slot.fadeInst;
    if (fade && (fade != s.fading.get() || slot.fadePos >= slot.fadeLen)) {
        fade = nullptr;  // the fading instance is no longer guaranteed alive
        slot.fadeInst = nullptr;
    }

    while (start < end) {
        const int n = std::min(InstrumentInstance::kMaxFrames, end - start);
        inst->process(n);
        const float* a = inst->outL();
        const float* b = inst->outR();
        for (int i = 0; i < n; ++i) {
            const float l = a[i] * gl, r = b[i] * gr;
            left[start + i] += l;
            right[start + i] += r;
            peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
        }
        if (fade) {
            fade->process(n);
            const float* fa = fade->outL();
            const float* fb = fade->outR();
            const float inv = 1.0f / (float)slot.fadeLen;
            for (int i = 0; i < n; ++i) {
                const float g = std::max(0.0f, 1.0f - (float)(slot.fadePos + i) * inv);
                left[start + i] += fa[i] * gl * g;
                right[start + i] += fb[i] * gr * g;
            }
            slot.fadePos += n;
            if (slot.fadePos >= slot.fadeLen) {
                fade = nullptr;
                slot.fadeInst = nullptr;
            }
        }
        start += n;
    }
}

void Rack::deliver(Slot& slot, InstrumentInstance& inst, const BlockEvent& ev, bool targeted) {
    const uint8_t type = ev.status & 0xF0;
    const int channel = (ev.status & 0x0F) + 1;
    if (!targeted) {
        const int want = slot.channel.load(std::memory_order_relaxed);
        if (want != 0 && want != channel) return;
    }
    const bool hostSustain = inst.meta().hostSustain;

    auto releaseKey = [&](int key, float vel) {
        inst.noteOff(slot.sentKey[key], vel);
        slot.heldVelocity[key] = 0;
        slot.sustained[key] = false;
    };

    switch (type) {
        case 0x90:
            if (ev.data2 > 0) {
                const int key = ev.data1 & 0x7f;
                if (!targeted && (key < slot.keyLow.load(std::memory_order_relaxed) ||
                                  key > slot.keyHigh.load(std::memory_order_relaxed)))
                    return;
                if (slot.heldVelocity[key]) releaseKey(key, 0.0f);  // retrigger
                const int tk = std::min(127, std::max(0, key + slot.transpose.load(std::memory_order_relaxed)));
                slot.heldVelocity[key] = ev.data2;
                slot.sentKey[key] = (uint8_t)tk;
                slot.sustained[key] = false;
                inst.noteOn(tk, ev.data2 / 127.0f);
                return;
            }
            [[fallthrough]];
        case 0x80: {
            const int key = ev.data1 & 0x7f;
            if (!slot.heldVelocity[key]) return;
            if (slot.pedal && hostSustain) {
                slot.sustained[key] = true;
                return;
            }
            releaseKey(key, type == 0x80 ? ev.data2 / 127.0f : 0.0f);
            return;
        }
        case 0xB0: {
            const int cc = ev.data1 & 0x7f;
            if (cc == 64 && hostSustain) {
                const bool down = ev.data2 >= 64;
                if (!down && slot.pedal)
                    for (int k = 0; k < 128; ++k)
                        if (slot.sustained[k]) releaseKey(k, 0.0f);
                slot.pedal = down;
                return;
            }
            if (cc == 120 || cc == 123) {
                inst.allNotesOff();
                slot.heldVelocity.fill(0);
                slot.sustained.fill(false);
                slot.pedal = false;
                return;
            }
            if (cc == 121) inst.pitchBend(0.0f);
            inst.controlChange(cc, ev.data2 / 127.0f);
            return;
        }
        case 0xE0: {
            const int raw = ((ev.data2 & 0x7f) << 7) | (ev.data1 & 0x7f);
            inst.pitchBend(std::max(-1.0f, (raw - 8192) / 8192.0f));
            return;
        }
        case 0xD0:
            inst.channelPressure((ev.data1 & 0x7f) / 127.0f);
            return;
        default:
            return;
    }
}

void Rack::inject(const Injected& ev) {
    std::lock_guard<std::mutex> lock(injectMutex_);
    const uint32_t w = injectWrite_.load(std::memory_order_relaxed);
    const uint32_t r = injectRead_.load(std::memory_order_acquire);
    if (w - r >= (uint32_t)kInjectCapacity) throw std::runtime_error("event queue full (is audio running?)");
    injectRing_[w % kInjectCapacity] = ev;
    injectWrite_.store(w + 1, std::memory_order_release);
}

// ----------------------------------------------------------------------------
// Control side

Rack::SlotState* Rack::findLocked(const std::string& id) {
    for (auto& s : slots_)
        if (s.slot->id == id) return &s;
    return nullptr;
}

int Rack::indexOfLocked(const std::string& id) const {
    for (size_t i = 0; i < slots_.size(); ++i)
        if (slots_[i].slot->id == id) return (int)i;
    return -1;
}

std::string Rack::uniqueIdLocked(const std::string& base) const {
    std::string id = base;
    for (int n = 2; indexOfLocked(id) >= 0; ++n) id = base + "-" + std::to_string(n);
    return id;
}

void Rack::tick() {
    std::lock_guard<std::mutex> lock(mutex_);
    collectGarbageLocked();

    bool changed = false;
    const double now = nowSeconds();
    for (auto& s : slots_) {
        if (s.fading && now > s.slot->fadingUntil) {
            s.fading.reset();
            changed = true;
        }
    }
    if (changed) publishLocked();
}

// A broken custom UI never blocks loading (the editor falls back to the default panel), so say why here.
static void logUiProblems(Log& log, const std::string& slotId, const InstrumentModule& module) {
    if (module.ui().empty() && module.ui().problems.empty()) return;
    const json report = checkUi(module.bytes(), module.ui(), module.meta());
    for (const auto& e : report.value("errors", json::array()))
        log.log("warn", slotId, "custom UI: " + e.get<std::string>());
}

void Rack::reloadLocked(SlotState& s, std::shared_ptr<const InstrumentModule> module) {
    const auto& newParams = module->meta().params;
    std::vector<float> values(newParams.size());
    for (size_t i = 0; i < newParams.size(); ++i) {
        values[i] = newParams[i].def;
        if (s.inst && s.inst->meta().id == module->meta().id) {
            int old = s.inst->meta().paramIndexById(newParams[i].id);
            if (old >= 0) values[i] = s.inst->paramValue(old);
        } else if (!s.inst) {  // the file was unreadable: values saved in the project
            auto it = s.slot->savedParams.find(std::to_string(newParams[i].id));
            if (it != s.slot->savedParams.end() && it->is_number()) values[i] = newParams[i].clamp(it->get<float>());
        }
    }
    auto inst = InstrumentInstance::create(module, sampleRate_.load(), values, &log_, s.slot->id);
    if (s.inst && s.slot->name == s.inst->meta().name) s.slot->name = module->meta().name;
    s.fading = (s.inst && !s.inst->crashed()) ? s.inst : nullptr;
    s.inst = std::shared_ptr<InstrumentInstance>(inst.release());
    s.slot->fadingUntil = nowSeconds() + kFadeHoldSeconds;
    s.slot->savedParams = json::object();
    s.slot->sha256 = module->hash();
    logUiProblems(log_, s.slot->id, *module);
}

void Rack::applySettingsLocked(Slot& slot, const json& p) {
    if (p.contains("name") && p["name"].is_string() && !p["name"].get<std::string>().empty())
        slot.name = p["name"].get<std::string>();
    if (p.contains("channel")) slot.channel = jsonInt(p, "channel", 0, 16);
    if (p.contains("key_low")) slot.keyLow = jsonInt(p, "key_low", 0, 127);
    if (p.contains("key_high")) slot.keyHigh = jsonInt(p, "key_high", 0, 127);
    if (p.contains("transpose")) slot.transpose = jsonInt(p, "transpose", -48, 48);
    if (p.contains("gain_db")) slot.gainDb = jsonFloat(p, "gain_db", -80.0f, 24.0f);
    if (p.contains("pan")) slot.pan = jsonFloat(p, "pan", -1.0f, 1.0f);
    if (p.contains("mute")) slot.mute = p["mute"].get<bool>();
    if (p.contains("solo")) slot.solo = p["solo"].get<bool>();
}

void Rack::applyParamsLocked(SlotState& s, const json& params) {
    if (!s.inst) throw std::runtime_error("slot has no instrument");
    const auto& meta = s.inst->meta();
    auto resolve = [&](const std::string& key) -> int {
        int idx = meta.paramIndexByName(key);
        if (idx >= 0) return idx;
        try {
            size_t used = 0;
            int id = std::stoi(key, &used);
            if (used == key.size()) idx = meta.paramIndexById(id);
        } catch (...) {
        }
        if (idx < 0) {
            std::string names;
            for (auto& p : meta.params) names += (names.empty() ? "" : ", ") + p.name + " (" + std::to_string(p.id) + ")";
            throw std::runtime_error("unknown param '" + key + "'. available: " + (names.empty() ? "none" : names));
        }
        return idx;
    };
    auto set = [&](const std::string& key, const json& value) {
        if (!value.is_number()) throw std::runtime_error("param '" + key + "' value must be a number");
        s.inst->setParam(resolve(key), value.get<float>());
    };
    if (params.is_object()) {
        for (auto it = params.begin(); it != params.end(); ++it) set(it.key(), it.value());
    } else if (params.is_array()) {
        for (auto& e : params) {
            const json& k = e.at("param");
            set(k.is_string() ? k.get<std::string>() : std::to_string(k.get<int>()), e.at("value"));
        }
    } else {
        throw std::runtime_error("'params' must be an object {name_or_id: value}");
    }
}

json Rack::slotInfoLocked(const SlotState& s, bool detailed) const {
    const Slot& slot = *s.slot;
    json j;
    j["id"] = slot.id;
    j["index"] = indexOfLocked(slot.id);
    j["name"] = slot.name;
    j["channel"] = slot.channel.load();
    j["key_low"] = slot.keyLow.load();
    j["key_high"] = slot.keyHigh.load();
    j["transpose"] = slot.transpose.load();
    j["gain_db"] = num(slot.gainDb.load());
    j["pan"] = num(slot.pan.load());
    j["mute"] = slot.mute.load();
    j["solo"] = slot.solo.load();
    j["cpu"] = std::round(slot.cpuLoad.load() * 1000.0) / 1000.0;
    j["peak_db"] = std::round(gainToDb(slot.peak.load()) * 10.0) / 10.0;
    j["source"] = {{"path", slot.sourcePath}};
    std::string status = "ok", error = slot.lastError;
    if (s.inst) {
        const auto& m = s.inst->meta();
        j["instrument"] = {{"id", m.id}, {"name", m.name}, {"version", m.version},
                           {"author", m.author}, {"hash", s.inst->module().hash()},  // SHA-256
                           {"size", s.inst->module().bytes().size()}};
        if (detailed) {
            j["instrument"]["description"] = m.description;
            j["instrument"]["tags"] = m.tags;
            j["instrument"]["outputs"] = m.outputs;
            j["instrument"]["host_sustain"] = m.hostSustain;
            const auto& ui = s.inst->module().ui();
            j["instrument"]["ui"] = {{"kind", ui.kind()}, {"files", ui.files.size()}, {"bytes", ui.totalBytes}};
        }
        if (s.inst->crashed()) {
            status = "crashed";
            error = s.inst->crashReason();
        }
        if (s.inst->nanCount()) j["nan_samples"] = s.inst->nanCount();
        json params = json::array();
        for (size_t i = 0; i < m.params.size(); ++i) {
            const auto& p = m.params[i];
            json pj = {{"index", i}, {"id", p.id}, {"name", p.name}, {"value", num(s.inst->paramValue((int)i))}};
            if (detailed) {
                pj["min"] = num(p.min);
                pj["max"] = num(p.max);
                pj["default"] = num(p.def);
                if (!p.unit.empty()) pj["unit"] = p.unit;
                if (p.scale != "linear") pj["scale"] = p.scale;
                if (p.steps) pj["steps"] = p.steps;
                if (!p.labels.empty()) pj["labels"] = p.labels;
            }
            params.push_back(pj);
        }
        j["params"] = params;
    } else {
        status = error.empty() ? "empty" : "error";  // "error": the file is missing or not a valid instrument
    }
    j["status"] = status;
    if (!error.empty()) j["error"] = error;
    return j;
}

json Rack::rackInfoLocked() const {
    json slots = json::array();
    for (const auto& s : slots_) slots.push_back(slotInfoLocked(s, false));
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return {{"name", name_},
            {"version", kVersion},
            {"aci_abi", kAciAbiVersion},
            {"sample_rate", sampleRate_.load()},
            {"audio_running", now - lastProcessTime_.load() < 1.0},
            {"max_slots", kMaxSlots},
            {"slots", slots}};
}

json Rack::deployLocked(const json& p) {
    // Slots always refer to a file: projects store the path and read it again when they are opened.
    if (!p.contains("path") || !p["path"].is_string())
        throw std::runtime_error("slot.deploy needs 'path': the instrument's .wasm file on the rack's machine "
                                 "(wasm_base64 is accepted by instrument.validate only)");
    const std::string path = p["path"].get<std::string>();
    auto module = InstrumentModule::load(readInstrumentFile(path));

    std::string id = p.contains("slot_id") && p["slot_id"].is_string() ? p["slot_id"].get<std::string>() : "";
    SlotState* s = id.empty() ? nullptr : findLocked(id);
    const bool reloaded = s != nullptr;
    if (s) {
        reloadLocked(*s, module);
    } else {
        if ((int)slots_.size() >= kMaxSlots)
            throw std::runtime_error("rack is full (" + std::to_string(kMaxSlots) + " slots)");
        id = uniqueIdLocked(slugify(id.empty() ? module->meta().name : id));
        auto slot = std::make_shared<Slot>(nextSerial_++, id);
        slot->name = module->meta().name;
        auto inst = InstrumentInstance::create(module, sampleRate_.load(), {}, &log_, id);
        slots_.push_back({slot, std::shared_ptr<InstrumentInstance>(inst.release()), nullptr});
        s = &slots_.back();
        logUiProblems(log_, id, *module);
    }

    Slot& slot = *s->slot;
    slot.sourcePath = path;
    slot.sha256 = module->hash();  // deploying is how a file (or a changed file) becomes trusted
    applySettingsLocked(slot, p);
    if (p.contains("params")) applyParamsLocked(*s, p["params"]);
    slot.lastError.clear();
    publishLocked();

    log_.log("info", slot.id, std::string(reloaded ? "reloaded " : "deployed ") + module->meta().name + " " +
                                  module->meta().version + " from " + path + " (sha256 " + module->hash().substr(0, 12) + "...)");
    json info = slotInfoLocked(*s, true);
    info["reloaded"] = reloaded;
    return info;
}

json Rack::handleRpc(const json& request) {
    try {
        if (!request.is_object() || !request.contains("method"))
            throw std::runtime_error("request must be {\"method\": ..., \"params\": {...}}");
        const std::string method = request["method"].get<std::string>();
        const json p = request.value("params", json::object());
        auto slotId = [&]() -> std::string {
            if (!p.contains("slot_id") || !p["slot_id"].is_string()) throw std::runtime_error("'slot_id' is required");
            return p["slot_id"].get<std::string>();
        };

        if (method == "instrument.validate") {
            // Pure function of the binary: no rack lock needed.
            std::vector<uint8_t> bytes;
            if (p.contains("wasm_base64")) bytes = base64Decode(p["wasm_base64"].get<std::string>());
            else if (p.contains("path")) bytes = readInstrumentFile(p["path"].get<std::string>());
            else throw std::runtime_error("instrument.validate needs 'wasm_base64' or 'path'");
            ValidateOptions opt;
            opt.sampleRate = sampleRate_.load();
            if (p.contains("keys")) {
                opt.keys.clear();
                for (auto& k : p["keys"]) opt.keys.push_back(parseKey(k));
            }
            if (p.contains("seconds")) opt.seconds = jsonFloat(p, "seconds", 0.1f, 30.0f);
            opt.wavPath = p.value("wav_path", std::string());
            opt.paramSweep = p.value("param_sweep", true);
            return {{"result", validateInstrument(bytes, opt)}};
        }

        std::unique_lock<std::mutex> lock(mutex_);
        auto requireSlot = [&]() -> SlotState& {
            std::string id = slotId();
            SlotState* s = findLocked(id);
            if (!s) {
                std::string ids;
                for (auto& x : slots_) ids += (ids.empty() ? "" : ", ") + x.slot->id;
                throw std::runtime_error("no slot '" + id + "'. slots: " + (ids.empty() ? "(none)" : ids));
            }
            return *s;
        };

        json result;
        if (method == "rack.info") {
            result = rackInfoLocked();
            if (p.value("detailed", false)) {
                result["slots"] = json::array();
                for (auto& s : slots_) result["slots"].push_back(slotInfoLocked(s, true));
            }
        } else if (method == "rack.set_name") {
            name_ = p.at("name").get<std::string>();
            ++changeCounter_;
            result = {{"name", name_}};
        } else if (method == "rack.panic") {
            lock.unlock();
            std::vector<uint32_t> serials;
            {
                std::lock_guard<std::mutex> l(mutex_);
                for (auto& s : slots_) serials.push_back(s.slot->serial);
            }
            for (uint32_t serial : serials) inject({0xB0, 123, 0, serial, 0});
            result = {{"ok", true}};
        } else if (method == "rack.clear") {
            slots_.clear();
            publishLocked();
            log_.log("info", "", "rack cleared");
            result = {{"ok", true}};
        } else if (method == "rack.logs") {
            uint64_t since = p.value("since", (uint64_t)0);
            lock.unlock();
            json lines = json::array();
            for (const auto& l : log_.since(since))
                lines.push_back({{"seq", l.seq}, {"time", l.time}, {"level", l.level}, {"slot", l.slot}, {"message", l.message}});
            result = {{"lines", lines}};
        } else if (method == "slot.deploy") {
            result = deployLocked(p);
        } else if (method == "slot.remove") {
            std::string id = slotId();
            int idx = indexOfLocked(id);
            if (idx < 0) throw std::runtime_error("no slot '" + id + "'");
            slots_.erase(slots_.begin() + idx);
            publishLocked();
            log_.log("info", id, "removed");
            result = {{"removed", id}};
        } else if (method == "slot.update") {
            SlotState& s = requireSlot();
            applySettingsLocked(*s.slot, p);
            if (p.contains("params")) applyParamsLocked(s, p["params"]);
            ++changeCounter_;
            result = slotInfoLocked(s, false);
        } else if (method == "slot.set_params") {
            SlotState& s = requireSlot();
            applyParamsLocked(s, p.at("params"));
            result = slotInfoLocked(s, false);
        } else if (method == "slot.info") {
            SlotState& s = requireSlot();
            result = slotInfoLocked(s, true);
            result["meta_json"] = s.inst ? s.inst->meta().json : "";
        } else if (method == "slot.move") {
            std::string id = slotId();
            int from = indexOfLocked(id);
            if (from < 0) throw std::runtime_error("no slot '" + id + "'");
            int to = jsonInt(p, "index", 0, (int)slots_.size() - 1);
            SlotState moved = slots_[from];
            slots_.erase(slots_.begin() + from);
            slots_.insert(slots_.begin() + to, moved);
            publishLocked();
            result = {{"id", id}, {"index", to}};
        } else if (method == "slot.export") {
            SlotState& s = requireSlot();
            if (!s.inst) throw std::runtime_error("slot is empty");
            result = {{"slot_id", s.slot->id},
                      {"filename", slugify(s.inst->meta().name) + ".aci.wasm"},
                      {"wasm_base64", base64Encode(s.inst->module().bytes())}};
        } else if (method == "slot.note") {
            uint32_t target = 0;
            int channel = 1;
            if (p.contains("slot_id")) target = requireSlot().slot->serial;
            else if (p.contains("channel")) channel = jsonInt(p, "channel", 1, 16);
            lock.unlock();
            std::vector<int> keys;
            if (p.contains("keys")) for (auto& k : p["keys"]) keys.push_back(parseKey(k));
            if (p.contains("key")) keys.push_back(parseKey(p["key"]));
            if (keys.empty()) keys.push_back(60);
            const int velocity = p.contains("velocity") ? jsonInt(p, "velocity", 1, 127) : 100;
            const double durationMs = p.contains("duration_ms") ? jsonFloat(p, "duration_ms", 1.0f, 60000.0f) : 500.0;
            const uint32_t offFrames = (uint32_t)(durationMs * 0.001 * sampleRate_.load());
            const uint8_t ch = (uint8_t)(channel - 1);
            for (int k : keys) inject({(uint8_t)(0x90 | ch), (uint8_t)k, (uint8_t)velocity, target, 0});
            for (int k : keys) inject({(uint8_t)(0x80 | ch), (uint8_t)k, 0, target, offFrames});
            result = {{"keys", keys}, {"velocity", velocity}, {"duration_ms", durationMs}};
        } else {
            throw std::runtime_error("unknown method '" + method + "'");
        }
        return {{"result", result}};
    } catch (const std::exception& e) {
        return {{"error", e.what()}};
    }
}

// ----------------------------------------------------------------------------
// State

std::string Rack::saveState() {
    std::lock_guard<std::mutex> lock(mutex_);
    json slots = json::array();
    for (const auto& s : slots_) {
        const Slot& slot = *s.slot;
        json params = slot.savedParams;  // a slot whose file is unreadable keeps what the project had
        if (s.inst) {
            params = json::object();
            const auto& meta = s.inst->meta();
            for (size_t i = 0; i < meta.params.size(); ++i)
                params[std::to_string(meta.params[i].id)] = num(s.inst->paramValue((int)i));
        }
        slots.push_back({{"id", slot.id},
                         {"name", slot.name},
                         {"path", slot.sourcePath},
                         {"sha256", slot.sha256},
                         {"channel", slot.channel.load()},
                         {"key_low", slot.keyLow.load()},
                         {"key_high", slot.keyHigh.load()},
                         {"transpose", slot.transpose.load()},
                         {"gain_db", num(slot.gainDb.load())},
                         {"pan", num(slot.pan.load())},
                         {"mute", slot.mute.load()},
                         {"solo", slot.solo.load()},
                         {"params", params}});
    }
    return json{{"format", "aicontact-rack"}, {"version", 2}, {"name", name_}, {"slots", slots}}.dump();
}

void Rack::loadState(const std::string& state) {
    json j = json::parse(state);
    if (j.value("format", "") != "aicontact-rack") throw std::runtime_error("not an AI Contact state");

    std::lock_guard<std::mutex> lock(mutex_);
    name_ = j.value("name", std::string("AI Contact"));
    slots_.clear();
    for (const auto& sj : j.value("slots", json::array())) {
        auto slot = std::make_shared<Slot>(nextSerial_++, uniqueIdLocked(sj.value("id", std::string("inst"))));
        slot->name = sj.value("name", slot->id);
        slot->sourcePath = sj.value("path", std::string());
        slot->sha256 = sj.value("sha256", std::string());
        if (sj.contains("params") && sj["params"].is_object()) slot->savedParams = sj["params"];
        json settings = sj;
        settings.erase("params");
        applySettingsLocked(*slot, settings);
        SlotState s{slot, nullptr, nullptr};
        // The instrument is read from its file every time the project is opened. If that fails the slot
        // stays silent with the reason shown: playing something else would hide the problem. The file must
        // still be what was deployed: a file replaced behind the project's back (by another tool, a sync
        // client or an attacker) is refused before any of its code runs.
        try {
            if (slot->sourcePath.empty()) throw std::runtime_error("no instrument file recorded for this slot");
            std::vector<uint8_t> bytes = readInstrumentFile(slot->sourcePath);
            const std::string actual = sha256Hex(bytes);
            if (slot->sha256.empty())
                throw std::runtime_error("no SHA-256 recorded for " + slot->sourcePath + "; not loaded. Deploy the file "
                                         "to this slot again if you trust it");
            if (actual != slot->sha256)
                throw std::runtime_error(slot->sourcePath + " changed since it was deployed (SHA-256 " +
                                         slot->sha256.substr(0, 12) + "... expected, " + actual.substr(0, 12) +
                                         "... found); not loaded. Deploy it to this slot again if the change is yours");
            auto module = InstrumentModule::load(std::move(bytes));
            reloadLocked(s, module);
            s.fading.reset();
        } catch (const std::exception& e) {
            slot->lastError = e.what();
            std::string why = e.what();
            if (why.find(slot->sourcePath) == std::string::npos) why = slot->sourcePath + ": " + why;
            log_.log("error", slot->id, "could not load the instrument: " + why + " (deploy a file to this slot to fix it)");
        }
        slots_.push_back(std::move(s));
    }
    publishLocked();
}

std::shared_ptr<const InstrumentModule> Rack::slotModule(const std::string& slotId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& s : slots_)
        if (s.slot->id == slotId && s.inst) return s.inst->modulePtr();
    return nullptr;
}

int Rack::numSlots() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (int)slots_.size();
}

float Rack::paramNormalized(int slotIndex, int paramIndex) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slotIndex < 0 || (size_t)slotIndex >= slots_.size() || !slots_[slotIndex].inst) return -1.0f;
    const auto& inst = *slots_[slotIndex].inst;
    if (paramIndex < 0 || (size_t)paramIndex >= inst.meta().params.size()) return -1.0f;
    return inst.meta().params[paramIndex].toNormalized(inst.paramValue(paramIndex));
}

std::string Rack::paramLabel(int slotIndex, int paramIndex) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slotIndex < 0 || (size_t)slotIndex >= slots_.size() || !slots_[slotIndex].inst) return {};
    const auto& inst = *slots_[slotIndex].inst;
    if (paramIndex < 0 || (size_t)paramIndex >= inst.meta().params.size()) return {};
    return slots_[slotIndex].slot->id + ": " + inst.meta().params[paramIndex].name;
}

}  // namespace aicontact
