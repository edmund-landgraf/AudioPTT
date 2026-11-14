#pragma once
#include "AudioRouting.h"
#include <atomic>
#include <future>
#include <mutex>
#include <string>
#include <thread>

namespace audioptt {
// Discord INPUT is pinned to VB-CABLE recording/CABLE Output in both modes.
// Discord OUTPUT stays on physical speakers and is never sent into VB-CABLE.
// Broadcast OFF: webcam mic -> CABLE Input; no speaker monitoring from AudioPTT.
// Broadcast ON: webcam stopped, Windows Console/Multimedia playback -> CABLE Input;
// Monitor ON optionally copies that desktop audio to physical speakers.
// Windows default microphone roles are not changed.
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
    // Monitor applies only to desktop broadcast. It NEVER monitors the mic.
    // Start enabled so the operator hears the audio being broadcast by default.
    std::atomic<bool> monitor_{true}, broadcasting_{false}, running_{false}, stop_{false};
    std::thread worker_;
    mutable std::mutex statusMutex_;
    std::wstring status_ = L"Not started";
};
} // namespace audioptt
