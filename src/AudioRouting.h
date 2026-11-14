#pragma once
#include <string>

namespace audioptt {
// Saved on startup, before AudioPTT changes any Windows playback defaults.
// Discord input stays pinned to VB-CABLE CABLE Output, never 'Default'.
// Broadcast OFF: webcam captured by AudioPTT -> VB-CABLE; playback stays physical.
// Broadcast ON: webcam forwarding stops; Console/Multimedia playback -> CABLE Input.
// Windows recording defaults are NEVER changed by new broadcasts.
struct AudioRoute {
    std::wstring consoleId;
    std::wstring multimediaId;
    std::wstring physicalOutputId;
    std::wstring cableRenderId;        // CABLE Input, Windows playback
    std::wstring captureConsoleId;
    std::wstring captureMultimediaId;
    std::wstring captureCommunicationsId;
    std::wstring cableCaptureId;       // CABLE Output, explicit Discord input
    // Only true for recovery records written by the prior release, which DID
    // change Windows recording defaults and may need them restored after a crash.
    bool restoreLegacyCaptureRoles = false;
};
bool CaptureAudioRoute(AudioRoute& route, std::wstring& error);
bool RouteDesktopToCable(const AudioRoute& route, std::wstring& error);
bool RestoreDesktopRoute(const AudioRoute& route, std::wstring& error);
// True if an AudioPTT-owned playback default (or legacy recovery capture role)
// is still pointing to VB-CABLE.
bool IsAudioRoutedToCable(const AudioRoute& route);
// Verify both desktop playback roles are routed through VB-CABLE.
bool IsDesktopBroadcastRouteActive(const AudioRoute& route);
} // namespace audioptt
