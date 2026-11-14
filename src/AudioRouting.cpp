#include "AudioRouting.h"
#include "AudioDevices.h"
#include "Diagnostics.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace audioptt {
namespace {
// This COM interface is undocumented by Microsoft. It may change across
// Windows versions. Calls must fail closed and never guess a device ID.
// Interface layout is retained solely to call SetDefaultEndpoint.
struct IPolicyConfig : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
const GUID iidPolicy = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};
const GUID clsidPolicy = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
std::wstring EndpointId(IMMDevice* device) {
    LPWSTR raw = nullptr;
    if (!device || FAILED(device->GetId(&raw))) return {};
    std::wstring id(raw);
    CoTaskMemFree(raw);
    return id;
}
std::wstring DefaultId(IMMDeviceEnumerator* enumerator, EDataFlow flow, ERole role) {
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, role, &device))) return {};
    return EndpointId(device.Get());
}
std::wstring Name(IMMDevice* device) {
    ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) return {};
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) &&
        value.vt == VT_LPWSTR && value.pwszVal) name = value.pwszVal;
    PropVariantClear(&value);
    std::transform(name.begin(), name.end(), name.begin(), towupper);
    return name;
}
bool OpenObjects(ComPtr<IMMDeviceEnumerator>& enumerator, ComPtr<IPolicyConfig>* policy,
                 std::wstring& error) {
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) { error = L"Unable to enumerate Windows audio devices."; LogError(error); return false; }
    if (policy) {
        hr = CoCreateInstance(clsidPolicy, nullptr, CLSCTX_ALL, iidPolicy,
                              reinterpret_cast<void**>(policy->GetAddressOf()));
        if (FAILED(hr)) {
            error = L"Windows default audio routing API is unavailable (PolicyConfig). HRESULT " + std::to_wstring(static_cast<unsigned>(hr));
            LogError(error);
            return false;
        }
    }
    return true;
}
bool SetRole(IPolicyConfig* policy, const std::wstring& id, EDataFlow flow,
             ERole role, std::wstring& error) {
    if (id.empty()) { error = L"Missing saved audio endpoint ID."; LogError(error); return false; }
    HRESULT hr = policy->SetDefaultEndpoint(id.c_str(), role);
    if (FAILED(hr)) {
        error = L"Windows rejected default " +
                std::wstring(flow == eCapture ? L"recording" : L"playback") +
                L" device change, role=" +
                std::to_wstring(static_cast<int>(role)) + L", HRESULT=" +
                std::to_wstring(static_cast<unsigned>(hr));
        LogError(error);
        return false;
    }
    return true;
}
} // namespace

bool CaptureAudioRoute(AudioRoute& route, std::wstring& error) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (!OpenObjects(enumerator, nullptr, error)) return false;
    route.consoleId = DefaultId(enumerator.Get(), eRender, eConsole);
    route.multimediaId = DefaultId(enumerator.Get(), eRender, eMultimedia);
    route.captureConsoleId = DefaultId(enumerator.Get(), eCapture, eConsole);
    route.captureMultimediaId = DefaultId(enumerator.Get(), eCapture, eMultimedia);
    route.captureCommunicationsId = DefaultId(enumerator.Get(), eCapture, eCommunications);

    DeviceSnapshot snapshot;
    try {
        snapshot = ReadDeviceSnapshot();
    } catch (...) {
        error = L"BLOCKED: Windows audio device snapshot is unavailable.";
        LogError(error);
        return false;
    }
    const auto name = [](const EndpointDetail& d) {
        return d.Present() ? d.name : std::wstring(L"(not found)");
    };
    LogInfo(L"Windows console output: " + name(snapshot.consoleOutput));
    LogInfo(L"Windows multimedia output: " + name(snapshot.multimediaOutput));
    LogInfo(L"Windows communications output: " + name(snapshot.communicationsOutput));
    LogInfo(L"Default communications microphone: " + name(snapshot.microphone));
    LogInfo(L"VB-CABLE playback / CABLE Input: " + name(snapshot.cableRender));
    LogInfo(L"VB-CABLE recording / CABLE Output: " + name(snapshot.cableCapture));

    route.cableRenderId = snapshot.cableRender.id;
    route.cableCaptureId = snapshot.cableCapture.id;
    route.physicalOutputId = route.consoleId;
    if (route.consoleId.empty() || route.multimediaId.empty() ||
        route.captureConsoleId.empty() || route.captureMultimediaId.empty() ||
        route.captureCommunicationsId.empty()) {
        error = L"BLOCKED: One or more Windows default playback/recording roles are missing. "
                L"Set physical speakers and a physical microphone as Windows defaults.";
        LogError(error);
        return false;
    }
    if (route.cableRenderId.empty() || route.cableCaptureId.empty()) {
        error = L"BLOCKED: VB-CABLE Input or CABLE Output is missing. Install/enable VB-CABLE.";
        LogError(error);
        return false;
    }
    if (route.consoleId == route.cableRenderId || route.multimediaId == route.cableRenderId) {
        error = L"BLOCKED: Console/Multimedia playback is already VB-CABLE. "
                L"Select physical speakers before starting AudioPTT.";
        LogError(error);
        return false;
    }
    if (route.captureCommunicationsId == route.cableCaptureId) {
        error = L"BLOCKED: Windows communications microphone is already CABLE Output. "
                L"Select your physical webcam microphone as Windows default communications input, "
                L"then set Discord Input explicitly to CABLE Output (not Default).";
        LogError(error);
        return false;
    }
    // AudioPTT will not change Windows default input roles. The physical
    // communications microphone is captured directly in microphone mode.
    LogInfo(L"Windows recording defaults will remain unchanged in both modes.");
    if (snapshot.communicationsOutput.id == route.cableRenderId)
        LogWarning(L"Communications playback is VB-CABLE. Set Discord output explicitly "
                   L"to your physical speakers to prevent callers being rebroadcast.");
    LogInfo(L"Audio route saved: webcam -> VB-CABLE while Broadcast OFF. "
            L"Discord Input must be explicitly CABLE Output; Output must be physical speakers.");
    error.clear();
    return true;
}

bool IsDesktopBroadcastRouteActive(const AudioRoute& route) {
    if (route.cableRenderId.empty()) return false;
    ComPtr<IMMDeviceEnumerator> enumerator;
    std::wstring ignored;
    if (!OpenObjects(enumerator, nullptr, ignored)) return false;
    return DefaultId(enumerator.Get(), eRender, eConsole) == route.cableRenderId &&
           DefaultId(enumerator.Get(), eRender, eMultimedia) == route.cableRenderId;
}

bool RouteDesktopToCable(const AudioRoute& route, std::wstring& error) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IPolicyConfig> policy;
    LogInfo(L"Broadcast ON: switching only Windows Console/Multimedia playback "
            L"to CABLE Input. Recording defaults are untouched.");
    if (!OpenObjects(enumerator, &policy, error)) return false;
    if (DefaultId(enumerator.Get(), eRender, eConsole) != route.consoleId ||
        DefaultId(enumerator.Get(), eRender, eMultimedia) != route.multimediaId) {
        error = L"BLOCKED: Windows default playback changed since AudioPTT started. "
                L"Restart with physical speakers selected.";
        LogError(error);
        return false;
    }
    const auto rollback = [&]() {
        const std::wstring original = error;
        std::wstring restoreError;
        const bool restored = RestoreDesktopRoute(route, restoreError);
        error = original;
        if (!restored) error += L" CRITICAL: incomplete rollback: " + restoreError;
    };
    if (!SetRole(policy.Get(), route.cableRenderId, eRender, eConsole, error)) {
        rollback();
        return false;
    }
    if (!SetRole(policy.Get(), route.cableRenderId, eRender, eMultimedia, error)) {
        rollback();
        return false;
    }
    if (!IsDesktopBroadcastRouteActive(route)) {
        error = L"Windows did not apply both VB-CABLE playback defaults.";
        LogError(error);
        rollback();
        return false;
    }
    LogInfo(L"Broadcast ON: playback=CABLE Input; Discord Input stays CABLE Output, "
            L"Windows default microphone remains physical.");
    error.clear();
    return true;
}

bool RestoreDesktopRoute(const AudioRoute& route, std::wstring& error) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IPolicyConfig> policy;
    LogInfo(L"Restoring saved Windows Console/Multimedia playback defaults; "
            L"microphone defaults untouched (except legacy crash recovery).");
    if (!OpenObjects(enumerator, &policy, error)) return false;
    bool success = true;
    std::wstring detail;
    // Only reverse our cable assignments. Respect any manual changes to
    // other endpoints made while AudioPTT was active.
    const auto restore = [&](EDataFlow flow, ERole role,
                             const std::wstring& cableId, const std::wstring& originalId) {
        if (!cableId.empty() && !originalId.empty() &&
            DefaultId(enumerator.Get(), flow, role) == cableId) {
            if (!SetRole(policy.Get(), originalId, flow, role, detail)) success = false;
        }
    };
    restore(eRender, eConsole, route.cableRenderId, route.consoleId);
    restore(eRender, eMultimedia, route.cableRenderId, route.multimediaId);
    // Compatibility: builds before the fixed-cable design changed Windows
    // microphone defaults. Recover those ONLY for an old persisted route;
    // otherwise never change any recording role.
    if (route.restoreLegacyCaptureRoles) {
        LogWarning(L"Restoring microphone defaults from legacy AudioPTT crash recovery.");
        restore(eCapture, eConsole, route.cableCaptureId, route.captureConsoleId);
        restore(eCapture, eMultimedia, route.cableCaptureId, route.captureMultimediaId);
        restore(eCapture, eCommunications, route.cableCaptureId, route.captureCommunicationsId);
    }
    // Missing previous default IDs cannot be treated as a successful restore.
    if (IsAudioRoutedToCable(route)) success = false;
    if (!success && detail.empty())
        detail = L"One or more AudioPTT-owned default roles still point to VB-CABLE.";
    error = success ? L"" : detail;
    if (success) LogInfo(L"AudioPTT-owned default routes restored; Windows mic unchanged.");
    else LogError(L"AudioPTT route restore failed: " + error);
    return success;
}

bool IsAudioRoutedToCable(const AudioRoute& route) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    std::wstring ignored;
    if (!OpenObjects(enumerator, nullptr, ignored)) return false;
    const auto is = [&](EDataFlow flow, ERole role, const std::wstring& id) {
        return !id.empty() && DefaultId(enumerator.Get(), flow, role) == id;
    };
    const bool playbackIsCable =
        is(eRender, eConsole, route.cableRenderId) ||
        is(eRender, eMultimedia, route.cableRenderId);
    if (!route.restoreLegacyCaptureRoles) return playbackIsCable;
    return playbackIsCable ||
           is(eCapture, eConsole, route.cableCaptureId) ||
           is(eCapture, eMultimedia, route.cableCaptureId) ||
           is(eCapture, eCommunications, route.cableCaptureId);
}
} // namespace audioptt
