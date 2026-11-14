#pragma once
#include <atomic>
#include <string>

namespace audioptt {
// Text-to-speech sample sent exclusively to the specified VB-CABLE playback
// endpoint, never to the Windows default or physical speakers. The optional
// desktop Broadcast monitor is solely responsible for intentional local sound.
// Call from a worker thread. Cancellation is checked during WASAPI playback.
bool PlaySampleAudioToCable(const std::wstring& cableRenderId,
                            std::atomic<bool>& cancel,
                            std::wstring& error);
} // namespace audioptt
