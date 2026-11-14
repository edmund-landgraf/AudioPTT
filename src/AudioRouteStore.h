#pragma once
#include "AudioRouting.h"
#include <string>

namespace audioptt {
// Saved before changing Windows defaults. Allows the next launch to offer
// recovery if the app crashed while broadcasting through VB-CABLE.
bool SaveRecoveryRoute(const AudioRoute& route, std::wstring& error);
bool LoadRecoveryRoute(AudioRoute& route);
void ClearRecoveryRoute();
} // namespace audioptt
