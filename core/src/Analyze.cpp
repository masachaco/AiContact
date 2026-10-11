#include "aicontact/Analyze.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <stdexcept>

#include "aicontact/Instrument.h"
#include "aicontact/Log.h"
#include "aicontact/Ui.h"

namespace aicontact {

using json = nlohmann::json;

namespace {

struct Render {
    std::vector<float> left, right;
    double cpuSeconds = 0;
    std::string crash;
};

// Plays `keys` at t=0 and releases them at `releaseAt` seconds.
Render render(InstrumentInstance& inst, const std::vector<int>& keys, double seconds, double releaseAt, double sr) {
    Render r;
    const int total = (int)(seconds * sr), releaseFrame = (int)(releaseAt * sr);
    r.left.reserve((size_t)total);
    r.right.reserve((size_t)total);
    bool released = false;
    const auto t0 = std::chrono::steady_clock::now();
    for (int k : keys) inst.noteOn(k, 100.0f / 127.0f);
    for (int pos = 0; pos < total;) {
        if (!released && pos >= releaseFrame) {
            for (int k : keys) inst.noteOff(k, 0.0f);
            released = true;
        }
        int n = std::min(InstrumentInstance::kMaxFrames, total - pos);
        if (!released) n = std::min(n, std::max(1, releaseFrame - pos));
        inst.applyPendingParams();
        inst.process(n);
        r.left.insert(r.left.end(), inst.outL(), inst.outL() + n);
        r.right.insert(r.right.end(), inst.outR(), inst.outR() + n);
        pos += n;
    }
    r.cpuSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (inst.crashed()) r.crash = inst.crashReason();
    return r;
}

double db(double x) { return x > 1e-7 ? std::round(200.0 * std::log10(x)) / 10.0 : -140.0; }

json analyze(const Render& r, bool withTail, uint64_t nanSamples) {
    const size_t n = r.left.size();
    double peak = 0, sum2 = 0, sum = 0;
    size_t clipped = 0;
    bool stereo = false;
    for (size_t i = 0; i < n; ++i) {
        for (float v : {r.left[i], r.right[i]}) {
            peak = std::max(peak, (double)std::fabs(v));
            sum2 += (double)v * v;
            sum += v;
            if (std::fabs(v) > 1.0f) ++clipped;
        }
        if (!stereo && std::fabs(r.left[i] - r.right[i]) > 1e-6f) stereo = true;
    }
    const double rms = std::sqrt(sum2 / std::max<size_t>(1, 2 * n));
    const double dc = sum / std::max<size_t>(1, 2 * n);
    json a = {{"peak_dbfs", db(peak)}, {"rms_dbfs", db(rms)}, {"dc_offset", std::round(dc * 1e5) / 1e5},
              {"nan_or_inf_samples", nanSamples}, {"clipped_samples", clipped}, {"stereo", stereo}};
    json warnings = json::array();
    double tail = -140.0;
    if (withTail) {
        double t2 = 0;
        const size_t start = n * 9 / 10;
        for (size_t i = start; i < n; ++i) t2 += (double)r.left[i] * r.left[i] + (double)r.right[i] * r.right[i];
        tail = db(std::sqrt(t2 / std::max<size_t>(1, 2 * (n - start))));
        a["tail_rms_dbfs"] = tail;
    }
    if (!r.crash.empty()) warnings.push_back("instrument trapped: " + r.crash);
    if (nanSamples) warnings.push_back("NaN/Inf samples: the host replaces them with silence");
    if (peak < 1e-4) warnings.push_back("output is silent");
    if (clipped) warnings.push_back("output exceeds 0 dBFS; lower the output gain (aim for peaks around -6 dBFS)");
    else if (peak > 1e-4 && db(peak) < -30) warnings.push_back("output is very quiet (peak below -30 dBFS)");
    if (std::fabs(dc) > 0.01) warnings.push_back("noticeable DC offset; consider a DC-blocking high-pass");
    if (withTail && tail > -60 && peak > 1e-4)
        warnings.push_back("sound does not decay after note-off (stuck voice or infinite release?)");
    a["warnings"] = warnings;
    return a;
}

}  // namespace

void writeWav(const std::string& path, const std::vector<float>& left, const std::vector<float>& right,
              int sampleRate) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    auto u32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
    auto u16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
    const size_t frames = std::min(left.size(), right.size());
    const uint32_t bytes = (uint32_t)(frames * 2 * sizeof(float));
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(3); u16(2); u32((uint32_t)sampleRate);
    u32((uint32_t)sampleRate * 8); u16(8); u16(32);
    f.write("data", 4); u32(bytes);
    for (size_t i = 0; i < frames; ++i) {
        f.write((const char*)&left[i], 4);
        f.write((const char*)&right[i], 4);
    }
}

json validateInstrument(const std::vector<uint8_t>& wasm, const ValidateOptions& opt) {
    json report = {{"ok", false}, {"size_bytes", wasm.size()}};
    Log log;
    try {
        auto module = InstrumentModule::load(wasm);
        const auto& meta = module->meta();
        report["meta"] = json::parse(meta.json);
        report["hash"] = module->hash();

        auto inst = InstrumentInstance::create(module, opt.sampleRate, {}, &log, "validate");
        Render r = render(*inst, opt.keys, opt.seconds, opt.seconds * 0.6, opt.sampleRate);
        json audio = analyze(r, true, inst->nanCount());
        audio["keys"] = opt.keys;
        report["audio"] = audio;
        // Same engine as the plugin, so this is the load the DAW will see for this many voices.
        report["cpu_percent"] = std::round(1000.0 * r.cpuSeconds / opt.seconds) / 10.0;
        if (!opt.wavPath.empty()) {
            writeWav(opt.wavPath, r.left, r.right, (int)opt.sampleRate);
            report["wav_path"] = opt.wavPath;
        }
        bool ok = r.crash.empty() && inst->nanCount() == 0;

        if (opt.paramSweep && !meta.params.empty()) {
            json problems = json::array();
            for (size_t i = 0; i < meta.params.size(); ++i) {
                const auto& p = meta.params[i];
                for (float value : {p.min, p.max}) {
                    std::vector<float> values(meta.params.size());
                    for (size_t k = 0; k < values.size(); ++k) values[k] = meta.params[k].def;
                    values[i] = value;
                    auto probe = InstrumentInstance::create(module, opt.sampleRate, values, &log, "validate");
                    Render pr = render(*probe, {60}, 0.5, 0.3, opt.sampleRate);
                    json pa = analyze(pr, false, probe->nanCount());
                    std::string bad;
                    for (const auto& w : pa["warnings"]) {
                        const std::string s = w.get<std::string>();
                        if (s.find("NaN") != std::string::npos || s.find("exceeds") != std::string::npos ||
                            s.find("trapped") != std::string::npos)
                            bad += (bad.empty() ? "" : "; ") + s;
                    }
                    if (!bad.empty()) {
                        char head[160];
                        std::snprintf(head, sizeof(head), "%s=%g: ", p.name.c_str(), value);
                        problems.push_back(std::string(head) + bad);
                    }
                }
            }
            report["param_sweep_problems"] = problems;
            ok = ok && problems.empty();
        }
        report["ui"] = checkUi(module->bytes(), module->ui(), meta);
        if (report["ui"].contains("errors") && !report["ui"]["errors"].empty()) ok = false;
        report["ok"] = ok;
    } catch (const std::exception& e) {
        report["error"] = e.what();
    }
    json lines = json::array();
    for (const auto& l : log.since(0))
        if (lines.size() < 50) lines.push_back(l.level + ": " + l.message);
    if (!lines.empty()) report["log"] = lines;
    return report;
}

}  // namespace aicontact
