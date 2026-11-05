#include "pch.h"

static DWORD WINAPI DelayedLookupTablePatchThread(LPVOID)
{
    TraceLog::Write("delayed lookup-table thread started");

    // Yaneurao's executable is Enigma-packed.  Its CP932 lookup table does
    // not exist yet when d3d9.dll is loaded, so do not touch the EXE entry
    // point (that can invalidate Enigma's unpacking stubs).  Instead retry
    // the table scan while the game finishes unpacking.
    for (int i = 0; i < 600; i++)
    {
        Sleep(100);
        SjisTunnelEncoding::PatchGameLookupTable();
    }

    TraceLog::Write("delayed lookup-table thread timed out after 600 attempts");
    return 0;
}

void Initialize()
{
    TraceLog::Initialize();
    TraceLog::Write("Initialize begin");

    wchar_t path[32768]{};
    DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (length > 0 && length < ARRAYSIZE(path))
        TraceLog::WriteW("game", path);
    length = GetModuleFileNameW(Proxy::ProxyModuleHandle, path, ARRAYSIZE(path));
    if (length > 0 && length < ARRAYSIZE(path))
        TraceLog::WriteW("proxy", path);

    wchar_t currentDirectory[32768]{};
    length = GetCurrentDirectoryW(ARRAYSIZE(currentDirectory), currentDirectory);
    if (length > 0 && length < ARRAYSIZE(currentDirectory))
        TraceLog::WriteW("current_directory_before_init", currentDirectory);

    TraceLog::Write(
        "locale api: ACP=%u OEMCP=%u thread_locale=0x%08lX ui_language=0x%04X",
        GetACP(),
        GetOEMCP(),
        GetThreadLocale(),
        GetUserDefaultUILanguage()
    );
    TraceLog::Write(
        "locale emulator modules: LocaleEmulator=%p LoaderDll=%p LEProc=%p",
        GetModuleHandleW(L"LocaleEmulator.dll"),
        GetModuleHandleW(L"LoaderDll.dll"),
        GetModuleHandleW(L"LEProc.exe")
    );

    // Uncomment for games that only work in a Japanese locale
    // (and include LoaderDll.dll and LocaleEmulator.dll from https://github.com/xupefei/Locale-Emulator/releases)
    /*
    if (GetACP() != 932)
    {
        if (LocaleEmulator::Relaunch())
            ExitProcess(0);
    }
    //*/

    CompilerHelper::Init();
    TraceLog::Write("CompilerHelper initialized");
    Win32AToWAdapter::Init();
    TraceLog::Write("Win32AToWAdapter initialized");

    GdiProportionalizer::Init();
    TraceLog::Write("GDI proportionalizer initialized");
    D3DXProportionalizer::Init();
    TraceLog::Write("D3DX proportionalizer initialized");
    D2DProportionalizer::Init();
    TraceLog::Write("D2D proportionalizer initialized");

    EnginePatches::Init();
    TraceLog::Write("engine patches initialized");

    SetCurrentDirectoryW(Path::GetModuleFolderPath(nullptr).c_str());
    TraceLog::WriteW("current_directory_after_init", Path::GetModuleFolderPath(nullptr));

    // The first attempt is useful for unpacked engines.  The worker is
    // required for Enigma-packed Yaneurao builds, where the table appears
    // only after the original entry point has started.
    SjisTunnelEncoding::PatchGameLookupTable();
    HANDLE hThread = CreateThread(nullptr, 0, DelayedLookupTablePatchThread, nullptr, 0, nullptr);
    if (hThread != nullptr)
        CloseHandle(hThread);

    TraceLog::Write("Initialize end; delayed thread handle=%p", hThread);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        // Proxy::Init establishes ProxyModuleHandle before the logger is
        // initialized, so the log is always placed next to this DLL.
        Proxy::Init(hModule);
        TraceLog::Write("DLL_PROCESS_ATTACH completed Proxy::Init");
        // Initialize from DLL_PROCESS_ATTACH rather than detouring the
        // executable entry point.  The latter is unsafe for Enigma-packed
        // executables and was the source of early startup crashes.
        Initialize();
        break;
    	
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
