#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace aicontact {

struct LogLine {
    uint64_t seq = 0;
    double time = 0;        // seconds since epoch
    std::string level;      // "info" | "warn" | "error" | "module"
    std::string slot;       // slot id, may be empty
    std::string message;
};

// Bounded in-memory log. log() may be called from any thread; tryLog() is the
// real-time-safe variant (drops the line if the lock is contended).
class Log {
public:
    explicit Log(size_t capacity = 500) : capacity_(capacity) {}

    void log(const std::string& level, const std::string& slot, const std::string& message);
    bool tryLog(const char* level, const char* slot, const char* message, size_t len);
    std::vector<LogLine> since(uint64_t seq) const;

private:
    void pushLocked(LogLine line);

    mutable std::mutex mutex_;
    std::deque<LogLine> lines_;
    uint64_t nextSeq_ = 1;
    size_t capacity_;
};

}  // namespace aicontact
