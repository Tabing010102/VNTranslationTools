#include "pch.h"

static DWORD WINAPI DelayedLookupTablePatchThread(LPVOID)
{
    // Yaneurao's executable is Enigma-packed.  Its CP932 lookup table does
    // not exist yet when d3d9.dll is loaded, so do not touch the EXE entry
    // point (that can invalidate Enigma's unpacking stubs).  Instead retry
    // the table scan while the game finishes unpacking.
    for (int i = 0; i < 600; i++)
    {
        Sleep(100);
        SjisTunnelEncoding::PatchGameLookupTable();
    }
    return 0;
}

void Initialize()
{
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
    Win32AToWAdapter::Init();

    GdiProportionalizer::Init();
    D2DProportionalizer::Init();

    EnginePatches::Init();

    SetCurrentDirectoryW(Path::GetModuleFolderPath(nullptr).c_str());

    // The first attempt is useful for unpacked engines.  The worker is
    // required for Enigma-packed Yaneurao builds, where the table appears
    // only after the original entry point has started.
    SjisTunnelEncoding::PatchGameLookupTable();
    HANDLE hThread = CreateThread(nullptr, 0, DelayedLookupTablePatchThread, nullptr, 0, nullptr);
    if (hThread != nullptr)
        CloseHandle(hThread);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        Proxy::Init(hModule);
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
