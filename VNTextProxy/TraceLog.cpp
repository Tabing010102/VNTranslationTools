#include "pch.h"

#if VNTP_ENABLE_TRACE_LOG
namespace
{
    constexpr LONG NotInitialized = 0;
    constexpr LONG Initializing = 1;
    constexpr LONG Ready = 2;

    volatile LONG State = NotInitialized;
    HANDLE LogFile = INVALID_HANDLE_VALUE;
    CRITICAL_SECTION LogLock{};
    std::wstring LogPath;

    std::wstring GetModulePath()
    {
        wchar_t path[32768]{};
        DWORD length = GetModuleFileNameW(Proxy::ProxyModuleHandle, path, ARRAYSIZE(path));
        if (length == 0 || length >= ARRAYSIZE(path))
            return L"";
        return path;
    }

    std::wstring GetLogPath(const std::wstring& modulePath)
    {
        size_t slash = modulePath.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return L"yaneurao_v21455.log";
        return modulePath.substr(0, slash + 1) + L"yaneurao_v21455.log";
    }

    std::wstring GetTempLogPath()
    {
        wchar_t tempPath[MAX_PATH]{};
        DWORD length = GetTempPathW(ARRAYSIZE(tempPath), tempPath);
        if (length == 0 || length >= ARRAYSIZE(tempPath))
            return L"yaneurao_v21455.log";
        return std::wstring(tempPath) + L"yaneurao_v21455.log";
    }

    void WriteLine(const char* message)
    {
        if (LogFile == INVALID_HANDLE_VALUE)
            return;

        SYSTEMTIME time{};
        GetLocalTime(&time);

        char line[8192]{};
        sprintf_s(
            line,
            "[%04u-%02u-%02u %02u:%02u:%02u.%03u][pid=%lu tid=%lu] %s\r\n",
            time.wYear,
            time.wMonth,
            time.wDay,
            time.wHour,
            time.wMinute,
            time.wSecond,
            time.wMilliseconds,
            GetCurrentProcessId(),
            GetCurrentThreadId(),
            message
        );

        DWORD written = 0;
        WriteFile(LogFile, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
        FlushFileBuffers(LogFile);
    }
}

void TraceLog::Initialize()
{
    if (InterlockedCompareExchange(&State, Initializing, NotInitialized) != NotInitialized)
        return;

    InitializeCriticalSection(&LogLock);

    std::wstring modulePath = GetModulePath();
    LogPath = GetLogPath(modulePath);
    LogFile = CreateFileW(
        LogPath.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (LogFile == INVALID_HANDLE_VALUE)
    {
        LogPath = GetTempLogPath();
        LogFile = CreateFileW(
            LogPath.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
    }

    InterlockedExchange(&State, Ready);

    if (LogFile != INVALID_HANDLE_VALUE)
    {
        EnterCriticalSection(&LogLock);
        char message[1024]{};
        std::string path = ToUtf8(LogPath);
        sprintf_s(message, "logger started; path=%s", path.c_str());
        WriteLine(message);
        LeaveCriticalSection(&LogLock);
    }
}

void TraceLog::Write(const char* format, ...)
{
    if (State != Ready)
        return;

    char message[7168]{};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);

    EnterCriticalSection(&LogLock);
    WriteLine(message);
    LeaveCriticalSection(&LogLock);
}

void TraceLog::WriteW(const char* label, const std::wstring& value)
{
    std::string utf8 = ToUtf8(value);
    Write("%s=%s", label, utf8.c_str());
}
#else
// No file creation, temporary-directory fallback, locks, formatting or writes
// are present in the default release logger implementation.
void TraceLog::Initialize() {}
void TraceLog::Write(const char*, ...) {}
void TraceLog::WriteW(const char*, const std::wstring&) {}
#endif

std::string TraceLog::ToUtf8(const std::wstring& value)
{
    if (value.empty())
        return {};

    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return "<invalid-utf16>";

    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::string TraceLog::Hex(const void* data, size_t length, size_t maximum)
{
    if (data == nullptr)
        return "<null>";

    const BYTE* bytes = static_cast<const BYTE*>(data);
    length = (std::min)(length, maximum);

    std::string result;
    result.reserve(length * 3 + 16);
    char byte[4]{};
    for (size_t i = 0; i < length; i++)
    {
        sprintf_s(byte, "%02X", bytes[i]);
        if (!result.empty())
            result += ' ';
        result += byte;
    }
    if (length < maximum)
        return result;
    result += " ...";
    return result;
}
