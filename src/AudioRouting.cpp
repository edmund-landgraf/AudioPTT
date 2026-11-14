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
std::wstring DefaultId(IMMDeviceEnumerator* enumerator, ERole role) {
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, role, &device))) return {};
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
bool SetRole(IPolicyConfig* policy, const std::wstring& id, ERole role,
             std::wstring& error) {
    if (id.empty()) { error = L"Missing saved audio endpoint ID."; LogError(error); return false; }
    HRESULT hr = policy->SetDefaultEndpoint(id.c_str(), role);
    if (FAILED(hr)) {
        error = L"Windows rejected default playback device change, role=" +
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
    route.consoleId = DefaultId(enumerator.Get(), eConsole);
    route.multimediaId = DefaultId(enumerator.Get(), eMultimedia);
    DeviceSnapshot snapshot;
    try {
        snapshot = ReadDeviceSnapshot();
        const auto fmt = [](const EndpointDetail& d) {
            return d.Present() ? d.name : std::wstring(L"(not found)");
        };
        LogInfo(L"Windows console output: " + fmt(snapshot.consoleOutput));
        LogInfo(L"Windows multimedia output: " + fmt(snapshot.multimediaOutput));
        LogInfo(L"Windows communications output: " + fmt(snapshot.communicationsOutput));
        LogInfo(L"Default communications microphone: " + fmt(snapshot.microphone));
        LogInfo(L"VB-CABLE playback / CABLE Input: " + fmt(snapshot.cableRender));
        LogInfo(L"VB-CABLE recording / CABLE Output: " + fmt(snapshot.cableCapture));
    } catch (...) {
        LogWarning(L"Could not read the complete device snapshot; using basic audio endpoint checks.");
    }
    if (route.consoleId.empty() || route.multimediaId.empty()) {
        error = L"BLOCKED: Windows Console or Multimedia default playback endpoint is missing. Select physical speakers in Windows Sound settings.";
        LogError(error);
        return false;
    }
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) {
        error = L"Cannot enumerate active Windows playback endpoints.";
        LogError(error);
        return false;
    }
    UINT count = 0;
    devices->GetCount(&count);
    route.cableRenderId.clear();
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(devices->Item(i, &device)) &&
            Name(device.Get()).find(L"CABLE INPUT") != std::wstring::npos) {
            route.cableRenderId = EndpointId(device.Get());
            break;
        }
    }
    if (route.cableRenderId.empty()) {
        error = L"BLOCKED: VB-CABLE playback endpoint 'CABLE Input' is missing. Install/enable VB-CABLE and retry.";
        LogError(error);
        return false;
    }
    if (route.consoleId == route.cableRenderId || route.multimediaId == route.cableRenderId) {
        const std::wstring roles = route.consoleId == route.cableRenderId &&
                                   route.multimediaId == route.cableRenderId
                                       ? L"Console AND Multimedia"
                                       : (route.consoleId == route.cableRenderId
                                              ? L"Console" : L"Multimedia");
        error = L"BLOCKED: Windows " + roles +
                L" output is already VB-CABLE (CABLE Input). Set Windows default playback to your PHYSICAL speakers/headphones, then click Retry audio setup.";
        LogError(error);
        return false;
    }
    if (!snapshot.cableCapture.Present()) {
        error = L"BLOCKED: VB-CABLE recording endpoint 'CABLE Output' is missing. Confirm the virtual cable driver is installed and enabled.";
        LogError(error);
        return false;
    }
    if (!snapshot.microphone.Present() || snapshot.microphone.id == snapshot.cableCapture.id) {
        error = L"BLOCKED: Windows default communications microphone is missing or set to CABLE Output. Select your physical microphone as the Windows default communications input.";
        LogError(error);
        return false;
    }
    if (snapshot.communicationsOutput.id == route.cableRenderId)
        LogWarning(L"Communications playback is VB-CABLE. Discord incoming voices may be rebroadcast; select your physical speakers as Discord Output.");
    LogInfo(L"Startup routing checks passed. Saved physical speaker ID and VB-CABLE render ID.");
    route.physicalOutputId = route.consoleId;
    error.clear();
    return true;
}
bool RouteDesktopToCable(const AudioRoute& route, std::wstring& error) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IPolicyConfig> policy;
    LogInfo(L"Broadcast ON: switching Windows console/multimedia output to VB-CABLE.");
    if (!OpenObjects(enumerator, &policy, error)) return false;
    // Do not override output changes made manually since AudioPTT started.
    if (DefaultId(enumerator.Get(), eConsole) != route.consoleId ||
        DefaultId(enumerator.Get(), eMultimedia) != route.multimediaId) {
        error = L"BLOCKED: Windows playback default changed since AudioPTT started. Retry setup with physical speakers selected.";
        LogError(error);
        return false;
    }
    if (!SetRole(policy.Get(), route.cableRenderId, eConsole, error)) return false;
    if (!SetRole(policy.Get(), route.cableRenderId, eMultimedia, error)) {
        std::wstring ignored;
        RestoreDesktopRoute(route, ignored);
        return false;
    }
    if (DefaultId(enumerator.Get(), eConsole) != route.cableRenderId ||
        DefaultId(enumerator.Get(), eMultimedia) != route.cableRenderId) {
        error = L"Windows did not apply the VB-CABLE playback change.";
        LogError(error);
        std::wstring ignored;
        RestoreDesktopRoute(route, ignored);
        return false;
    }
    LogInfo(L"Broadcast ON: Windows Console and Multimedia now route to CABLE Input.");
    error.clear();
    return true;
}
bool RestoreDesktopRoute(const AudioRoute& route, std::wstring& error) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IPolicyConfig> policy;
    LogInfo(L"Restoring Windows playback defaults to the saved physical devices.");
    if (!OpenObjects(enumerator, &policy, error)) return false;
    bool success = true;
    std::wstring detail;
    // Never undo manual device changes made by the user while AudioPTT ran.
    if (DefaultId(enumerator.Get(), eConsole) == route.cableRenderId)
        success = SetRole(policy.Get(), route.consoleId, eConsole, detail) && success;
    if (DefaultId(enumerator.Get(), eMultimedia) == route.cableRenderId)
        success = SetRole(policy.Get(), route.multimediaId, eMultimedia, detail) && success;
    // Check actual post-change defaults. Don't report success if Windows
    // silently refused an operation or kept the cable selected.
    const bool restored = DefaultId(enumerator.Get(), eConsole) != route.cableRenderId &&
                          DefaultId(enumerator.Get(), eMultimedia) != route.cableRenderId;
    success = success && restored;
    if (!success && detail.empty()) detail = L"One or more Windows audio playback roles still point to VB-CABLE.";
    error = success ? L"" : detail;
    if (success) LogInfo(L"Windows playback restored (no console/multimedia role left on the cable).");
    else LogError(L"Playback restore failed: " + error);
    return success;
}
bool IsAudioRoutedToCable(const AudioRoute& route) {
    if (route.cableRenderId.empty()) return false;
    ComPtr<IMMDeviceEnumerator> enumerator;
    std::wstring ignored;
    if (!OpenObjects(enumerator, nullptr, ignored)) return false;
    return DefaultId(enumerator.Get(), eConsole) == route.cableRenderId ||
           DefaultId(enumerator.Get(), eMultimedia) == route.cableRenderId;
}
} // namespace audioptt
