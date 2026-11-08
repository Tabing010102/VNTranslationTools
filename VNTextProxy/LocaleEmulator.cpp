#include "pch.h"

namespace
{
    // Set in the environment of the relaunched process to prevent a relaunch loop.
    constexpr wchar_t RelaunchMarker[] = L"VNTEXTPROXY_LE_RELAUNCHED";
    constexpr wchar_t TimeZoneKey[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones\\Tokyo Standard Time";
    constexpr wchar_t CodePageKey[] = L"System\\CurrentControlSet\\Control\\Nls\\CodePage";
    constexpr ULONG64 HkeyLocalMachine = 0x80000002;

    bool FileExists(const std::wstring& path)
    {
        DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }
}

bool LocaleEmulator::ConsumeRelaunchMarker()
{
    if (GetEnvironmentVariableW(RelaunchMarker, nullptr, 0) == 0)
        return false;

    SetEnvironmentVariableW(RelaunchMarker, nullptr);
    return true;
}

bool LocaleEmulator::Relaunch()
{
    std::wstring exePath = Path::GetModuleFilePath(nullptr);
    std::wstring exeFolder = Path::GetDirectoryName(exePath);
    std::wstring loaderPath = Path::Combine(exeFolder, L"LoaderDll.dll");
    std::wstring emulatorPath = Path::Combine(exeFolder, L"LocaleEmulator.dll");
    if (!FileExists(loaderPath) || !FileExists(emulatorPath))
    {
        TraceLog::Write("locale emulator: LoaderDll.dll/LocaleEmulator.dll not found next to the game; not relaunching");
        return false;
    }

    HMODULE hLoader = LoadLibraryW(loaderPath.c_str());
    if (hLoader == nullptr)
    {
        TraceLog::Write("locale emulator: cannot load LoaderDll.dll error=%lu", GetLastError());
        return false;
    }

    LeCreateProcess_t LeCreateProcess = (LeCreateProcess_t)GetProcAddress(hLoader, "LeCreateProcess");
    if (LeCreateProcess == nullptr)
    {
        TraceLog::Write("locale emulator: LeCreateProcess export not found");
        return false;
    }

    std::vector<BYTE> leb = CreateLeb();
    TraceLog::Write("locale emulator: leb size=%u data=%s", (unsigned)leb.size(), TraceLog::Hex(leb.data(), leb.size(), 2048).c_str());
    std::wstring commandLine = GetCommandLineW();
    STARTUPINFOW startupInfo{ sizeof(startupInfo) };
    ML_PROCESS_INFORMATION processInfo{};

    // Like LEProc: same command line, working directory = game folder,
    // environment inherited (including the relaunch marker).
    SetEnvironmentVariableW(RelaunchMarker, L"1");
    LONG status = LeCreateProcess(
        reinterpret_cast<PLEB>(leb.data()),
        exePath.c_str(),
        commandLine.data(),
        exeFolder.c_str(),
        0,
        &startupInfo,
        &processInfo,
        nullptr,
        nullptr,
        nullptr,
        nullptr
    );
    if (status != 0)
    {
        SetEnvironmentVariableW(RelaunchMarker, nullptr);
        TraceLog::Write("locale emulator: LeCreateProcess failed status=0x%08lX", status);
        return false;
    }

    TraceLog::Write("locale emulator: relaunched as pid=%lu", processInfo.dwProcessId);
    if (processInfo.hThread != nullptr)
        CloseHandle(processInfo.hThread);
    if (processInfo.hProcess != nullptr)
        CloseHandle(processInfo.hProcess);
    return true;
}

// Reproduces the block LEProc 2.5.0.1 builds for the "Run in Japanese" profile:
// Location=ja-JP, Timezone=Tokyo Standard Time, RedirectRegistry=true,
// IsAdvancedRedirection=false.
std::vector<BYTE> LocaleEmulator::CreateLeb()
{
    LEB leb{};
    leb.AnsiCodePage = 932;
    leb.OemCodePage = 932;
    leb.LocaleID = 1041;
    leb.DefaultCharset = SHIFTJIS_CHARSET;
    leb.HookUILanguageApi = 0;

    HKEY hTimeZone;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, TimeZoneKey, 0, KEY_READ, &hTimeZone) == ERROR_SUCCESS)
    {
        // LEProc uses the localized standard name for both names.
        wchar_t standardName[256]{};
        DWORD bufferSize = sizeof(standardName);
        if (RegGetValueW(hTimeZone, nullptr, L"Std", RRF_RT_REG_SZ, nullptr, standardName, &bufferSize) == ERROR_SUCCESS)
        {
            wcsncpy_s(leb.Timezone.StandardName, standardName, _TRUNCATE);
            wcsncpy_s(leb.Timezone.DaylightName, standardName, _TRUNCATE);
        }

        REG_TZI_FORMAT timeZoneInfo{};
        bufferSize = sizeof(timeZoneInfo);
        if (RegGetValueW(hTimeZone, nullptr, L"TZI", RRF_RT_REG_BINARY, nullptr, &timeZoneInfo, &bufferSize) == ERROR_SUCCESS)
        {
            leb.Timezone.Bias = timeZoneInfo.Bias;
            leb.Timezone.StandardBias = timeZoneInfo.StandardBias;
        }
        leb.Timezone.DaylightBias = 0;

        RegCloseKey(hTimeZone);
    }

    struct RegistryValue
    {
        const wchar_t* ValueName;
        const wchar_t* Data;
    };
    static const RegistryValue registryValues[] =
    {
        { L"InstallLanguage", L"1041" },
        { L"Default",         L"1041" },
        { L"OEMCP",           L"932"  },
        { L"ACP",             L"932"  },
    };
    constexpr size_t entryCount = std::size(registryValues);
    leb.NumberOfRegistryRedirectionEntries = entryCount;

    // Strings are stored without their terminator in Length/DataSize and are
    // followed by two zero bytes, exactly like LERegistryRedirector.
    const size_t dataStart = sizeof(LEB) + entryCount * sizeof(REGISTRY_REDIRECTION_ENTRY64);
    std::vector<BYTE> data;
    auto addString = [&](const wchar_t* value, ULONG64& offset) -> size_t
    {
        size_t length = wcslen(value) * sizeof(wchar_t);
        offset = dataStart + data.size();
        const BYTE* bytes = reinterpret_cast<const BYTE*>(value);
        data.insert(data.end(), bytes, bytes + length);
        data.insert(data.end(), 2, 0);
        return length;
    };

    std::vector<REGISTRY_REDIRECTION_ENTRY64> entries(entryCount);
    for (size_t i = 0; i < entryCount; i++)
    {
        REGISTRY_ENTRY64 entry{};
        entry.Root = HkeyLocalMachine;
        entry.DataType = REG_SZ;
        entry.SubKey.Length = entry.SubKey.MaximumLength = (USHORT)addString(CodePageKey, entry.SubKey.Buffer);
        entry.ValueName.Length = entry.ValueName.MaximumLength = (USHORT)addString(registryValues[i].ValueName, entry.ValueName.Buffer);
        entry.DataSize = addString(registryValues[i].Data, entry.Data);
        entries[i].Original = entry;
        entries[i].Redirected = entry;
    }

    std::vector<BYTE> result(dataStart);
    memcpy(result.data(), &leb, sizeof(leb));
    memcpy(result.data() + sizeof(LEB), entries.data(), entries.size() * sizeof(entries[0]));
    result.insert(result.end(), data.begin(), data.end());
    return result;
}
