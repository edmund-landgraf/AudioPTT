#include "AudioDevices.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <stdexcept>
#include <algorithm>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace audioptt {
std::vector<DeviceInfo> EnumerateDevices() {
    std::vector<DeviceInfo> devices;
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) throw std::runtime_error("Unable to create Windows audio device enumerator");
    for (auto flow : {eCapture, eRender}) {
        ComPtr<IMMDevice> defaultDevice;
        std::wstring defaultId;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &defaultDevice))) {
            LPWSTR raw = nullptr;
            if (SUCCEEDED(defaultDevice->GetId(&raw))) {
                defaultId = raw;
                CoTaskMemFree(raw);
            }
        }
        ComPtr<IMMDeviceCollection> collection;
        hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
        if (FAILED(hr)) continue;
        UINT count = 0;
        if (FAILED(collection->GetCount(&count))) continue;
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(i, &device))) continue;
            LPWSTR rawId = nullptr;
            if (FAILED(device->GetId(&rawId))) continue;
            std::wstring id = rawId;
            CoTaskMemFree(rawId);
            std::wstring name = L"(unnamed device)";
            ComPtr<IPropertyStore> props;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value))) {
                    if (value.vt == VT_LPWSTR && value.pwszVal) name = value.pwszVal;
                }
                PropVariantClear(&value);
            }
            devices.push_back({id, name, flow == eCapture ? DeviceDirection::Capture
                                                         : DeviceDirection::Render, id == defaultId});
        }
    }
    return devices;
}

namespace {
EndpointDetail Lookup(IMMDeviceEnumerator* enumerator, EDataFlow flow, ERole role) {
    EndpointDetail result;
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, role, &device))) return result;
    LPWSTR id = nullptr;
    if (SUCCEEDED(device->GetId(&id))) {
        result.id = id;
        CoTaskMemFree(id);
    }
    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT val;
        PropVariantInit(&val);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &val)) &&
            val.vt == VT_LPWSTR && val.pwszVal) result.name = val.pwszVal;
        PropVariantClear(&val);
    }
    return result;
}
bool ContainsInsensitive(std::wstring name, const std::wstring& fragment) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    return name.find(fragment) != std::wstring::npos;
}
}
DeviceSnapshot ReadDeviceSnapshot() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator))))
        throw std::runtime_error("Windows audio device enumeration is unavailable");
    DeviceSnapshot result;
    result.consoleOutput = Lookup(enumerator.Get(), eRender, eConsole);
    result.multimediaOutput = Lookup(enumerator.Get(), eRender, eMultimedia);
    result.communicationsOutput = Lookup(enumerator.Get(), eRender, eCommunications);
    result.microphone = Lookup(enumerator.Get(), eCapture, eCommunications);
    for (const auto flow : {eRender, eCapture}) {
        ComPtr<IMMDeviceCollection> collection;
        if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection))) continue;
        UINT count = 0;
        if (FAILED(collection->GetCount(&count))) continue;
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(i, &device))) continue;
            EndpointDetail detail;
            LPWSTR raw = nullptr;
            if (FAILED(device->GetId(&raw))) continue;
            detail.id = raw;
            CoTaskMemFree(raw);
            ComPtr<IPropertyStore> props;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT val;
                PropVariantInit(&val);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &val)) &&
                    val.vt == VT_LPWSTR && val.pwszVal) detail.name = val.pwszVal;
                PropVariantClear(&val);
            }
            if (flow == eRender && !result.cableRender.Present() &&
                ContainsInsensitive(detail.name, L"CABLE INPUT"))
                result.cableRender = detail;
            if (flow == eCapture && !result.cableCapture.Present() &&
                ContainsInsensitive(detail.name, L"CABLE OUTPUT"))
                result.cableCapture = detail;
        }
    }
    return result;
}
} // namespace audioptt
