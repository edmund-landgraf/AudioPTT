#pragma once
#include <string>

namespace audioptt {
// Saved on startup, before AudioPTT changes any Windows default devices.
// In idle mode, all defaults remain untouched and the app captures no audio.
// In Broadcast mode, Console+Multimedia playback and all three recording
// roles point to VB-CABLE. Discord must use Default for microphone input.
struct AudioRoute {
    std::wstring consoleId;
    std::wstring multimediaId;
    std::wstring physicalOutputId;
    std::wstring cableRenderId;        // CABLE Input, Windows playback
    std::wstring captureConsoleId;
    std::wstring captureMultimediaId;
    std::wstring captureCommunicationsId;
    std::wstring cableCaptureId;       // CABLE Output, Windows recording
};
bool CaptureAudioRoute(AudioRoute& route, std::wstring& error);
bool RouteDesktopToCable(const AudioRoute& route, std::wstring& error);
bool RestoreDesktopRoute(const AudioRoute& route, std::wstring& error);
// True if any Windows playback or recording default is currently on the cable.
// Also used to detect partially completed changes or old crash recovery state.
bool IsAudioRoutedToCable(const AudioRoute& route);
// Verify the entire intended live Broadcast route, not just one role.
bool IsDesktopBroadcastRouteActive(const AudioRoute& route);
} // namespace audioptt
