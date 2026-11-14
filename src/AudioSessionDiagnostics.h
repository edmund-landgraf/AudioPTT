#pragma once
#include <string>

namespace audioptt {
// Snapshot of Windows shared-mode audio sessions bound to actual playback
// endpoints. This is a diagnostic inventory, not attribution of audio content.
void LogRenderSessions(const std::wstring& context,
                       const std::wstring& physicalOutputId,
                       const std::wstring& cableRenderId);
 // Diagnostic capture-side session inventory to identify applications
 // consuming CABLE Output vs the physical webcam input. Sessions identify
 // processes but do not prove they actually transmitted audio to Discord.
void LogCaptureSessions(const std::wstring& context,
                        const std::wstring& physicalMicId,
                        const std::wstring& cableCaptureId);
} // namespace audioptt
