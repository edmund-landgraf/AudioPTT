#include "AudioSessionDiagnostics.h"
#include "Diagnostics.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <wrl/client.h>
#include <algorithm>
#include <iterator>
#include <string>

using Microsoft::WRL::ComPtr;

namespace audioptt {
namespace {
struct ScopedCom {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // If called from the STA Win32 UI thread, COM is already usable.
    bool Ready() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
    ~ScopedCom() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
std::wstring HexHr(HRESULT hr) {
    wchar_t text[20]{};
    swprintf_s(text, L"0x%08X", static_cast<unsigned>(hr));
    return text;
}
std::wstring ExeName(DWORD pid) {
    if (!pid) return L"(system)";
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return L"(process unavailable)";
    wchar_t path[32768]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    const BOOL success = QueryFullProcessImageNameW(process, 0, path, &length);
    CloseHandle(process);
    if (!success) return L"(process unavailable)";
    std::wstring filename(path, length);
    const auto slash = filename.find_last_of(L"\\/");
    return slash == std::wstring::npos ? filename : filename.substr(slash + 1);
}
std::wstring State(AudioSessionState state) {
    switch (state) {
        case AudioSessionStateActive: return L"active";
        case AudioSessionStateInactive: return L"inactive";
        case AudioSessionStateExpired: return L"expired";
        default: return L"unknown";
    }
}
void Inspect(IMMDeviceEnumerator* enumerator, const wchar_t* title,
             const std::wstring& deviceId) {
    if (deviceId.empty()) {
        LogWarning(std::wstring(title) + L": no endpoint ID");
        return;
    }
    ComPtr<IMMDevice> device;
    HRESULT hr = enumerator->GetDevice(deviceId.c_str(), &device);
    if (FAILED(hr)) {
        LogWarning(std::wstring(title) + L": cannot open endpoint: " + HexHr(hr));
        return;
    }
    ComPtr<IAudioSessionManager2> manager;
    hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(manager.GetAddressOf()));
    if (FAILED(hr)) {
        LogWarning(std::wstring(title) + L": session manager unavailable: " + HexHr(hr));
        return;
    }
    ComPtr<IAudioSessionEnumerator> sessions;
    hr = manager->GetSessionEnumerator(&sessions);
    if (FAILED(hr)) {
        LogWarning(std::wstring(title) + L": cannot enumerate audio sessions: " + HexHr(hr));
        return;
    }
    int count = 0;
    hr = sessions->GetCount(&count);
    if (FAILED(hr)) {
        LogWarning(std::wstring(title) + L": session count failed: " + HexHr(hr));
        return;
    }
    LogInfo(std::wstring(title) + L": " + std::to_wstring(count) +
            L" audio session(s) (includes inactive, excludes exclusive-mode streams).");
    const int limit = std::min(count, 48);
    for (int i = 0; i < limit; ++i) {
        ComPtr<IAudioSessionControl> session;
        if (FAILED(sessions->GetSession(i, &session))) continue;
        AudioSessionState state = AudioSessionStateInactive;
        if (FAILED(session->GetState(&state)) || state == AudioSessionStateExpired) continue;
        ComPtr<IAudioSessionControl2> session2;
        DWORD pid = 0;
        if (SUCCEEDED(session.As(&session2))) {
            // Some sessions span processes. The returned PID is only a clue,
            // not proof of which application caused a captured tone.
            if (FAILED(session2->GetProcessId(&pid))) pid = 0;
        }
        LPWSTR display = nullptr;
        std::wstring displayName;
        if (SUCCEEDED(session->GetDisplayName(&display)) && display) {
            displayName = display;
            CoTaskMemFree(display);
        }
        const std::wstring name = ExeName(pid);
        std::wstring details = std::wstring(title) + L": [" + State(state) +
            L"] pid=" + std::to_wstring(pid) + L" " + name;
        if (!displayName.empty() && displayName != name)
            details += L" (session: " + displayName + L")";

        // Metering support is optional for an individual audio session.
        ComPtr<IAudioMeterInformation> meter;
        if (SUCCEEDED(session.As(&meter))) {
            float peak = 0.0f;
            if (SUCCEEDED(meter->GetPeakValue(&peak)))
                details += L" peak=" + std::to_wstring(peak);
        }
        LogInfo(details);
    }
    if (count > limit)
        LogWarning(std::wstring(title) + L": additional sessions omitted from log.");
}
}
void LogRenderSessions(const std::wstring& context,
                       const std::wstring& physicalOutputId,
                       const std::wstring& cableRenderId) {
    LogInfo(L"---- Audio session snapshot: " + context + L" ----");
    ScopedCom com;
    if (!com.Ready()) {
        LogWarning(L"Session snapshot: COM unavailable " + HexHr(com.hr));
        return;
    }
    ComPtr<IMMDeviceEnumerator> enumerator;
    const HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        LogWarning(L"Session snapshot: device enumerator failed " + HexHr(hr));
        return;
    }
    Inspect(enumerator.Get(), L"PHYSICAL SPEAKERS", physicalOutputId);
    if (cableRenderId == physicalOutputId) {
        LogWarning(L"VB-CABLE and physical-speaker endpoint IDs are identical; possible routing error.");
        return;
    }
    Inspect(enumerator.Get(), L"VB-CABLE INPUT", cableRenderId);
    LogInfo(L"Session names are clues only. Windows 'Listen to this device', drivers and "
            L"browser/device mixing may not appear as a separately identifiable session.");
}
} // namespace audioptt
