#pragma once
#include <string>

namespace audioptt {
// Snapshot of Windows shared-mode audio sessions bound to actual playback
// endpoints. This is a diagnostic inventory, not attribution of audio content.
void LogRenderSessions(const std::wstring& context,
                       const std::wstring& physicalOutputId,
                       const std::wstring& cableRenderId);
} // namespace audioptt
