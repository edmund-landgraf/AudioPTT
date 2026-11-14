#pragma once
#include <string>
#include <vector>

namespace audioptt {
enum class DeviceDirection { Capture, Render };
struct DeviceInfo {
    std::wstring id;
    std::wstring name;
    DeviceDirection direction;
    bool isDefault = false;
};
std::vector<DeviceInfo> EnumerateDevices();
} // namespace audioptt
