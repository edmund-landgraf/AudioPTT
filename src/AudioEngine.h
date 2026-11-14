#pragma once
#include <atomic>
#include <string>

namespace audioptt {
// Routing deliberately NOT implemented; this separates GUI state from real audio operations.
// Future Start() must only report success after all WASAPI streams have opened.
class AudioEngine {
public:
    void SetMonitor(bool enabled) noexcept { monitor_.store(enabled); }
    bool Monitor() const noexcept { return monitor_.load(); }
    bool Broadcasting() const noexcept { return broadcasting_.load(); }
    bool SetBroadcast(bool enabled, std::wstring& error);
private:
    std::atomic<bool> monitor_{false};
    std::atomic<bool> broadcasting_{false};
};
} // namespace audioptt
