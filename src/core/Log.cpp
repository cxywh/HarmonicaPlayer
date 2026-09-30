#include "core/Log.h"

#include <atomic>
#include <cstdio>
#include <mutex>

#include "core/Platform.h"
#include "core/Time.h"

namespace hp {
namespace {

std::mutex g_logMutex;

// ---- debug.log：警告/错误落到 exe 同目录，方便用户打包发过来 ----
// 约定：日志文件写不出去时**绝不**影响功能，静默降级（兜底原则）。
const char  kLogFileName[]  = "debug.log";
const long long kLogMaxBytes = 1024 * 1024;   // 超过 1MB 从头来过，免得无限长大
std::FILE*  g_logFile     = nullptr;
bool        g_logFileDone = false;            // 只尝试打开一次

std::string WallClockText() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d", now.wYear, now.wMonth,
                  now.wDay, now.wHour, now.wMinute, now.wSecond);
    return std::string(buffer);
}

// 调用前必须已经持有 g_logMutex
void OpenLogFileLocked() {
    if (g_logFileDone) {
        return;
    }
    g_logFileDone = true;
    wchar_t buffer[MAX_PATH] = {0};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return;   // 拿不到路径就算了，不打扰用户
    }
    std::wstring path(buffer, length);
    const std::size_t slash = path.find_last_of(L"\\/");
    path.resize(slash == std::wstring::npos ? 0 : slash + 1);
    path += L"debug.log";

    std::FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"ab") != 0 || file == nullptr) {
        return;
    }
    _fseeki64(file, 0, SEEK_END);
    if (_ftelli64(file) > kLogMaxBytes) {
        std::fclose(file);
        file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
            return;
        }
    }
    g_logFile = file;
    const std::string banner = "===== Harmonica Player 启动 " + WallClockText() + " =====\r\n";
    std::fwrite(banner.data(), 1, banner.size(), g_logFile);
    std::fflush(g_logFile);
}
std::atomic<std::size_t> g_logLineCount{0};
const double g_startSeconds = NowSeconds();

// 用 WriteConsoleW 输出，避免 std::cout 在中文 Windows 控制台上出现乱码；
// 输出被重定向到文件时退回 WriteFile（此时没有颜色可言）。
bool         g_console = false;
HANDLE       g_stdout = INVALID_HANDLE_VALUE;
WORD         g_defaultAttributes = 7;

void EnsureInitializedLocked() {
    static bool initialized = false;
    if (initialized) {
        return;
    }
    initialized = true;

    g_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    g_console = (g_stdout != INVALID_HANDLE_VALUE && g_stdout != nullptr) &&
                GetConsoleMode(g_stdout, &mode) != FALSE;
    if (g_console) {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (GetConsoleScreenBufferInfo(g_stdout, &info)) {
            g_defaultAttributes = info.wAttributes;
        }
    }
}

WORD ColorOfLocked(LogLevel level) {
    switch (level) {
        case LogLevel::Ok:    return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        case LogLevel::Warn:  return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        case LogLevel::Error: return FOREGROUND_RED | FOREGROUND_INTENSITY;
        case LogLevel::Info:
        case LogLevel::Plain:
        default:              return g_defaultAttributes;
    }
}

const char* TagOf(LogLevel level) {
    switch (level) {
        case LogLevel::Ok:    return "[完成] ";
        case LogLevel::Warn:  return "[警告] ";
        case LogLevel::Error: return "[错误] ";
        case LogLevel::Info:  return "[信息] ";
        case LogLevel::Plain:
        default:              return "";
    }
}

void WriteUtf8Locked(const std::string& text) {
    if (text.empty()) {
        return;
    }
    if (!g_console) {
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fflush(stdout);
        return;
    }

    const int wideLength = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                               static_cast<int>(text.size()), nullptr, 0);
    if (wideLength <= 0) {
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fflush(stdout);
        return;
    }
    std::wstring wide(static_cast<size_t>(wideLength), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(),
                        wideLength);

    DWORD written = 0;
    WriteConsoleW(g_stdout, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr);
}

}  // namespace

void LogLine(LogLevel level, const std::string& message) {
    g_logLineCount.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_logMutex);
    EnsureInitializedLocked();

    const double elapsed = NowSeconds() - g_startSeconds;
    char prefix[48];
    std::snprintf(prefix, sizeof(prefix), "[%8.3f] ", elapsed);

    std::string line;
    if (level != LogLevel::Plain) {
        line += prefix;
        line += TagOf(level);
    }
    line += message;
    line += "\r\n";

    const WORD color = ColorOfLocked(level);
    const bool recolor = g_console && color != g_defaultAttributes;
    if (recolor) {
        SetConsoleTextAttribute(g_stdout, color);
    }
    WriteUtf8Locked(line);
    if (recolor) {
        SetConsoleTextAttribute(g_stdout, g_defaultAttributes);
    }

    // 只有警告和错误才落盘（用户要的是"记录错误"，也避免刷屏写爆磁盘）
    if (level == LogLevel::Warn || level == LogLevel::Error) {
        OpenLogFileLocked();
        if (g_logFile != nullptr) {
            std::fwrite(line.data(), 1, line.size(), g_logFile);
            std::fflush(g_logFile);
        }
    }
}

void LogStartSession() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    OpenLogFileLocked();
}

std::size_t LogLineCount() { return g_logLineCount.load(std::memory_order_relaxed); }

void ResetLogLineCount() { g_logLineCount.store(0, std::memory_order_relaxed); }

void LogInfo(const std::string& message)  { LogLine(LogLevel::Info, message); }
void LogOk(const std::string& message)    { LogLine(LogLevel::Ok, message); }
void LogWarn(const std::string& message)  { LogLine(LogLevel::Warn, message); }
void LogError(const std::string& message) { LogLine(LogLevel::Error, message); }
void LogPlain(const std::string& message) { LogLine(LogLevel::Plain, message); }
void LogBlank()                           { LogPlain(""); }

void LogInline(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    EnsureInitializedLocked();
    WriteUtf8Locked(message);
}

double SinceStartSeconds() {
    return NowSeconds() - g_startSeconds;
}

}  // namespace hp
