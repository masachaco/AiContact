#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace aicontact {

struct ValidateOptions {
    std::vector<int> keys{48, 60, 64, 67};  // pressed together at t=0, released at 60%
    double seconds = 2.0;
    double sampleRate = 48000.0;
    std::string wavPath;                    // optional: write the render here
    bool paramSweep = true;                 // render every param at min and max
};

// Full check of an ACI binary without a rack: spec conformance, a test render
// with level / NaN / DC / clipping / stuck-note analysis, the CPU load the
// plugin would see (same wasm3 engine) and a min/max sweep of every param.
// Never throws for module problems: they are reported as {"ok": false, ...}.
nlohmann::json validateInstrument(const std::vector<uint8_t>& wasm, const ValidateOptions& options);

void writeWav(const std::string& path, const std::vector<float>& left, const std::vector<float>& right,
              int sampleRate);  // 32-bit float stereo, throws on I/O error

}  // namespace aicontact
