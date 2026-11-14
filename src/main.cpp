#include <windows.h>
#include <objbase.h>
#include <string>
#include <sstream>
#include "AudioEngine.h"
#include "AudioDevices.h"
#include "AudioRouteStore.h"

namespace {
constexpr int kButton = 101;
constexpr int kMonitor = 102;
constexpr int kDevices = 103;
constexpr int kStatus = 104;
constexpr int kRetry = 107;
constexpr UINT_PTR kStartupTimer = 105;
constexpr UINT_PTR kHealthTimer = 106;
constexpr int kHotkey = 201;

audioptt::AudioEngine engine;
HWND broadcastButton = nullptr;
HWND monitorCheck = nullptr;
HWND statusLabel = nullptr;
HWND hotkeyLabel = nullptr;

void Refresh() {
    SetWindowTextW(broadcastButton, engine.Broadcasting() ?
                   L"STOP BROADCAST" : L"BROADCAST PC AUDIO");
    const std::wstring status = engine.Status();
    SetWindowTextW(statusLabel, status.c_str());
    SendMessageW(monitorCheck, BM_SETCHECK,
                 engine.Monitor() ? BST_CHECKED : BST_UNCHECKED, 0);
}
void ShowDevices(HWND parent) {
    try {
        const auto devices = audioptt::EnumerateDevices();
        std::wostringstream out;
        out << L"ACTIVE WINDOWS AUDIO ENDPOINTS\r\n\r\n";
        for (const auto& device : devices) {
            out << (device.direction == audioptt::DeviceDirection::Capture ?
                    L"[MIC] " : L"[PLAYBACK] ")
                << device.name << (device.isDefault ? L" (default)" : L"")
                << L"\r\nID: " << device.id << L"\r\n\r\n";
        }
        if (devices.empty()) out << L"No active endpoints found.";
        MessageBoxW(parent, out.str().c_str(), L"Audio endpoints",
                    MB_OK | MB_ICONINFORMATION);
    } catch (...) {
        MessageBoxW(parent, L"Unable to enumerate audio devices.",
                    L"AudioPTT", MB_OK | MB_ICONERROR);
    }
}
void SetupAudio(HWND hwnd) {
    std::wstring error;
    if (engine.Start(error)) {
        Refresh();
        return;
    }

    // Only offer crash recovery when we have a previous route snapshot AND
    // a Windows playback role still points to its VB-CABLE endpoint.
    audioptt::AudioRoute saved;
    if (audioptt::LoadRecoveryRoute(saved) &&
        audioptt::IsAudioRoutedToCable(saved)) {
        if (MessageBoxW(hwnd,
                L"Windows output is still routed through VB-CABLE from an earlier AudioPTT session.\n\n"
                L"Restore your previous speakers/headphones now?",
                L"AudioPTT - Restore Windows audio",
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON1) == IDYES) {
            std::wstring restoreError;
            if (audioptt::RestoreDesktopRoute(saved, restoreError)) {
                audioptt::ClearRecoveryRoute();
                if (engine.Start(error)) {
                    Refresh();
                    return;
                }
            } else {
                error = L"Could not restore Windows output: " + restoreError;
            }
        }
    }
    if (!error.empty())
        MessageBoxW(hwnd, error.c_str(), L"AudioPTT audio setup",
                    MB_OK | MB_ICONWARNING);
    Refresh();
}
void Toggle(HWND hwnd) {
    std::wstring error;
    if (!engine.SetBroadcast(!engine.Broadcasting(), error) && !error.empty())
        MessageBoxW(hwnd, error.c_str(), L"Audio routing error",
                    MB_OK | MB_ICONWARNING);
    Refresh();
}
LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE: {
        HINSTANCE instance = reinterpret_cast<LPCREATESTRUCTW>(lp)->hInstance;
        CreateWindowW(L"STATIC", L"AudioPTT", WS_CHILD | WS_VISIBLE,
                      20, 18, 340, 30, hwnd, nullptr, instance, nullptr);
        broadcastButton = CreateWindowW(L"BUTTON", L"BROADCAST PC AUDIO",
                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      20, 60, 340, 54, hwnd,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButton)),
                      instance, nullptr);
        monitorCheck = CreateWindowW(L"BUTTON", L"Monitor PC audio through speakers",
                      WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                      20, 128, 350, 28, hwnd,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMonitor)),
                      instance, nullptr);
        statusLabel = CreateWindowW(L"STATIC", L"Starting...",
                      WS_CHILD | WS_VISIBLE, 20, 173, 355, 44, hwnd,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatus)),
                      instance, nullptr);
        CreateWindowW(L"BUTTON", L"List audio devices",
                      WS_CHILD | WS_VISIBLE, 20, 222, 165, 35, hwnd,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDevices)),
                      instance, nullptr);
        CreateWindowW(L"BUTTON", L"Retry audio setup",
                      WS_CHILD | WS_VISIBLE, 195, 222, 165, 35, hwnd,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRetry)),
                      instance, nullptr);
        hotkeyLabel = CreateWindowW(L"STATIC", L"",
                      WS_CHILD | WS_VISIBLE, 20, 274, 355, 28,
                      hwnd, nullptr, instance, nullptr);
        const bool hotkeyRegistered = RegisterHotKey(
            hwnd, kHotkey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SPACE) != FALSE;
        SetWindowTextW(hotkeyLabel, hotkeyRegistered ?
                       L"Hotkey: Ctrl + Alt + Space" :
                       L"Hotkey unavailable (used by another application)");
        SetTimer(hwnd, kStartupTimer, 200, nullptr);
        SetTimer(hwnd, kHealthTimer, 750, nullptr);
        Refresh();
        return 0;
    }
    case WM_TIMER:
        if (wp == kStartupTimer) {
            KillTimer(hwnd, kStartupTimer);
            SetupAudio(hwnd);
            return 0;
        }
        if (wp == kHealthTimer) {
            std::wstring alert;
            if (!engine.CheckHealth(alert) && !alert.empty())
                MessageBoxW(hwnd, alert.c_str(), L"AudioPTT - Playback recovered",
                            MB_OK | MB_ICONWARNING);
            Refresh();
            return 0;
        }
        break;
    case WM_HOTKEY:
        if (wp == kHotkey) {
            Toggle(hwnd);
            return 0;
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kButton:
            Toggle(hwnd);
            return 0;
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
        case kRetry:
            SetupAudio(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE: {
        engine.Stop();
        const std::wstring status = engine.Status();
        if (status.find(L"WARNING: Windows playback was not restored") != std::wstring::npos) {
            MessageBoxW(hwnd,
                L"Windows output could not be restored automatically.\n"
                L"Open Settings > System > Sound and select your physical speakers/headphones.",
                L"AudioPTT - Audio routing warning", MB_OK | MB_ICONWARNING);
        }
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
        UnregisterHotKey(hwnd, kHotkey);
        KillTimer(hwnd, kStartupTimer);
        KillTimer(hwnd, kHealthTimer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // Prevent competing copies from changing system defaults behind each other.
    HANDLE singleton = CreateMutexW(nullptr, FALSE, L"Local\\AudioPTT.SingleInstance");
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"AudioPTT is already running.",
                    L"AudioPTT", MB_OK | MB_ICONINFORMATION);
        if (singleton) CloseHandle(singleton);
        return 1;
    }
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Windows COM initialization failed.",
                    L"AudioPTT", MB_OK | MB_ICONERROR);
        CloseHandle(singleton);
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
        CloseHandle(singleton);
        return 1;
    }
    HWND window = CreateWindowExW(0, className, L"AudioPTT - Discord Audio Router",
                                 WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 400, 365,
                                 nullptr, nullptr, instance, nullptr);
    if (!window) {
        CoUninitialize();
        CloseHandle(singleton);
        return 1;
    }
    ShowWindow(window, show);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    CloseHandle(singleton);
    return static_cast<int>(msg.wParam);
}
