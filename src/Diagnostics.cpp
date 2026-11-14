#include "Diagnostics.h"
#include <windows.h>
#include <deque>
#include <mutex>

namespace audioptt {
namespace {
std::mutex logMutex;
std::deque<std::wstring> entries;
std::uint64_t revision = 0;
constexpr size_t maxEntries = 1500; // Preserve multi-scan debug sessions and initial endpoint snapshot
std::wstring Stamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%02u:%02u:%02u.%03u",
               time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return buffer;
}
}
void Log(LogLevel level, const std::wstring& message) {
    const wchar_t* label = level == LogLevel::Error ? L"ERROR" :
                           level == LogLevel::Warning ? L"WARN " : L"INFO ";
    std::wstring line = L"[" + Stamp() + L"] [" + label + L"] " + message;
    {
        std::lock_guard<std::mutex> guard(logMutex);
        entries.emplace_back(std::move(line));
        if (entries.size() > maxEntries) entries.pop_front();
        ++revision;
    }
}
std::wstring LogText() {
    std::lock_guard<std::mutex> guard(logMutex);
    std::wstring text;
    for (const auto& entry : entries) {
        text += entry;
        text += L"\r\n";
    }
    if (text.empty()) text = L"No events yet. Retry audio setup to record diagnostics.\r\n";
    return text;
}
std::uint64_t LogRevision() {
    std::lock_guard<std::mutex> guard(logMutex);
    return revision;
}
void ClearLogs() {
    std::lock_guard<std::mutex> guard(logMutex);
    entries.clear();
    ++revision;
}
}
