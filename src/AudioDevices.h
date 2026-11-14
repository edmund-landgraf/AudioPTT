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
struct EndpointDetail {
    std::wstring name;
    std::wstring id;
    bool Present() const { return !id.empty(); }
};
struct DeviceSnapshot {
    EndpointDetail consoleOutput;
    EndpointDetail multimediaOutput;
    EndpointDetail communicationsOutput;
    EndpointDetail microphone;
    EndpointDetail cableRender;  // CABLE Input (audio supplied by AudioPTT)
    EndpointDetail cableCapture; // CABLE Output (select in Discord)
};
DeviceSnapshot ReadDeviceSnapshot();
} // namespace audioptt
