#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "aicontact/Instrument.h"
#include "aicontact/Log.h"

namespace aicontact {

struct MidiEvent {
    int offset = 0;  // sample offset inside the block
    uint8_t status = 0, data1 = 0, data2 = 0;
};

// Automation coming from the plugin host (DAW) for a slot's parameter.
struct MacroChange {
    int slotIndex = 0;
    int paramIndex = 0;
    float normalized = 0;
};

// A rack position. Shared between snapshots; its settings are atomics so the
// audio thread can read them without locks.
class Slot {
public:
    Slot(uint32_t serialNumber, std::string slotId) : serial(serialNumber), id(std::move(slotId)) {}

    const uint32_t serial;
    const std::string id;

    // --- control side (guarded by Rack::mutex_) ---
    std::string name;
    std::string sourcePath;   // the instrument file (absolute path on this machine); read on every load
    std::string sha256;       // what that file held when it was deployed; anything else is refused on load
    std::string lastError;    // why the instrument could not be loaded, cleared on success
    nlohmann::json savedParams = nlohmann::json::object();  // {id: value} kept while the file is unreadable
    double fadingUntil = 0;   // when the crossfade source may be dropped

    // --- shared (atomics) ---
    std::atomic<int> channel{0};  // 0 = omni, 1..16
    std::atomic<int> keyLow{0}, keyHigh{127}, transpose{0};
    std::atomic<float> gainDb{0.0f}, pan{0.0f};
    std::atomic<bool> mute{false}, solo{false};
    std::atomic<float> peak{0.0f};     // max |sample| since last read
    std::atomic<float> cpuLoad{0.0f};  // smoothed fraction of the block budget

    // --- audio thread only ---
    InstrumentInstance* audioInst = nullptr;
    InstrumentInstance* fadeInst = nullptr;
    int fadePos = 0, fadeLen = 0;
    std::array<uint8_t, 128> heldVelocity{};  // non-zero while a key is down (or sustained)
    std::array<uint8_t, 128> sentKey{};       // transposed key actually sent to the module
    std::array<bool, 128> sustained{};
    bool pedal = false;
};

class Rack {
public:
    static constexpr int kMaxSlots = 32;

    Rack();
    ~Rack();

    // ---- host lifecycle (never concurrent with process) ----
    void prepare(double sampleRate, int maxBlockSize);

    // ---- audio thread ----
    // Renders `frames` samples (outputs are overwritten). `events` must be
    // sorted by offset.
    void process(float* left, float* right, int frames,
                 const MidiEvent* events, int numEvents,
                 const MacroChange* macros = nullptr, int numMacros = 0);

    // ---- control side (any non-audio thread) ----
    // Periodic housekeeping, call ~10x per second: frees retired snapshots and
    // finishes crossfades.
    void tick();

    // JSON-RPC style entry point used by the control server and the GUI:
    // {"method": "...", "params": {...}} -> {"result": ...} or {"error": "..."}
    nlohmann::json handleRpc(const nlohmann::json& request);

    // Project state: every slot's instrument file path, its SHA-256, routing, mixer
    // and params. No binaries: loadState reads each file again and checks its hash
    // before running it; slots whose file is missing, changed or invalid go into an
    // error state (status "error") instead of playing anything else. Throws on
    // malformed state.
    std::string saveState();
    void loadState(const std::string& state);

    std::string name() const;
    void setName(const std::string& name);
    double sampleRate() const { return sampleRate_.load(); }
    Log& log() { return log_; }

    // Host-automation helpers (plugin parameters).
    int numSlots() const;
    float paramNormalized(int slotIndex, int paramIndex) const;  // -1 if absent
    std::string paramLabel(int slotIndex, int paramIndex) const;

    // The module a slot currently runs (null if none). Immutable, so the GUI can
    // read its custom UI files without holding any lock.
    std::shared_ptr<const InstrumentModule> slotModule(const std::string& slotId) const;

    // Bumped on every structural change (deploy/remove/rename...).
    uint64_t changeCounter() const { return changeCounter_.load(); }

private:
    struct SlotState {
        std::shared_ptr<Slot> slot;
        std::shared_ptr<InstrumentInstance> inst;
        std::shared_ptr<InstrumentInstance> fading;  // previous instance, fading out
    };
    struct Snapshot {
        std::vector<SlotState> slots;
    };
    struct Retired {
        Snapshot* snapshot;
        uint64_t epoch;
    };
    struct Injected {
        uint8_t status, data1, data2;
        uint32_t targetSerial;  // 0 = route by channel
        uint32_t delayFrames;
    };
    struct BlockEvent {
        int offset;
        uint8_t status, data1, data2;
        uint32_t targetSerial;
    };

    // control helpers (mutex_ held)
    void publishLocked();
    void collectGarbageLocked();
    SlotState* findLocked(const std::string& id);
    int indexOfLocked(const std::string& id) const;
    std::string uniqueIdLocked(const std::string& base) const;
    nlohmann::json slotInfoLocked(const SlotState& s, bool detailed) const;
    nlohmann::json rackInfoLocked() const;
    void applySettingsLocked(Slot& slot, const nlohmann::json& p);
    void applyParamsLocked(SlotState& s, const nlohmann::json& params);
    nlohmann::json deployLocked(const nlohmann::json& p);
    void reloadLocked(SlotState& s, std::shared_ptr<const InstrumentModule> module);
    void inject(const Injected& ev);

    // audio helpers
    void deliver(Slot& slot, InstrumentInstance& inst, const BlockEvent& ev, bool targeted);
    void render(SlotState& s, float* left, float* right, int start, int end, bool audible, float& peak);

    mutable std::mutex mutex_;
    std::vector<SlotState> slots_;  // authoritative list (control side)
    std::vector<Retired> retired_;
    std::string name_ = "AI Contact";
    uint32_t nextSerial_ = 1;
    std::atomic<uint64_t> changeCounter_{0};

    std::atomic<Snapshot*> current_{nullptr};
    std::atomic<uint64_t> epoch_{0};
    std::atomic<double> sampleRate_{48000.0};

    // injected events: multi-producer (guarded by injectMutex_), single consumer
    std::mutex injectMutex_;
    static constexpr int kInjectCapacity = 1024;
    std::array<Injected, kInjectCapacity> injectRing_{};
    std::atomic<uint32_t> injectWrite_{0}, injectRead_{0};

    // audio-thread state
    struct Pending { uint64_t dueFrame; Injected ev; };
    std::array<Pending, kInjectCapacity> pending_{};
    int numPending_ = 0;
    uint64_t framePosition_ = 0;
    std::vector<BlockEvent> blockEvents_;
    std::atomic<double> lastProcessTime_{0};

    Log log_;
};

}  // namespace aicontact
