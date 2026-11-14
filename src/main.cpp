#include <windows.h>
#include <objbase.h>
#include <string>
#include <sstream>
#include "AudioEngine.h"
#include "AudioDevices.h"

namespace {
constexpr int kButton = 101;
constexpr int kMonitor = 102;
constexpr int kDevices = 103;
constexpr int kStatus = 104;
constexpr UINT_PTR kTimer = 105;
constexpr UINT_PTR kRefreshTimer = 106;
audioptt::AudioEngine engine;
HWND broadcastButton = nullptr;
HWND monitorCheck = nullptr;
HWND statusLabel = nullptr;

void Refresh() {
    SetWindowTextW(broadcastButton, engine.Broadcasting() ? L"STOP BROADCAST" : L"BROADCAST PC AUDIO");
    std::wstring status = engine.Status();
    SetWindowTextW(statusLabel, status.c_str());
    SendMessageW(monitorCheck, BM_SETCHECK, engine.Monitor() ? BST_CHECKED : BST_UNCHECKED, 0);
}
void ShowDevices(HWND parent) {
    try {
        const auto devices = audioptt::EnumerateDevices();
        std::wostringstream out;
        out << L"ACTIVE WINDOWS AUDIO ENDPOINTS\r\n\r\n";
        for (const auto& device : devices) {
            out << (device.direction == audioptt::DeviceDirection::Capture ? L"[MIC] " : L"[PLAYBACK] ")
                << device.name << (device.isDefault ? L" (default)" : L"")
                << L"\r\nID: " << device.id << L"\r\n\r\n";
        }
        if (devices.empty()) out << L"No active endpoints found.";
        MessageBoxW(parent, out.str().c_str(), L"Audio endpoints", MB_OK | MB_ICONINFORMATION);
    } catch (...) {
        MessageBoxW(parent, L"Unable to enumerate audio devices.", L"AudioPTT", MB_OK | MB_ICONERROR);
    }
}
LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE: {
        HINSTANCE instance = reinterpret_cast<LPCREATESTRUCTW>(lp)->hInstance;
        CreateWindowW(L"STATIC", L"AudioPTT", WS_CHILD | WS_VISIBLE, 20, 18, 340, 30,
                      hwnd, nullptr, instance, nullptr);
        broadcastButton = CreateWindowW(L"BUTTON", L"BROADCAST PC AUDIO",
                           WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20, 60, 340, 54,
                           hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButton)), instance, nullptr);
        monitorCheck = CreateWindowW(L"BUTTON", L"Monitor PC audio through speakers",
                           WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 20, 128, 350, 28,
                           hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMonitor)), instance, nullptr);
        statusLabel = CreateWindowW(L"STATIC", L"Starting...",
                           WS_CHILD | WS_VISIBLE, 20, 173, 355, 36,
                           hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatus)), instance, nullptr);
        CreateWindowW(L"BUTTON", L"List audio devices",
                      WS_CHILD | WS_VISIBLE, 20, 225, 340, 35,
                      hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDevices)), instance, nullptr);
        // Defer hardware initialization until the window is visible.
        SetTimer(hwnd, kTimer, 200, nullptr);
        Refresh();
        SetTimer(hwnd, kRefreshTimer, 600, nullptr);
        return 0;
    }
    case WM_TIMER:
        if (wp == kRefreshTimer) {
            Refresh();
            return 0;
        }
        if (wp == kTimer) {
            KillTimer(hwnd, kTimer);
            std::wstring error;
            if (!engine.Start(error) && !error.empty())
                MessageBoxW(hwnd, error.c_str(), L"Audio routing failed", MB_OK | MB_ICONWARNING);
            Refresh();
            return 0;
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kButton: {
            std::wstring error;
            if (!engine.SetBroadcast(!engine.Broadcasting(), error) && !error.empty())
                MessageBoxW(hwnd, error.c_str(), L"Routing not available", MB_OK | MB_ICONWARNING);
            Refresh();
            return 0;
        }
        case kMonitor: {
            std::wstring error;
            const bool checked = SendMessageW(monitorCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (!engine.SetMonitor(checked, error) && !error.empty())
                MessageBoxW(hwnd, error.c_str(), L"Monitoring", MB_OK | MB_ICONWARNING);
            Refresh();
            return 0;
        }
        case kDevices:
            ShowDevices(hwnd);
            return 0;
        }
        break;
    case WM_DESTROY:
        engine.Stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Windows COM initialization failed.", L"AudioPTT", MB_ICONERROR);
        return 1;
    }
    const wchar_t* className = L"AudioPTT.MainWindow";
    WNDCLASSW cls{};
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = instance;
    cls.lpszClassName = className;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&cls)) {
        CoUninitialize();
        return 1;
    }
    HWND window = CreateWindowExW(0, className, L"AudioPTT - Discord Audio Router",
                                 WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 400, 320,
                                 nullptr, nullptr, instance, nullptr);
    if (!window) {
        CoUninitialize();
        return 1;
    }
    ShowWindow(window, show);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
