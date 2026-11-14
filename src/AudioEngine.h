#pragma once
#include "AudioRouting.h"
#include <atomic>
#include <future>
#include <mutex>
#include <string>
#include <thread>

namespace audioptt {
// Broadcast OFF: fully idle. No mic capture or cable forwarding. Windows
// defaults remain physical, so Discord with Input=Default uses the webcam.
// Broadcast ON: Windows default capture=CABLE Output, Windows default playback
// (console/multimedia)=CABLE Input, so Discord with Input=Default gets PC audio.
// Monitor ON copies the cable output to local speakers; never the microphone.
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
    // Monitor applies only to desktop broadcast. Idle opens no streams.
    // Start enabled so the operator hears the audio being broadcast by default.
    std::atomic<bool> monitor_{true}, broadcasting_{false}, running_{false}, stop_{false};
    std::thread worker_;
    mutable std::mutex statusMutex_;
    std::wstring status_ = L"Not started";
};
} // namespace audioptt
