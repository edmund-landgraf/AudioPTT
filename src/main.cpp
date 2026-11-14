#include <windows.h>
#include <commctrl.h>
#include <cstring>
#include <cstdint>
#include <objbase.h>
#include <string>
#include <sstream>
#include <vector>
#include "AudioEngine.h"
#include "AudioDevices.h"
#include "AudioRouteStore.h"
#include "Diagnostics.h"

namespace {
constexpr int kButton = 101;
constexpr int kMonitor = 102;
constexpr int kDevices = 103;
constexpr int kRetry = 107;
constexpr int kCopyLogs = 108;
constexpr int kClearLogs = 109;
constexpr int kTabs = 110;
constexpr UINT_PTR kStartupTimer = 201;
constexpr UINT_PTR kHealthTimer = 202;
constexpr int kHotkey = 203;

audioptt::AudioEngine engine;
HWND tabs = nullptr;
HWND broadcastButton = nullptr;
HWND monitorCheck = nullptr;
HWND statusLabel = nullptr;
HWND routeDetails = nullptr;
HWND logEdit = nullptr;
std::vector<HWND> mainControls;
std::vector<HWND> logControls;
std::wstring uiProblem;
std::wstring lastRouteText;
std::uint64_t lastLogRevision = static_cast<std::uint64_t>(-1);
int selectedTab = 0;

HWND AddControl(std::vector<HWND>& page, const wchar_t* className,
                const wchar_t* caption, DWORD style, int x, int y, int w, int h,
                HWND parent, HINSTANCE instance, int id = 0, DWORD exStyle = 0) {
    HWND child = CreateWindowExW(exStyle, className, caption, WS_CHILD | style,
                                 x, y, w, h, parent,
                                 id ? reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)) : nullptr,
                                 instance, nullptr);
    if (child) {
        page.push_back(child);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    }
    return child;
}
std::wstring Friendly(const audioptt::EndpointDetail& endpoint) {
    return endpoint.Present() ? (endpoint.name.empty() ? L"(unnamed endpoint)" : endpoint.name)
                              : L"(not detected)";
}
std::wstring ShortName(const audioptt::EndpointDetail& endpoint) {
    std::wstring name = Friendly(endpoint);
    constexpr size_t max = 38;
    if (name.size() > max) name = name.substr(0, max - 3) + L"...";
    return name;
}
void RefreshDevices() {
    if (!routeDetails) return;
    try {
        const auto snapshot = audioptt::ReadDeviceSnapshot();
        const auto cable = snapshot.cableRender.id;
        const auto label = [&cable](const audioptt::EndpointDetail& endpoint) {
            return (endpoint.Present() && endpoint.id == cable ? L"[VB-CABLE] " : L"") +
                   ShortName(endpoint);
        };
        // These are LIVE Windows endpoints. Discord configuration cannot be
        // inspected from here; its microphone line is explicitly instructional.
        std::wstring monitorOutput = L"(not selected)";
        if (engine.Broadcasting()) {
            try {
                for (const auto& device : audioptt::EnumerateDevices()) {
                    if (device.id == engine.PhysicalOutputId()) {
                        monitorOutput = device.name;
                        break;
                    }
                }
            } catch (...) {}
        } else {
            monitorOutput = Friendly(snapshot.consoleOutput);
        }
        if (monitorOutput.size() > 38) monitorOutput = monitorOutput.substr(0, 35) + L"...";
        std::wstring view =
            L"Physical microphone:  " + ShortName(snapshot.microphone) +
            L"\r\nWindows default out: " + label(snapshot.consoleOutput) +
            L"\r\nMedia playback out:  " + label(snapshot.multimediaOutput) +
            L"\r\nDiscord voice out:  " + ShortName(snapshot.communicationsOutput) +
            L"\r\nVB-CABLE playback:   " + ShortName(snapshot.cableRender) +
            L"\r\nVB-CABLE recording:  " + ShortName(snapshot.cableCapture) +
            L"\r\nDiscord input:      set to CABLE Output in Discord" +
            L"\r\nMonitor output:     " +
            (engine.Broadcasting() && !engine.Monitor() ?
                std::wstring(L"OFF (PC silent locally)") : monitorOutput);
        if (view != lastRouteText) {
            SetWindowTextW(routeDetails, view.c_str());
            lastRouteText = std::move(view);
        }
    } catch (...) {
        const std::wstring error = L"Unable to read Windows audio device settings.";
        if (lastRouteText != error) {
            SetWindowTextW(routeDetails, error.c_str());
            lastRouteText = error;
            audioptt::LogError(error);
        }
    }
}
void RefreshLogs(bool force = false) {
    if (!logEdit || selectedTab != 1) return;
    const auto revision = audioptt::LogRevision();
    if (!force && revision == lastLogRevision) return;
    lastLogRevision = revision;
    const auto log = audioptt::LogText();
    SetWindowTextW(logEdit, log.c_str());
    SendMessageW(logEdit, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SendMessageW(logEdit, EM_SCROLLCARET, 0, 0);
}
void Refresh() {
    if (broadcastButton)
        SetWindowTextW(broadcastButton, engine.Broadcasting() ?
                       L"STOP BROADCAST" : L"BROADCAST PC AUDIO");
    if (monitorCheck)
        SendMessageW(monitorCheck, BM_SETCHECK, engine.Monitor() ? BST_CHECKED : BST_UNCHECKED, 0);
    if (statusLabel) {
        const std::wstring state = uiProblem.empty() ? engine.Status() : uiProblem;
        const std::wstring status = (engine.Running() ? L"STATUS: " : L"SETUP: ") + state;
        SetWindowTextW(statusLabel, status.c_str());
    }
    RefreshLogs();
}
void SetPage(int index) {
    selectedTab = index;
    for (const HWND control : mainControls) ShowWindow(control, index == 0 ? SW_SHOW : SW_HIDE);
    for (const HWND control : logControls) ShowWindow(control, index == 1 ? SW_SHOW : SW_HIDE);
    if (index == 1) RefreshLogs(true);
    else RefreshDevices();
}
void ShowDevicesInLog() {
    audioptt::LogInfo(L"---- Full active audio endpoint list ----");
    try {
        for (const auto& device : audioptt::EnumerateDevices()) {
            audioptt::LogInfo(std::wstring(device.direction == audioptt::DeviceDirection::Capture ?
                                L"[Capture] " : L"[Playback] ") +
                                device.name + (device.isDefault ? L" [console default]" : L""));
            audioptt::LogInfo(L"    Endpoint ID: " + device.id);
        }
    } catch (...) {
        audioptt::LogError(L"Unable to enumerate the complete audio endpoint list.");
    }
    TabCtrl_SetCurSel(tabs, 1);
    SetPage(1);
}
void SetupAudio(HWND hwnd) {
    audioptt::LogInfo(L"---- Audio setup / retry requested ----");
    uiProblem.clear();
    std::wstring error;
    if (engine.Start(error)) {
        audioptt::LogInfo(L"Audio setup ready: physical microphone forwarded to VB-CABLE Input.");
        RefreshDevices();
        Refresh();
        return;
    }
    if (!error.empty()) audioptt::LogError(L"Startup blocked: " + error);

    // Offer recovery only when there is a saved previous route AND one of the
    // Windows output roles still selects its cable endpoint.
    audioptt::AudioRoute saved;
    if (audioptt::LoadRecoveryRoute(saved) &&
        audioptt::IsAudioRoutedToCable(saved)) {
        audioptt::LogWarning(L"Previous session left a recoverable VB-CABLE default output.");
        const int answer = MessageBoxW(hwnd,
            L"AudioPTT found a saved physical-speaker route from an earlier session.\n"
            L"Windows is still sending audio to VB-CABLE.\n\n"
            L"Restore the previous speakers now?",
            L"AudioPTT - Restore audio output",
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON1);
        if (answer == IDYES) {
            audioptt::LogInfo(L"Recovery approved: restoring saved Windows playback defaults.");
            std::wstring restoreError;
            if (audioptt::RestoreDesktopRoute(saved, restoreError)) {
                audioptt::ClearRecoveryRoute();
                audioptt::LogInfo(L"Recovered previous playback route. Retrying audio setup.");
                if (engine.Start(error)) {
                    uiProblem.clear();
                    RefreshDevices();
                    Refresh();
                    return;
                }
                audioptt::LogError(L"Audio setup after recovery failed: " + error);
            } else {
                error = L"Could not restore previous Windows output: " + restoreError;
                audioptt::LogError(error);
            }
        } else {
            audioptt::LogWarning(L"User declined automatic playback recovery.");
        }
    }
    uiProblem = error.empty() ? L"Unknown audio setup failure; inspect Logs." : error;
    RefreshDevices();
    Refresh();
}
void Toggle() {
    audioptt::LogInfo(engine.Broadcasting() ? L"STOP broadcast requested." :
                                               L"START broadcast requested.");
    std::wstring error;
    if (!engine.SetBroadcast(!engine.Broadcasting(), error)) {
        uiProblem = error;
        audioptt::LogError(L"Broadcast toggle blocked: " + error);
    } else {
        uiProblem.clear();
        audioptt::LogInfo(engine.Broadcasting() ? L"Desktop audio broadcast active." :
                                                   L"Microphone mode restored.");
    }
    RefreshDevices();
    Refresh();
}
void CopyLogs(HWND hwnd) {
    const auto content = audioptt::LogText();
    if (!OpenClipboard(hwnd)) {
        audioptt::LogWarning(L"Could not open Windows clipboard to copy logs.");
        return;
    }
    EmptyClipboard();
    const SIZE_T bytes = (content.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        void* target = GlobalLock(memory);
        if (target) {
            memcpy(target, content.c_str(), bytes);
            GlobalUnlock(memory);
            if (SetClipboardData(CF_UNICODETEXT, memory)) memory = nullptr;
        }
        if (memory) GlobalFree(memory);
    }
    CloseClipboard();
}
LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE: {
        HINSTANCE instance = reinterpret_cast<LPCREATESTRUCTW>(lp)->hInstance;
        tabs = CreateWindowExW(0, WC_TABCONTROLW, L"",
                  WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
                  12, 12, 534, 558, hwnd,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTabs)), instance, nullptr);
        SendMessageW(tabs, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<LPWSTR>(L"Main");
        TabCtrl_InsertItem(tabs, 0, &item);
        item.pszText = const_cast<LPWSTR>(L"Logs");
        TabCtrl_InsertItem(tabs, 1, &item);

        AddControl(mainControls, L"STATIC", L"AudioPTT  -  Mic / PC audio switch",
                   WS_VISIBLE, 28, 51, 485, 24, hwnd, instance);
        broadcastButton = AddControl(mainControls, L"BUTTON", L"BROADCAST PC AUDIO",
                   WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 28, 78, 495, 55,
                   hwnd, instance, kButton);
        monitorCheck = AddControl(mainControls, L"BUTTON", L"Monitor PC audio through speakers",
                   WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 28, 142, 480, 27,
                   hwnd, instance, kMonitor);
        AddControl(mainControls, L"BUTTON", L"Live audio settings",
                   WS_VISIBLE | BS_GROUPBOX, 28, 176, 495, 264, hwnd, instance);
        routeDetails = AddControl(mainControls, L"STATIC", L"Reading Windows audio devices...",
                   WS_VISIBLE | SS_LEFTNOWORDWRAP, 42, 198, 470, 230, hwnd, instance);
        statusLabel = AddControl(mainControls, L"STATIC", L"Starting audio...",
                   WS_VISIBLE | SS_LEFT, 28, 448, 495, 44, hwnd, instance);
        AddControl(mainControls, L"BUTTON", L"Device IDs / diagnostics",
                   WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                   28, 500, 234, 34, hwnd, instance, kDevices);
        AddControl(mainControls, L"BUTTON", L"Retry audio setup",
                   WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                   274, 500, 249, 34, hwnd, instance, kRetry);
        AddControl(mainControls, L"STATIC", L"Hotkey: Ctrl + Alt + Space",
                   WS_VISIBLE, 28, 545, 495, 20, hwnd, instance);

        logEdit = AddControl(logControls, L"EDIT", L"",
                   WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
                   ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP,
                   28, 52, 495, 440, hwnd, instance, 0, WS_EX_CLIENTEDGE);
        SendMessageW(logEdit, EM_SETLIMITTEXT, 1000000, 0);
        AddControl(logControls, L"BUTTON", L"Copy logs",
                   WS_TABSTOP | BS_PUSHBUTTON, 28, 500, 234, 34,
                   hwnd, instance, kCopyLogs);
        AddControl(logControls, L"BUTTON", L"Clear logs",
                   WS_TABSTOP | BS_PUSHBUTTON, 274, 500, 249, 34,
                   hwnd, instance, kClearLogs);
        AddControl(logControls, L"STATIC",
                   L"Recent events (last 400); includes audio routing failures.",
                   0, 28, 545, 495, 20, hwnd, instance);

        SetPage(0);
        const bool hotkey = RegisterHotKey(
            hwnd, kHotkey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SPACE) != FALSE;
        if (!hotkey) audioptt::LogWarning(L"Ctrl+Alt+Space is reserved by another program.");
        audioptt::LogInfo(L"AudioPTT started. A VB-CABLE driver is required in this prototype.");
        audioptt::LogInfo(L"Discord input must be set manually to CABLE Output. Discord output should use physical speakers.");
        SetTimer(hwnd, kStartupTimer, 200, nullptr);
        SetTimer(hwnd, kHealthTimer, 750, nullptr);
        RefreshDevices();
        Refresh();
        return 0;
    }
    case WM_NOTIFY: {
        auto* header = reinterpret_cast<LPNMHDR>(lp);
        if (header && header->hwndFrom == tabs && header->code == TCN_SELCHANGE) {
            SetPage(TabCtrl_GetCurSel(tabs));
            return 0;
        }
        break;
    }
    case WM_TIMER:
        if (wp == kStartupTimer) {
            KillTimer(hwnd, kStartupTimer);
            SetupAudio(hwnd);
            return 0;
        }
        if (wp == kHealthTimer) {
            std::wstring alert;
            if (!engine.CheckHealth(alert) && !alert.empty()) {
                uiProblem = alert;
                audioptt::LogError(L"Runtime health check: " + alert);
            }
            static int deviceTicks = 0;
            if (++deviceTicks >= 3) {
                deviceTicks = 0;
                RefreshDevices();
            }
            Refresh();
            return 0;
        }
        break;
    case WM_HOTKEY:
        if (wp == kHotkey) { Toggle(); return 0; }
        break;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kButton: Toggle(); return 0;
        case kMonitor: {
            std::wstring error;
            const bool enabled = SendMessageW(monitorCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            audioptt::LogInfo(enabled ? L"Monitor enabled." : L"Monitor disabled (silent local desktop audio).");
            if (!engine.SetMonitor(enabled, error)) {
                uiProblem = error;
                audioptt::LogError(L"Monitor change failed: " + error);
            } else uiProblem.clear();
            RefreshDevices();
            Refresh();
            return 0;
        }
        case kDevices: ShowDevicesInLog(); return 0;
        case kRetry: SetupAudio(hwnd); return 0;
        case kCopyLogs: CopyLogs(hwnd); return 0;
        case kClearLogs: audioptt::ClearLogs(); RefreshLogs(true); return 0;
        }
        break;
    case WM_CLOSE:
        engine.Stop();
        if (engine.Status().find(L"WARNING: Windows playback was not restored") != std::wstring::npos) {
            audioptt::LogError(engine.Status());
            MessageBoxW(hwnd,
                L"Windows output could not be restored automatically.\n"
                L"Settings > System > Sound > select your physical speakers/headphones.",
                L"AudioPTT - Restore audio output", MB_OK | MB_ICONWARNING);
        }
        DestroyWindow(hwnd);
        return 0;
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
    HANDLE singleton = CreateMutexW(nullptr, FALSE, L"Local\\AudioPTT.SingleInstance");
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"AudioPTT is already running.",
                    L"AudioPTT", MB_OK | MB_ICONINFORMATION);
        if (singleton) CloseHandle(singleton);
        return 1;
    }
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Windows COM initialization failed.",
                    L"AudioPTT", MB_OK | MB_ICONERROR);
        CloseHandle(singleton);
        return 1;
    }
    INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_TAB_CLASSES};
    InitCommonControlsEx(&controls);

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
                                 CW_USEDEFAULT, CW_USEDEFAULT, 558, 622,
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
