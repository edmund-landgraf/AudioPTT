#include "AudioRouting.h"
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
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, INT, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, INT, const PROPERTYKEY&, const PROPVARIANT*) = 0;
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
    if (FAILED(hr)) { error = L"Unable to enumerate Windows audio devices."; return false; }
    if (policy) {
        hr = CoCreateInstance(clsidPolicy, nullptr, CLSCTX_ALL, iidPolicy,
                              reinterpret_cast<void**>(policy->GetAddressOf()));
        if (FAILED(hr)) {
            error = L"Windows default audio routing API is unavailable on this system.";
            return false;
        }
    }
    return true;
}
bool SetRole(IPolicyConfig* policy, const std::wstring& id, ERole role,
             std::wstring& error) {
    if (id.empty()) { error = L"Missing saved audio endpoint ID."; return false; }
    HRESULT hr = policy->SetDefaultEndpoint(id.c_str(), role);
    if (FAILED(hr)) {
        error = L"Windows rejected default playback device change.";
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
    if (route.consoleId.empty() || route.multimediaId.empty()) {
        error = L"Physical speaker defaults are missing. Set Windows playback to your speakers before starting.";
        return false;
    }
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) {
        error = L"Unable to find active playback endpoints.";
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
        error = L"VB-CABLE's CABLE Input playback endpoint was not found.";
        return false;
    }
    if (route.consoleId == route.cableRenderId || route.multimediaId == route.cableRenderId) {
        error = L"Set both Windows default and multimedia playback to physical speakers before launching AudioPTT.";
        return false;
    }
    route.physicalOutputId = route.consoleId;
    error.clear();
    return true;
}
bool RouteDesktopToCable(const AudioRoute& route, std::wstring& error) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IPolicyConfig> policy;
    if (!OpenObjects(enumerator, &policy, error)) return false;
    if (!SetRole(policy.Get(), route.cableRenderId, eConsole, error)) return false;
    if (!SetRole(policy.Get(), route.cableRenderId, eMultimedia, error)) {
        std::wstring ignored;
        SetRole(policy.Get(), route.consoleId, eConsole, ignored);
        return false;
    }
    error.clear();
    return true;
}
bool RestoreDesktopRoute(const AudioRoute& route, std::wstring& error) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IPolicyConfig> policy;
    if (!OpenObjects(enumerator, &policy, error)) return false;
    bool success = true;
    std::wstring detail;
    // Never undo manual device changes made by the user while AudioPTT ran.
    if (DefaultId(enumerator.Get(), eConsole) == route.cableRenderId)
        success = SetRole(policy.Get(), route.consoleId, eConsole, detail) && success;
    if (DefaultId(enumerator.Get(), eMultimedia) == route.cableRenderId)
        success = SetRole(policy.Get(), route.multimediaId, eMultimedia, detail) && success;
    error = success ? L"" : detail;
    return success;
}
} // namespace audioptt
