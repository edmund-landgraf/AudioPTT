#include "AudioEngine.h"

namespace audioptt {
bool AudioEngine::SetBroadcast(bool enabled, std::wstring& error) {
    if (enabled) {
        error = L"Audio routing is not implemented yet. No sound has been sent to Discord.";
        broadcasting_.store(false);
        return false;
    }
    broadcasting_.store(false);
    error.clear();
    return true;
}
} // namespace audioptt
