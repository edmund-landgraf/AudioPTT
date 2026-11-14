#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace audioptt {
// All COM audio objects are created and used on the worker thread.
// Reconfigure synchronously to avoid mixing microphone and desktop packets.
class AudioEngine {
public:
    ~AudioEngine();
    bool Start(std::wstring& error);
    bool SetBroadcast(bool enabled, std::wstring& error);
    bool SetMonitor(bool enabled, std::wstring& error);
    void Stop();
    bool Monitor() const noexcept { return monitor_.load(); }
    bool Broadcasting() const noexcept { return broadcasting_.load(); }
    bool Running() const noexcept { return running_.load(); }
    std::wstring Status() const;
private:
    bool Launch(bool broadcast, std::wstring& error);
    void Worker(bool broadcast, std::promise<std::wstring> startup);
    std::atomic<bool> monitor_{false}, broadcasting_{false}, running_{false}, stop_{false};
    std::thread worker_;
    mutable std::mutex statusMutex_;
    std::wstring status_ = L"Not started";
};
} // namespace audioptt
