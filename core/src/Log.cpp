#include "aicontact/Log.h"

#include <chrono>
#include <cstdio>

namespace aicontact {

static double wallSeconds() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

void Log::pushLocked(LogLine line) {
    line.seq = nextSeq_++;
    line.time = wallSeconds();
    lines_.push_back(std::move(line));
    while (lines_.size() > capacity_) lines_.pop_front();
}

void Log::log(const std::string& level, const std::string& slot, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    pushLocked({0, 0, level, slot, message});
}

bool Log::tryLog(const char* level, const char* slot, const char* message, size_t len) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    // Allocation here is accepted: it only happens on errors / module debug
    // output, which is rate limited by the caller.
    pushLocked({0, 0, level, slot ? slot : "", std::string(message, len)});
    return true;
}

std::vector<LogLine> Log::since(uint64_t seq) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LogLine> out;
    for (const auto& l : lines_)
        if (l.seq > seq) out.push_back(l);
    return out;
}

}  // namespace aicontact
