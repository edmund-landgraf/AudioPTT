#pragma once
#include <string>

namespace audioptt {
struct EchoTestResult {
    bool completed = false;
    bool echoDetected = false;
    double toneLevel = 0.0;  // approximate amplitude on the physical speakers, 0..1
    std::wstring message;
};
// Diagnostic only. Plays a quiet A-B-A tone into VB-CABLE's playback endpoint
// and listens to the physical speaker mix. This must run on a worker thread.
// Does not monitor/mute the microphone or change Windows default devices.
EchoTestResult RunEchoTest(const std::wstring& cableRenderId,
                           const std::wstring& physicalRenderId);
} // namespace audioptt
