#pragma once

#ifndef VNTP_ENABLE_TRACE_LOG
#define VNTP_ENABLE_TRACE_LOG 0
#endif

// Small file logger used only for diagnosing game-specific proxy loading and
// text paths.  The log is written next to the proxy DLL so Locale Emulator's
// current-directory changes do not change its destination. Logging is compiled
// out by default; build with VNTextProxyTraceLogging=1 for a diagnostic DLL.
class TraceLog
{
public:
    static constexpr bool Enabled = VNTP_ENABLE_TRACE_LOG != 0;

    static void Initialize();
    static void Write(const char* format, ...);
    static void WriteW(const char* label, const std::wstring& value);

    static std::string ToUtf8(const std::wstring& value);
    static std::string Hex(const void* data, size_t length, size_t maximum = 512);
};
