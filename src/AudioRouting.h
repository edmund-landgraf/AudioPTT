#pragma once
#include <string>

namespace audioptt {
// Saved on startup. We change only the Console and Multimedia playback roles.
// Communications stays on the physical endpoint for Discord voice chat.
struct AudioRoute {
    std::wstring consoleId;
    std::wstring multimediaId;
    std::wstring physicalOutputId;
    std::wstring cableRenderId;
};
bool CaptureAudioRoute(AudioRoute& route, std::wstring& error);
bool RouteDesktopToCable(const AudioRoute& route, std::wstring& error);
bool RestoreDesktopRoute(const AudioRoute& route, std::wstring& error);
} // namespace audioptt
