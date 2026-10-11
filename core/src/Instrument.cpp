#include "aicontact/Instrument.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "aicontact/Log.h"
#include "aicontact/Util.h"
#include "aicontact/WasmInfo.h"
#include "wasm3.h"

namespace aicontact {

using json = nlohmann::json;

namespace {

constexpr uint32_t kWasm3StackBytes = 256 * 1024;
constexpr uint64_t kMaxMemoryBytes = 256ull * 1024 * 1024;
// Gas budgets (wasm3 gas units). They exist to stop a runaway module (e.g. an
// infinite loop) from freezing the audio thread, not to police CPU usage.
// Calibrated so that a hung aci_process(256) traps after ~20 ms of wall time,
// while a patch running 64 sine oscillators per sample (41 gas/frame) uses
// about 1/20 of it.
// AICONTACT_GAS_PER_FRAME overrides the per-frame budget (in gas).
uint64_t gasPerFrame() {
    static const uint64_t v = [] {
        const char* env = std::getenv("AICONTACT_GAS_PER_FRAME");
        uint64_t gas = env ? std::strtoull(env, nullptr, 10) : 0;
        return (gas ? gas : 800) * M3_GAS_UNITS_PER_GAS;
    }();
    return v;
}
constexpr uint64_t kGasEvent = 50000ull * M3_GAS_UNITS_PER_GAS;  // ~5 ms
constexpr uint64_t kGasInit = 50ull * 1000 * 1000 * M3_GAS_UNITS_PER_GAS;
constexpr float kOutputLimit = 4.0f;  // +12 dBFS safety clamp

m3ApiRawFunction(hostAciLog) {
    m3ApiGetArgMem(const char*, ptr);
    m3ApiGetArg(int32_t, len);
    if (len < 0) len = 0;
    if (len > 4096) len = 4096;
    m3ApiCheckMem(ptr, len);
    if (_ctx && _ctx->userdata)
        static_cast<InstrumentInstance*>(_ctx->userdata)->moduleLog(ptr, (size_t)len);
    m3ApiSuccess();
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

InstrumentMeta parseMeta(const std::string& text) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("aci_meta() is not valid JSON: ") + e.what());
    }
    if (!j.is_object()) throw std::runtime_error("aci_meta() must return a JSON object");

    InstrumentMeta m;
    m.json = text;
    m.aci = j.value("aci", 0);
    if (m.aci != kAciAbiVersion)
        throw std::runtime_error("metadata 'aci' must be " + std::to_string(kAciAbiVersion));
    auto str = [&](const char* key) -> std::string {
        auto it = j.find(key);
        return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
    };
    m.id = str("id");
    m.name = str("name");
    if (m.id.empty()) throw std::runtime_error("metadata is missing 'id'");
    if (m.name.empty()) throw std::runtime_error("metadata is missing 'name'");
    m.version = str("version");
    m.author = str("author");
    m.description = str("description");
    if (j.contains("tags") && j["tags"].is_array())
        for (auto& t : j["tags"]) if (t.is_string()) m.tags.push_back(t.get<std::string>());
    m.outputs = j.value("outputs", 2);
    if (m.outputs != 1 && m.outputs != 2) throw std::runtime_error("metadata 'outputs' must be 1 or 2");
    m.polyphony = j.value("polyphony", 0);
    m.hostSustain = j.value("host_sustain", true);

    if (j.contains("params")) {
        if (!j["params"].is_array()) throw std::runtime_error("metadata 'params' must be an array");
        for (auto& p : j["params"]) {
            if (!p.is_object()) throw std::runtime_error("each param must be an object");
            ParamInfo pi;
            if (!p.contains("id") || !p["id"].is_number_integer())
                throw std::runtime_error("param is missing integer 'id'");
            pi.id = p["id"].get<int>();
            if (pi.id < 0) throw std::runtime_error("param id must be >= 0");
            pi.name = p.value("name", std::string("Param ") + std::to_string(pi.id));
            pi.min = p.value("min", 0.0f);
            pi.max = p.value("max", 1.0f);
            if (!(pi.max > pi.min))
                throw std::runtime_error("param '" + pi.name + "': max must be greater than min");
            pi.def = pi.clamp(p.value("default", pi.min));
            pi.unit = p.value("unit", std::string());
            pi.scale = p.value("scale", std::string("linear"));
            if (pi.scale == "log" && pi.min <= 0) pi.scale = "linear";
            pi.steps = p.value("steps", 0);
            if (p.contains("labels") && p["labels"].is_array())
                for (auto& l : p["labels"]) if (l.is_string()) pi.labels.push_back(l.get<std::string>());
            if (m.paramIndexById(pi.id) >= 0)
                throw std::runtime_error("duplicate param id " + std::to_string(pi.id));
            m.params.push_back(std::move(pi));
        }
    }
    return m;
}

}  // namespace

// ----------------------------------------------------------------------------

float ParamInfo::clamp(float v) const {
    if (!std::isfinite(v)) return def;
    v = std::min(max, std::max(min, v));
    if (steps > 1) {
        float step = (max - min) / float(steps - 1);
        v = min + std::round((v - min) / step) * step;
    }
    return v;
}

float ParamInfo::toNormalized(float v) const {
    v = clamp(v);
    if (scale == "log") return std::log(v / min) / std::log(max / min);
    return (v - min) / (max - min);
}

float ParamInfo::fromNormalized(float n) const {
    n = std::min(1.0f, std::max(0.0f, n));
    if (scale == "log") return clamp(min * std::pow(max / min, n));
    return clamp(min + n * (max - min));
}

int InstrumentMeta::paramIndexById(int pid) const {
    for (size_t i = 0; i < params.size(); ++i)
        if (params[i].id == pid) return (int)i;
    return -1;
}

int InstrumentMeta::paramIndexByName(const std::string& n) const {
    std::string ln = lower(n);
    for (size_t i = 0; i < params.size(); ++i)
        if (lower(params[i].name) == ln) return (int)i;
    return -1;
}

// ----------------------------------------------------------------------------

namespace {

// RAII wrapper around one wasm3 environment/runtime/module triple used while
// instantiating; ownership moves into InstrumentInstance on success.
struct Instantiated {
    M3Environment* env = nullptr;
    M3Runtime* runtime = nullptr;
    M3Module* module = nullptr;
    ~Instantiated() {
        if (runtime) m3_FreeRuntime(runtime);
        if (env) m3_FreeEnvironment(env);
    }
};

void check(M3Result r, const char* what) {
    if (r != m3Err_none) throw std::runtime_error(std::string(what) + ": " + r);
}

M3Function* findOptional(M3Module* module, const char* name) {
    M3Function* fn = nullptr;
    if (m3_FindFunctionIn(&fn, module, name) != m3Err_none) return nullptr;
    return fn;
}

M3Function* findRequired(M3Module* module, const char* name) {
    M3Function* fn = findOptional(module, name);
    if (!fn) throw std::runtime_error(std::string("missing export ") + name);
    return fn;
}

// Instantiates bytes into `out`, linking env.aci_log to `logTarget`.
void instantiate(const std::vector<uint8_t>& bytes, void* logTarget, Instantiated& out) {
    out.env = m3_NewEnvironment();
    if (!out.env) throw std::runtime_error("wasm3: out of memory");
    out.runtime = m3_NewRuntime(out.env, kWasm3StackBytes, nullptr);
    if (!out.runtime) throw std::runtime_error("wasm3: out of memory");
    m3_SetResourceLimit(out.runtime, c_m3Limit_MemoryBytes, kMaxMemoryBytes);

    M3Module* module = nullptr;
    check(m3_ParseModule(out.env, &module, bytes.data(), (uint32_t)bytes.size()), "parse");
    M3Result r = m3_LoadModule(out.runtime, module);  // takes ownership either way
    check(r, "load");
    out.module = module;

    r = m3_LinkRawFunctionEx(module, "env", "aci_log", "v(ii)", &hostAciLog, logTarget);
    if (r != m3Err_none && r != m3Err_functionLookupFailed) check(r, "link env.aci_log");

    // Arm gas metering before compiling so every body is instrumented.
    check(m3_SetResourceLimit(out.runtime, c_m3Limit_GasUnits, kGasInit), "arm gas");
    check(m3_CompileModule(module), "compile");
    check(m3_RunStart(module), "start function");
}

uint32_t callI32(M3Runtime* rt, M3Function* fn, uint32_t argc, const void** argv) {
    m3_SetResourceLimit(rt, c_m3Limit_GasUnits, kGasInit);
    check(m3_Call(fn, argc, argv), m3_GetFunctionName(fn));
    uint32_t ret = 0;
    const void* rets[] = {&ret};
    check(m3_GetResults(fn, 1, rets), m3_GetFunctionName(fn));
    return ret;
}

std::string readMetaString(M3Runtime* rt, M3Module* module) {
    uint32_t version = callI32(rt, findRequired(module, "aci_abi_version"), 0, nullptr);
    if ((int)version != kAciAbiVersion)
        throw std::runtime_error("aci_abi_version() returned " + std::to_string(version) +
                                 ", this host supports " + std::to_string(kAciAbiVersion));
    uint32_t ptr = callI32(rt, findRequired(module, "aci_meta"), 0, nullptr);
    size_t memSize = 0;
    uint8_t* mem = m3_GetMemory(module, &memSize, 0);
    if (!mem || ptr >= memSize) throw std::runtime_error("aci_meta() returned an out-of-bounds pointer");
    size_t maxLen = std::min<size_t>(memSize - ptr, 64 * 1024);
    const char* s = (const char*)mem + ptr;
    size_t len = strnlen(s, maxLen);
    if (len == maxLen) throw std::runtime_error("aci_meta() string is not NUL-terminated within 64 KiB");
    return std::string(s, len);
}

}  // namespace

std::shared_ptr<const InstrumentModule> InstrumentModule::load(std::vector<uint8_t> bytes) {
    auto m = std::shared_ptr<InstrumentModule>(new InstrumentModule());
    m->hash_ = sha256Hex(bytes);
    m->bytes_ = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));

    WasmInfo info = WasmInfo::parse(*m->bytes_);
    auto problems = checkAciStructure(info);
    if (!problems.empty()) {
        std::string msg = "not a valid ACI module:";
        for (auto& p : problems) msg += "\n  - " + p;
        throw std::runtime_error(msg);
    }
    m->ui_ = UiBundle::fromWasm(info);

    {
        Instantiated tmp;
        instantiate(*m->bytes_, nullptr, tmp);
        m->meta_ = parseMeta(readMetaString(tmp.runtime, tmp.module));
    }
    // Make sure it can actually be initialised and run before anyone uses it.
    auto probe = InstrumentInstance::create(m, 48000.0, {}, nullptr, "");
    probe->noteOn(60, 0.8f);
    probe->process(InstrumentInstance::kMaxFrames);
    probe->noteOff(60, 0.0f);
    probe->process(InstrumentInstance::kMaxFrames);
    if (probe->crashed()) throw std::runtime_error("module trapped during test run: " + probe->crashReason());
    return m;
}

// ----------------------------------------------------------------------------

std::unique_ptr<InstrumentInstance> InstrumentInstance::create(std::shared_ptr<const InstrumentModule> module,
                                                               double sampleRate,
                                                               const std::vector<float>& paramValues,
                                                               Log* log, const std::string& slotId) {
    std::unique_ptr<InstrumentInstance> self(new InstrumentInstance());
    self->module_ = module;
    self->bytes_ = module->bytes_;
    self->log_ = log;
    std::snprintf(self->slotId_, sizeof(self->slotId_), "%s", slotId.c_str());

    Instantiated inst;
    instantiate(*self->bytes_, self.get(), inst);
    M3Module* m = inst.module;

    M3Function* fnInit = findRequired(m, "aci_init");
    M3Function* fnOutput = findRequired(m, "aci_output");
    self->fnProcess_ = findRequired(m, "aci_process");
    self->fnNoteOn_ = findRequired(m, "aci_note_on");
    self->fnNoteOff_ = findRequired(m, "aci_note_off");
    self->fnSetParam_ = findOptional(m, "aci_set_param");
    self->fnPitchBend_ = findOptional(m, "aci_pitch_bend");
    self->fnControlChange_ = findOptional(m, "aci_control_change");
    self->fnChannelPressure_ = findOptional(m, "aci_channel_pressure");
    self->fnAllNotesOff_ = findOptional(m, "aci_all_notes_off");

    float sr = (float)sampleRate;
    int32_t maxFrames = kMaxFrames;
    const void* initArgs[] = {&sr, &maxFrames};
    uint32_t rc = callI32(inst.runtime, fnInit, 2, initArgs);
    if (rc != 0) throw std::runtime_error("aci_init() returned error code " + std::to_string((int32_t)rc));

    const int outputs = module->meta().outputs;
    for (int32_t ch = 0; ch < outputs; ++ch) {
        const void* a[] = {&ch};
        self->outOffset_[ch] = callI32(inst.runtime, fnOutput, 1, a);
    }
    if (outputs == 1) self->outOffset_[1] = self->outOffset_[0];
    size_t memSize = 0;
    m3_GetMemory(m, &memSize, 0);
    for (uint32_t off : self->outOffset_)
        if ((uint64_t)off + kMaxFrames * sizeof(float) > memSize || (off & 3))
            throw std::runtime_error("aci_output() returned an invalid buffer pointer");

    const auto& params = module->meta().params;
    self->numParams_ = params.size();
    self->paramTarget_.reset(new std::atomic<float>[params.size()]);
    self->paramApplied_.reset(new float[params.size()]);
    for (size_t i = 0; i < params.size(); ++i) {
        float v = params[i].clamp(i < paramValues.size() ? paramValues[i] : params[i].def);
        self->paramTarget_[i].store(v);
        self->paramApplied_[i] = v;
        if (self->fnSetParam_) {
            int32_t id = params[i].id;
            const void* a[] = {&id, &v};
            m3_SetResourceLimit(inst.runtime, c_m3Limit_GasUnits, kGasEvent);
            check(m3_Call(self->fnSetParam_, 2, a), "aci_set_param");
        }
    }

    self->env_ = inst.env;
    self->runtime_ = inst.runtime;
    self->wasmModule_ = inst.module;
    inst.env = nullptr;
    inst.runtime = nullptr;
    return self;
}

InstrumentInstance::~InstrumentInstance() {
    if (runtime_) m3_FreeRuntime(runtime_);
    if (env_) m3_FreeEnvironment(env_);
}

void InstrumentInstance::crash(const char* what) {
    if (crashed_.load(std::memory_order_relaxed)) return;
    std::snprintf(crashReason_, sizeof(crashReason_), "%s", what);
    crashed_.store(true, std::memory_order_release);
    if (log_) {
        char msg[300];
        int n = std::snprintf(msg, sizeof(msg), "instrument stopped: %s", what);
        log_->tryLog("error", slotId_, msg, (size_t)std::max(0, std::min(n, (int)sizeof(msg) - 1)));
    }
}

std::string InstrumentInstance::crashReason() const {
    return crashed() ? std::string(crashReason_) : std::string();
}

bool InstrumentInstance::call(M3Function* fn, uint32_t argc, const void** argv, uint64_t gas) {
    if (!fn || crashed_.load(std::memory_order_relaxed)) return false;
    m3_SetResourceLimit(runtime_, c_m3Limit_GasUnits, gas);
    M3Result r = m3_Call(fn, argc, argv);
    if (r != m3Err_none) {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "%s: %s", m3_GetFunctionName(fn), r);
        crash(buf);
        return false;
    }
    return true;
}

bool InstrumentInstance::process(int frames) {
    if (frames <= 0) return !crashed();
    frames = std::min(frames, kMaxFrames);
    int32_t n = frames;
    const void* a[] = {&n};
    if (!call(fnProcess_, 1, a, gasPerFrame() * (uint64_t)frames + kGasEvent)) {
        std::memset(outL_, 0, sizeof(outL_));
        std::memset(outR_, 0, sizeof(outR_));
        return false;
    }
    size_t memSize = 0;
    const uint8_t* mem = m3_GetMemory(wasmModule_, &memSize, 0);
    const float* src[2] = {(const float*)(mem + outOffset_[0]), (const float*)(mem + outOffset_[1])};
    float* dst[2] = {outL_, outR_};
    uint64_t bad = 0;
    for (int ch = 0; ch < 2; ++ch) {
        for (int i = 0; i < frames; ++i) {
            float v;
            std::memcpy(&v, src[ch] + i, sizeof(float));
            if (!std::isfinite(v)) { v = 0.0f; ++bad; }
            dst[ch][i] = std::min(kOutputLimit, std::max(-kOutputLimit, v));
        }
    }
    if (bad) {
        if (nanCount_.fetch_add(bad, std::memory_order_relaxed) == 0 && log_) {
            const char* msg = "module produced NaN/Inf samples (replaced with silence)";
            log_->tryLog("warn", slotId_, msg, std::strlen(msg));
        }
    }
    return true;
}

void InstrumentInstance::noteOn(int key, float velocity) {
    int32_t k = key;
    const void* a[] = {&k, &velocity};
    call(fnNoteOn_, 2, a, kGasEvent);
}

void InstrumentInstance::noteOff(int key, float velocity) {
    int32_t k = key;
    const void* a[] = {&k, &velocity};
    call(fnNoteOff_, 2, a, kGasEvent);
}

void InstrumentInstance::pitchBend(float value) {
    const void* a[] = {&value};
    call(fnPitchBend_, 1, a, kGasEvent);
}

void InstrumentInstance::controlChange(int cc, float value) {
    int32_t c = cc;
    const void* a[] = {&c, &value};
    call(fnControlChange_, 2, a, kGasEvent);
}

void InstrumentInstance::channelPressure(float value) {
    const void* a[] = {&value};
    call(fnChannelPressure_, 1, a, kGasEvent);
}

void InstrumentInstance::allNotesOff() {
    if (fnAllNotesOff_) {
        call(fnAllNotesOff_, 0, nullptr, kGasEvent);
    } else {
        for (int k = 0; k < 128; ++k) noteOff(k, 0.0f);
    }
}

void InstrumentInstance::setParam(int index, float value) {
    if (index < 0 || (size_t)index >= numParams_) return;
    paramTarget_[index].store(module_->meta().params[index].clamp(value), std::memory_order_relaxed);
    paramsDirty_.store(true, std::memory_order_release);
}

float InstrumentInstance::paramValue(int index) const {
    if (index < 0 || (size_t)index >= numParams_) return 0.0f;
    return paramTarget_[index].load(std::memory_order_relaxed);
}

std::vector<float> InstrumentInstance::paramValues() const {
    std::vector<float> v(numParams_);
    for (size_t i = 0; i < numParams_; ++i) v[i] = paramTarget_[i].load(std::memory_order_relaxed);
    return v;
}

void InstrumentInstance::applyPendingParams() {
    if (!paramsDirty_.exchange(false, std::memory_order_acquire)) return;
    const auto& params = module_->meta().params;
    for (size_t i = 0; i < numParams_; ++i) {
        float v = paramTarget_[i].load(std::memory_order_relaxed);
        if (v == paramApplied_[i]) continue;
        paramApplied_[i] = v;
        if (fnSetParam_) {
            int32_t id = params[i].id;
            const void* a[] = {&id, &v};
            call(fnSetParam_, 2, a, kGasEvent);
        }
    }
}

void InstrumentInstance::moduleLog(const char* msg, size_t len) {
    if (!log_) return;
    // Rate limit: at most 20 lines per second per instance.
    double now = nowSeconds();
    if (now - logWindowStart_ > 1.0) { logWindowStart_ = now; logCount_ = 0; }
    if (++logCount_ > 20) return;
    log_->tryLog("module", slotId_, msg, len);
}

}  // namespace aicontact
