#pragma once
#include "AudioRouting.h"
#include <atomic>
#include <future>
#include <mutex>
#include <string>
#include <thread>

namespace audioptt {
// Broadcast OFF: physical microphone -> CABLE Input; physical speakers are default.
// Broadcast ON:  Windows default output -> CABLE Input -> Discord CABLE Output.
// Monitor ON additionally copies the cable mix to physical speakers.
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
    // Saved physical speaker endpoint, even while Windows defaults point to the cable.
    std::wstring PhysicalOutputId() const { return route_.physicalOutputId; }
    std::wstring CableRenderId() const { return route_.cableRenderId; }
    // Called periodically from the UI thread to catch stalled or failed audio routes.
    bool CheckHealth(std::wstring& alert);
private:
    bool LaunchWorker(bool broadcast, std::wstring& error);
    void ShutdownWorker();
    void Worker(bool broadcast, bool monitor, std::promise<std::wstring> startup);
    AudioRoute route_;
    bool routeCaptured_ = false; // Only touched on UI/COM thread
    bool healthAlerted_ = false;
    // Monitor applies only to desktop broadcast. It never plays the microphone.
    // Start enabled so the operator hears the audio being broadcast by default.
    std::atomic<bool> monitor_{true}, broadcasting_{false}, running_{false}, stop_{false};
    std::thread worker_;
    mutable std::mutex statusMutex_;
    std::wstring status_ = L"Not started";
};
} // namespace audioptt
