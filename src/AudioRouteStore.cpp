#include "AudioRouteStore.h"
#include <windows.h>
#include <array>

namespace audioptt {
namespace {
constexpr wchar_t kRecoveryKey[] = L"Software\\AudioPTT\\Recovery";
bool WriteString(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(value.c_str()),
                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}
bool ReadString(HKEY key, const wchar_t* name, std::wstring& value) {
    std::array<wchar_t, 4096> buffer{};
    DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr,
                     buffer.data(), &bytes) != ERROR_SUCCESS) return false;
    value.assign(buffer.data());
    return !value.empty();
}
} // namespace

bool SaveRecoveryRoute(const AudioRoute& route, std::wstring& error) {
    if (route.consoleId.empty() || route.multimediaId.empty() ||
        route.physicalOutputId.empty() || route.cableRenderId.empty() ||
        route.captureConsoleId.empty() || route.captureMultimediaId.empty() ||
        route.captureCommunicationsId.empty() || route.cableCaptureId.empty()) {
        error = L"Cannot save recovery settings: audio device IDs are missing.";
        return false;
    }
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRecoveryKey, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS) {
        error = L"Unable to save audio recovery settings to your Windows user profile.";
        return false;
    }
    // Version 3 never changes Windows recording defaults. Older recovery
    // snapshots DID change them, so the loader must distinguish formats.
    const DWORD version = 3;
    const bool okay =
                      RegSetValueExW(key, L"RoutingVersion", 0, REG_DWORD,
                          reinterpret_cast<const BYTE*>(&version), sizeof(version)) == ERROR_SUCCESS &&
                      WriteString(key, L"Console", route.consoleId) &&
                      WriteString(key, L"Multimedia", route.multimediaId) &&
                      WriteString(key, L"Physical", route.physicalOutputId) &&
                      WriteString(key, L"Cable", route.cableRenderId) &&
                      WriteString(key, L"MicConsole", route.captureConsoleId) &&
                      WriteString(key, L"MicMultimedia", route.captureMultimediaId) &&
                      WriteString(key, L"MicCommunications", route.captureCommunicationsId) &&
                      WriteString(key, L"CableMic", route.cableCaptureId);
    RegCloseKey(key);
    if (!okay) {
        ClearRecoveryRoute();
        error = L"Unable to save the full audio device recovery settings.";
        return false;
    }
    error.clear();
    return true;
}

bool LoadRecoveryRoute(AudioRoute& route) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRecoveryKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    AudioRoute saved;
    const bool okay = ReadString(key, L"Console", saved.consoleId) &&
                      ReadString(key, L"Multimedia", saved.multimediaId) &&
                      ReadString(key, L"Physical", saved.physicalOutputId) &&
                      ReadString(key, L"Cable", saved.cableRenderId);
    // Older builds either changed playback only or both playback and
    // recording defaults. Missing recording keys are allowed on recovery.
    if (okay) {
        ReadString(key, L"MicConsole", saved.captureConsoleId);
        ReadString(key, L"MicMultimedia", saved.captureMultimediaId);
        ReadString(key, L"MicCommunications", saved.captureCommunicationsId);
        ReadString(key, L"CableMic", saved.cableCaptureId);
        DWORD version = 0;
        DWORD size = sizeof(version);
        const bool newRoute = RegGetValueW(key, nullptr, L"RoutingVersion",
                               RRF_RT_REG_DWORD, nullptr, &version, &size) == ERROR_SUCCESS
                               && version >= 3;
        saved.restoreLegacyCaptureRoles = !newRoute && !saved.cableCaptureId.empty()
                && !saved.captureCommunicationsId.empty();
    }
    RegCloseKey(key);
    if (okay) route = std::move(saved);
    return okay;
}

void ClearRecoveryRoute() {
    RegDeleteKeyW(HKEY_CURRENT_USER, kRecoveryKey);
}
} // namespace audioptt
