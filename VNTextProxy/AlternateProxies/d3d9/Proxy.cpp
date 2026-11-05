#include "pch.h"

namespace
{
    constexpr DWORD CrackPatchOffset = 0x12C597;

    DWORD WINAPI CrackThread(LPVOID)
    {
        HMODULE hGame = GetModuleHandleW(nullptr);
        if (hGame == nullptr)
        {
            TraceLog::Write("crack thread: game module not available");
            return 0;
        }

        DWORD imageSize = DetourGetModuleSize(nullptr);
        if (imageSize <= CrackPatchOffset + 1)
        {
            TraceLog::Write(
                "crack thread: image too small; base=%p image_size=0x%lX required=0x%lX",
                hGame,
                imageSize,
                CrackPatchOffset + 2
            );
            return 0;
        }

        BYTE* patchAddress = reinterpret_cast<BYTE*>(hGame) + CrackPatchOffset;
        TraceLog::Write(
            "crack thread: base=%p image_size=0x%lX patch=%p initial=%s",
            hGame,
            imageSize,
            patchAddress,
            TraceLog::Hex(patchAddress, 2).c_str()
        );

        // The original 9177 proxy waited for the unpacked instruction
        // "call eax" (FF D0) at this fixed RVA, then replaced it with two
        // NOPs.  Keep the same patch but do it without the original DLL's
        // message box and without modifying the packed entry point.
        for (int i = 0; i < 600; i++)
        {
            if (patchAddress[0] == 0xFF && patchAddress[1] == 0xD0)
            {
                DWORD oldProtect;
                if (VirtualProtect(patchAddress, 2, PAGE_EXECUTE_READWRITE, &oldProtect))
                {
                    patchAddress[0] = 0x90;
                    patchAddress[1] = 0x90;
                    FlushInstructionCache(GetCurrentProcess(), patchAddress, 2);

                    DWORD ignored;
                    VirtualProtect(patchAddress, 2, oldProtect, &ignored);
                    TraceLog::Write(
                        "crack thread: patched %p FF D0 -> 90 90; old_protect=0x%lX new=%s",
                        patchAddress,
                        oldProtect,
                        TraceLog::Hex(patchAddress, 2).c_str()
                    );
                }
                else
                    TraceLog::Write("crack thread: VirtualProtect failed at %p error=%lu", patchAddress, GetLastError());
                return 0;
            }
            Sleep(100);
        }
        TraceLog::Write(
            "crack thread: timed out; patch=%p final=%s",
            patchAddress,
            TraceLog::Hex(patchAddress, 2).c_str()
        );
        return 0;
    }
}

void Proxy::Init(HMODULE hProxy)
{
    ProxyModuleHandle = hProxy;
    TraceLog::Initialize();
    TraceLog::Write("Proxy::Init proxy_module=%p", hProxy);
    
    wchar_t realDllPath[MAX_PATH];
    GetSystemDirectory(realDllPath, MAX_PATH);
    wcscat_s(realDllPath, L"\\d3d9.dll");
    TraceLog::WriteW("original_d3d9_path", realDllPath);
    OriginalModuleHandle = LoadLibrary(realDllPath);
    if (OriginalModuleHandle == nullptr)
    {
        TraceLog::Write("Proxy::Init failed to load original d3d9.dll error=%lu", GetLastError());
        MessageBox(nullptr, L"Cannot load original d3d9.dll library", L"Proxy", MB_ICONERROR);
        ExitProcess(0);
    }
    TraceLog::Write("Proxy::Init original d3d9 loaded module=%p", OriginalModuleHandle);

#define RESOLVE(fn) Original##fn = GetProcAddress(OriginalModuleHandle, #fn)
    RESOLVE(D3DPERF_BeginEvent);
    RESOLVE(D3DPERF_EndEvent);
    RESOLVE(D3DPERF_GetStatus);
    RESOLVE(D3DPERF_QueryRepeatFrame);
    RESOLVE(D3DPERF_SetMarker);
    RESOLVE(D3DPERF_SetOptions);
    RESOLVE(D3DPERF_SetRegion);
    RESOLVE(DebugSetLevel);
    RESOLVE(DebugSetMute);
    RESOLVE(Direct3D9EnableMaximizedWindowedModeShim);
    RESOLVE(Direct3DCreate9);
    RESOLVE(Direct3DCreate9Ex);
    RESOLVE(Direct3DCreate9On12);
    RESOLVE(Direct3DCreate9On12Ex);
    RESOLVE(Direct3DShaderValidatorCreate9);
    RESOLVE(PSGPError);
    RESOLVE(PSGPSampleTexture);
#undef RESOLVE

    TraceLog::Write(
        "Proxy::Init exports Direct3DCreate9=%p Direct3DCreate9Ex=%p",
        OriginalDirect3DCreate9,
        OriginalDirect3DCreate9Ex
    );

    StartCrackThread();
    TraceLog::Write("Proxy::Init crack thread requested");
}

void Proxy::StartCrackThread()
{
    HANDLE hThread = CreateThread(nullptr, 0, CrackThread, nullptr, 0, nullptr);
    if (hThread != nullptr)
        CloseHandle(hThread);
    else
        TraceLog::Write("Proxy::StartCrackThread CreateThread failed error=%lu", GetLastError());
}

__declspec(naked) void FakeD3DPERF_BeginEvent()                         { __asm { jmp [Proxy::OriginalD3DPERF_BeginEvent] } }
__declspec(naked) void FakeD3DPERF_EndEvent()                           { __asm { jmp [Proxy::OriginalD3DPERF_EndEvent] } }
__declspec(naked) void FakeD3DPERF_GetStatus()                          { __asm { jmp [Proxy::OriginalD3DPERF_GetStatus] } }
__declspec(naked) void FakeD3DPERF_QueryRepeatFrame()                   { __asm { jmp [Proxy::OriginalD3DPERF_QueryRepeatFrame] } }
__declspec(naked) void FakeD3DPERF_SetMarker()                          { __asm { jmp [Proxy::OriginalD3DPERF_SetMarker] } }
__declspec(naked) void FakeD3DPERF_SetOptions()                         { __asm { jmp [Proxy::OriginalD3DPERF_SetOptions] } }
__declspec(naked) void FakeD3DPERF_SetRegion()                          { __asm { jmp [Proxy::OriginalD3DPERF_SetRegion] } }
__declspec(naked) void FakeDebugSetLevel()                              { __asm { jmp [Proxy::OriginalDebugSetLevel] } }
__declspec(naked) void FakeDebugSetMute()                               { __asm { jmp [Proxy::OriginalDebugSetMute] } }
__declspec(naked) void FakeDirect3D9EnableMaximizedWindowedModeShim()   { __asm { jmp [Proxy::OriginalDirect3D9EnableMaximizedWindowedModeShim] } }
__declspec(naked) void FakeDirect3DCreate9()                            { __asm { jmp [Proxy::OriginalDirect3DCreate9] } }
__declspec(naked) void FakeDirect3DCreate9Ex()                          { __asm { jmp [Proxy::OriginalDirect3DCreate9Ex] } }
__declspec(naked) void FakeDirect3DCreate9On12()                        { __asm { jmp [Proxy::OriginalDirect3DCreate9On12] } }
__declspec(naked) void FakeDirect3DCreate9On12Ex()                      { __asm { jmp [Proxy::OriginalDirect3DCreate9On12Ex] } }
__declspec(naked) void FakeDirect3DShaderValidatorCreate9()             { __asm { jmp [Proxy::OriginalDirect3DShaderValidatorCreate9] } }
__declspec(naked) void FakePSGPError()                                  { __asm { jmp [Proxy::OriginalPSGPError] } }
__declspec(naked) void FakePSGPSampleTexture()                          { __asm { jmp [Proxy::OriginalPSGPSampleTexture] } }
