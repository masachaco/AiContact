// aicontact-host: headless AI Contact host.
//
//   aicontact-host check    <instrument.wasm>
//   aicontact-host validate <instrument.wasm> [--keys 36,38] [--seconds 2] [--wav out.wav] [--no-sweep]
//   aicontact-host render <instrument.wasm> -o out.wav [--seconds 3] [--notes 60,64,67] [--sr 48000]
//   aicontact-host serve  [--name NAME] [--sr 48000] [--record out.wav] [--token T] [--state FILE]
//
// `serve` runs a rack with the control server and a simulated real-time audio
// clock, so the API / CLI can be used end-to-end without a DAW.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "aicontact/Analyze.h"
#include "aicontact/Auth.h"
#include "aicontact/ControlServer.h"
#include "aicontact/Instrument.h"
#include "aicontact/Rack.h"
#include "aicontact/Ui.h"
#include "aicontact/Util.h"

using namespace aicontact;
using json = nlohmann::json;

namespace {

std::atomic<bool> gStop{false};

void writeInterleaved(const std::string& path, const std::vector<float>& interleaved, int sampleRate) {
    std::vector<float> l(interleaved.size() / 2), r(interleaved.size() / 2);
    for (size_t i = 0; i < l.size(); ++i) { l[i] = interleaved[2 * i]; r[i] = interleaved[2 * i + 1]; }
    writeWav(path, l, r, sampleRate);
}

bool fileExists(const std::string& path) { return std::ifstream(path, std::ios::binary).good(); }

void saveStateIfChanged(Rack& rack, const std::string& path, std::string& saved) {
    std::string state = rack.saveState();
    if (state == saved) return;
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << state;
    }
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) == 0) saved = std::move(state);
}

std::string arg(int argc, char** argv, const char* name, const std::string& def) {
    for (int i = 0; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return def;
}

int cmdCheck(const std::string& path) {
    auto module = InstrumentModule::load(readFile(path));
    const auto& m = module->meta();
    json out = json::parse(m.json);
    std::cout << "OK  " << m.name << " (" << m.id << ") v" << m.version << "  hash " << module->hash() << "\n"
              << out.dump(2) << "\n";
    const json ui = aicontact::checkUi(module->bytes(), module->ui(), m);
    if (ui["kind"] != "none" || ui.contains("errors")) std::cout << "custom UI:\n" << ui.dump(2) << "\n";
    return 0;
}

int cmdValidate(int argc, char** argv, const std::string& path) {
    ValidateOptions opt;
    opt.seconds = std::stod(arg(argc, argv, "--seconds", "2"));
    opt.sampleRate = std::stod(arg(argc, argv, "--sr", "48000"));
    opt.wavPath = arg(argc, argv, "--wav", "");
    std::string keys = arg(argc, argv, "--keys", "");
    if (!keys.empty()) {
        opt.keys.clear();
        for (size_t p = 0; p < keys.size();) {
            size_t q = keys.find(',', p);
            opt.keys.push_back(std::stoi(keys.substr(p, q - p)));
            p = q == std::string::npos ? keys.size() : q + 1;
        }
    }
    for (int i = 0; i < argc; ++i)
        if (std::strcmp(argv[i], "--no-sweep") == 0) opt.paramSweep = false;
    json report = validateInstrument(readFile(path), opt);
    std::cout << report.dump(2) << "\n";
    return report["ok"].get<bool>() ? 0 : 1;
}

int cmdRender(int argc, char** argv, const std::string& path) {
    const int sr = std::stoi(arg(argc, argv, "--sr", "48000"));
    const double seconds = std::stod(arg(argc, argv, "--seconds", "3"));
    const std::string outPath = arg(argc, argv, "-o", "out.wav");
    std::vector<int> notes;
    std::string spec = arg(argc, argv, "--notes", "60,64,67");
    for (size_t p = 0; p < spec.size();) {
        size_t q = spec.find(',', p);
        notes.push_back(std::stoi(spec.substr(p, q - p)));
        p = q == std::string::npos ? spec.size() : q + 1;
    }

    Rack rack;
    rack.prepare(sr, 512);
    json r = rack.handleRpc({{"method", "slot.deploy"}, {"params", {{"path", path}, {"slot_id", "test"}}}});
    if (r.contains("error")) { std::cerr << r["error"].get<std::string>() << "\n"; return 1; }

    const int total = (int)(seconds * sr), block = 512;
    const int releaseAt = (int)(total * 0.6);
    std::vector<float> out((size_t)total * 2), L(block), R(block);
    auto t0 = std::chrono::steady_clock::now();
    for (int pos = 0; pos < total; pos += block) {
        const int n = std::min(block, total - pos);
        std::vector<MidiEvent> ev;
        if (pos == 0) for (int k : notes) ev.push_back({0, 0x90, (uint8_t)k, 100});
        if (releaseAt >= pos && releaseAt < pos + n)
            for (int k : notes) ev.push_back({releaseAt - pos, 0x80, (uint8_t)k, 0});
        rack.process(L.data(), R.data(), n, ev.data(), (int)ev.size());
        for (int i = 0; i < n; ++i) { out[(size_t)(pos + i) * 2] = L[i]; out[(size_t)(pos + i) * 2 + 1] = R[i]; }
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    float peak = 0;
    for (float v : out) peak = std::max(peak, std::fabs(v));
    writeInterleaved(outPath, out, sr);
    std::printf("rendered %.2fs in %.3fs (%.1fx realtime), peak %.3f -> %s\n", seconds, elapsed,
                seconds / elapsed, peak, outPath.c_str());
    for (const auto& l : rack.log().since(0)) std::printf("  [%s] %s %s\n", l.level.c_str(), l.slot.c_str(), l.message.c_str());
    return 0;
}

int cmdServe(int argc, char** argv) {
    const int sr = std::stoi(arg(argc, argv, "--sr", "48000"));
    const std::string record = arg(argc, argv, "--record", "");
    // The API token lives only in this process: given with --token (several hosts sharing one
    // directory need the same token) or made up now and printed for the client.
    std::string token = arg(argc, argv, "--token", "");
    if (token.empty()) token = generateToken();
    if (!isValidToken(token)) { std::cerr << "--token must be 64 hex characters\n"; return 2; }
    // --state FILE plays the part of a DAW project: restored at start (if it exists) and saved whenever it
    // changes (a killed process -- Windows has no SIGTERM -- still leaves the latest state behind).
    const std::string statePath = arg(argc, argv, "--state", "");
    Rack rack;
    rack.setName(arg(argc, argv, "--name", "Headless Rack"));
    rack.prepare(sr, 256);
    if (!statePath.empty() && fileExists(statePath)) {
        const auto bytes = readFile(statePath);
        rack.loadState(std::string(bytes.begin(), bytes.end()));
    }
    ControlServer server(rack, [token] { return token; });
    if (!server.start()) { std::cerr << "could not start control server\n"; return 1; }
    std::printf("AI Contact headless host '%s' on 127.0.0.1:%d, rack directory 127.0.0.1:%d (Ctrl+C to quit)\n"
                "token: %s\n", rack.name().c_str(), server.port(), server.gatewayPort(), token.c_str());
    std::fflush(stdout);

    std::signal(SIGINT, [](int) { gStop = true; });
    std::signal(SIGTERM, [](int) { gStop = true; });

    const int block = 256;
    std::vector<float> L(block), R(block), recorded;
    auto next = std::chrono::steady_clock::now();
    auto lastTick = next;
    uint64_t lastLogSeq = 0;
    std::string savedState;
    while (!gStop) {
        rack.process(L.data(), R.data(), block, nullptr, 0);
        if (!record.empty() && recorded.size() < (size_t)sr * 2 * 600)
            for (int i = 0; i < block; ++i) { recorded.push_back(L[i]); recorded.push_back(R[i]); }
        next += std::chrono::microseconds((int64_t)(1e6 * block / sr));
        std::this_thread::sleep_until(next);
        auto now = std::chrono::steady_clock::now();
        if (now - lastTick > std::chrono::milliseconds(100)) {
            lastTick = now;
            rack.tick();
            if (!statePath.empty()) saveStateIfChanged(rack, statePath, savedState);
            for (const auto& l : rack.log().since(lastLogSeq)) {
                std::printf("[%s] %s %s\n", l.level.c_str(), l.slot.c_str(), l.message.c_str());
                lastLogSeq = l.seq;
            }
            std::fflush(stdout);
        }
    }
    server.stop();
    if (!statePath.empty()) saveStateIfChanged(rack, statePath, savedState);
    if (!record.empty()) {
        writeInterleaved(record, recorded, sr);
        std::printf("recorded %zu frames -> %s\n", recorded.size() / 2, record.c_str());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage:\n  aicontact-host check <instrument.wasm>\n"
                     "  aicontact-host validate <instrument.wasm> [--keys 48,60,64] [--seconds 2] [--wav out.wav] [--no-sweep]\n"
                     "  aicontact-host render <instrument.wasm> -o out.wav [--seconds 3] [--notes 60,64,67] [--sr 48000]\n"
                     "  aicontact-host serve [--name NAME] [--sr 48000] [--record out.wav] [--token T] [--state FILE]\n");
        return 2;
    }
    const std::string cmd = argv[1];
    try {
        if (cmd == "check" && argc >= 3) return cmdCheck(argv[2]);
        if (cmd == "validate" && argc >= 3) return cmdValidate(argc, argv, argv[2]);
        if (cmd == "render" && argc >= 3) return cmdRender(argc, argv, argv[2]);
        if (cmd == "serve") return cmdServe(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::fprintf(stderr, "unknown command\n");
    return 2;
}
