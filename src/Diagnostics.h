#pragma once
#include <cstdint>
#include <string>

namespace audioptt {
enum class LogLevel { Info, Warning, Error };
void Log(LogLevel level, const std::wstring& message);
inline void LogInfo(const std::wstring& message) { Log(LogLevel::Info, message); }
inline void LogWarning(const std::wstring& message) { Log(LogLevel::Warning, message); }
inline void LogError(const std::wstring& message) { Log(LogLevel::Error, message); }
// Thread-safe snapshot for the Win32 Logs tab; bounded to the most recent events.
std::wstring LogText();
std::uint64_t LogRevision();
void ClearLogs();
}
