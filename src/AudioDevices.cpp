#include "AudioDevices.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <stdexcept>

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
} // namespace audioptt
