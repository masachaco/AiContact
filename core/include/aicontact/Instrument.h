#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aicontact/Ui.h"

struct M3Environment;
struct M3Runtime;
struct M3Module;
struct M3Function;

namespace aicontact {

class Log;

struct ParamInfo {
    int id = 0;
    std::string name;
    float min = 0, max = 1, def = 0;
    std::string unit, scale = "linear";
    int steps = 0;
    std::vector<std::string> labels;

    float clamp(float v) const;
    float toNormalized(float v) const;    // honours "log" scale
    float fromNormalized(float n) const;
};

struct InstrumentMeta {
    int aci = 0;
    std::string id, name, version, author, description;
    std::vector<std::string> tags;
    int outputs = 2;
    int polyphony = 0;
    bool hostSustain = true;
    std::vector<ParamInfo> params;
    std::string json;  // raw metadata as returned by aci_meta()

    int paramIndexById(int id) const;
    int paramIndexByName(const std::string& name) const;  // case-insensitive
};

// An immutable, validated ACI binary. Shared by every instance created from it.
class InstrumentModule {
public:
    // Validates the binary (structure, ABI version, metadata, init) and
    // returns a module, or throws std::runtime_error with a readable reason.
    static std::shared_ptr<const InstrumentModule> load(std::vector<uint8_t> bytes);

    const std::vector<uint8_t>& bytes() const { return *bytes_; }
    const InstrumentMeta& meta() const { return meta_; }
    const std::string& hash() const { return hash_; }

    // Custom GUI files (aci.ui/* sections). Data points into bytes().
    const UiBundle& ui() const { return ui_; }
    const uint8_t* uiData(const UiFile& f) const { return bytes_->data() + f.offset; }

private:
    friend class InstrumentInstance;
    std::shared_ptr<const std::vector<uint8_t>> bytes_;
    InstrumentMeta meta_;
    UiBundle ui_;
    std::string hash_;
};

// One running instance (its own wasm runtime and linear memory).
//
// Threading: create() runs on a control thread. After the instance has been
// published to the audio thread, every call except setParam()/paramValue()/
// crashed()/crashReason() must come from the audio thread only.
class InstrumentInstance {
public:
    static constexpr int kMaxFrames = 256;  // block size handed to modules

    // Throws std::runtime_error on failure. `paramValues` (by param index)
    // may be empty, in which case defaults are used.
    static std::unique_ptr<InstrumentInstance> create(std::shared_ptr<const InstrumentModule> module,
                                                      double sampleRate,
                                                      const std::vector<float>& paramValues,
                                                      Log* log, const std::string& slotId);
    ~InstrumentInstance();

    const InstrumentModule& module() const { return *module_; }
    std::shared_ptr<const InstrumentModule> modulePtr() const { return module_; }
    const InstrumentMeta& meta() const { return module_->meta(); }

    // ---- audio thread ----
    // Renders `frames` (<= kMaxFrames) samples. Returns false if the instance
    // is (or just became) crashed; outputs are then silent.
    bool process(int frames);
    const float* outL() const { return outL_; }
    const float* outR() const { return outR_; }

    void noteOn(int key, float velocity);
    void noteOff(int key, float velocity);
    void pitchBend(float value);
    void controlChange(int cc, float value);
    void channelPressure(float value);
    void allNotesOff();
    void applyPendingParams();

    // ---- any thread ----
    void setParam(int index, float value);  // plain value, clamped
    float paramValue(int index) const;
    std::vector<float> paramValues() const;
    bool crashed() const { return crashed_.load(std::memory_order_acquire); }
    std::string crashReason() const;
    uint64_t nanCount() const { return nanCount_.load(std::memory_order_relaxed); }

    // Used by the host import env.aci_log.
    void moduleLog(const char* msg, size_t len);

private:
    InstrumentInstance() = default;
    bool call(M3Function* fn, uint32_t argc, const void** argv, uint64_t gas);
    void crash(const char* what);

    std::shared_ptr<const InstrumentModule> module_;
    std::shared_ptr<const std::vector<uint8_t>> bytes_;
    M3Environment* env_ = nullptr;
    M3Runtime* runtime_ = nullptr;
    M3Module* wasmModule_ = nullptr;

    M3Function* fnProcess_ = nullptr;
    M3Function* fnNoteOn_ = nullptr;
    M3Function* fnNoteOff_ = nullptr;
    M3Function* fnSetParam_ = nullptr;
    M3Function* fnPitchBend_ = nullptr;
    M3Function* fnControlChange_ = nullptr;
    M3Function* fnChannelPressure_ = nullptr;
    M3Function* fnAllNotesOff_ = nullptr;

    uint32_t outOffset_[2] = {0, 0};
    float outL_[kMaxFrames] = {};
    float outR_[kMaxFrames] = {};

    size_t numParams_ = 0;
    std::unique_ptr<std::atomic<float>[]> paramTarget_;
    std::unique_ptr<float[]> paramApplied_;
    std::atomic<bool> paramsDirty_{false};

    std::atomic<bool> crashed_{false};
    char crashReason_[256] = {};
    std::atomic<uint64_t> nanCount_{0};

    Log* log_ = nullptr;
    char slotId_[64] = {};
    double logWindowStart_ = 0;
    int logCount_ = 0;
};

}  // namespace aicontact
