// Standalone x86 integration test. Run only in an isolated directory containing
// the newly built d3d9.dll and MahoroCHS Gothic.ttf, never in the game directory.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <string>
#include <vector>

static int Failures = 0;
static std::vector<BYTE> ExpectedFont;

static bool CheckFont(const char* label, HFONT font, bool required)
{
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ previous = font == nullptr ? nullptr : SelectObject(dc, font);
    DWORD length = previous == nullptr ? GDI_ERROR : GetFontData(dc, 0, 0, nullptr, 0);
    std::vector<BYTE> bytes;
    if (length != GDI_ERROR && length > 0 && length < 32 * 1024 * 1024)
    {
        bytes.resize(length);
        if (GetFontData(dc, 0, 0, bytes.data(), length) != length)
            bytes.clear();
    }
    bool match = !bytes.empty() && bytes == ExpectedFont;
    std::printf("CHECK %s exact_font_file_match=%s sfnt_bytes=%lu\n", label, match ? "yes" : "no", length);
    if (required && !match)
        ++Failures;
    if (previous != nullptr && previous != HGDI_ERROR)
        SelectObject(dc, previous);
    DeleteDC(dc);
    return match;
}

struct FontDesc
{
    INT Height;
    UINT Width, Weight, MipLevels;
    BOOL Italic;
    BYTE CharSet, OutputPrecision, Quality, PitchAndFamily;
    WCHAR FaceName[LF_FACESIZE];
};

int wmain()
{
    SetConsoleOutputCP(CP_UTF8);
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash == nullptr)
        return 10;
    *slash = L'\0';
    std::wstring folder(path);
    FILE* file = nullptr;
    _wfopen_s(&file, (folder + L"\\MahoroCHS Gothic.ttf").c_str(), L"rb");
    if (file == nullptr)
        return 11;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    if (size <= 0 || size > 32 * 1024 * 1024)
    {
        fclose(file);
        return 12;
    }
    ExpectedFont.resize(size);
    bool read = fread(ExpectedFont.data(), 1, ExpectedFont.size(), file) == ExpectedFont.size();
    fclose(file);
    if (!read)
        return 13;

    using CreateFontFunction = HFONT (WINAPI *)(const LOGFONTW*);
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    auto cachedCreateFont = reinterpret_cast<CreateFontFunction>(GetProcAddress(gdi, "CreateFontIndirectW"));
    if (cachedCreateFont == nullptr)
        return 14;
    LOGFONTW original{};
    original.lfHeight = 17;
    original.lfWidth = 10;
    original.lfWeight = FW_NORMAL;
    original.lfCharSet = SHIFTJIS_CHARSET;
    original.lfQuality = PROOF_QUALITY;
    original.lfPitchAndFamily = FIXED_PITCH;
    wcscpy_s(original.lfFaceName, L"MS Gothic");
    HFONT before = cachedCreateFont(&original);
    CheckFont("before_proxy_load", before, false);
    DeleteObject(before);

    HMODULE proxy = LoadLibraryW((folder + L"\\d3d9.dll").c_str());
    if (proxy == nullptr)
    {
        std::printf("FAIL LoadLibrary proxy error=%lu\n", GetLastError());
        return 15;
    }

    HFONT direct = cachedCreateFont(&original);
    CheckFont("cached_export_pointer_after_proxy_load", direct, true);
    LOGFONTW actual{};
    bool metrics = GetObjectW(direct, sizeof(actual), &actual) == sizeof(actual)
        && actual.lfHeight == original.lfHeight && actual.lfWidth == original.lfWidth
        && actual.lfWeight == original.lfWeight && actual.lfQuality == original.lfQuality;
    std::printf("CHECK requested_metrics_preserved=%s\n", metrics ? "yes" : "no");
    if (!metrics)
        ++Failures;
    DeleteObject(direct);

    HFONT imported = CreateFontIndirectW(&original);
    CheckFont("main_IAT_CreateFontIndirectW", imported, true);
    DeleteObject(imported);
    HFONT wide = CreateFontW(17, 10, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        SHIFTJIS_CHARSET, 0, 0, PROOF_QUALITY, FIXED_PITCH, L"MS Gothic");
    CheckFont("main_IAT_CreateFontW", wide, true);
    DeleteObject(wide);

    // D3DX is optional only when the environment cannot create a test device.
    // A font creation/selection failure after a device exists is a test failure.
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, ARRAYSIZE(system));
    HMODULE realD3d = LoadLibraryW((std::wstring(system) + L"\\d3d9.dll").c_str());
    using CreateD3dFunction = IDirect3D9* (WINAPI *)(UINT);
    auto createD3d = realD3d == nullptr ? nullptr : reinterpret_cast<CreateD3dFunction>(GetProcAddress(realD3d, "Direct3DCreate9"));
    IDirect3D9* d3d = createD3d == nullptr ? nullptr : createD3d(D3D_SDK_VERSION);
    HWND window = CreateWindowExW(0, L"STATIC", L"Font override isolated test", WS_OVERLAPPED,
        0, 0, 320, 160, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    IDirect3DDevice9* device = nullptr;
    if (d3d != nullptr && window != nullptr)
    {
        D3DPRESENT_PARAMETERS parameters{};
        parameters.Windowed = TRUE;
        parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
        parameters.hDeviceWindow = window;
        parameters.BackBufferWidth = 320;
        parameters.BackBufferHeight = 160;
        d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &device);
    }
    bool d3dxTested = false;
    if (device != nullptr)
    {
        HMODULE d3dx = GetModuleHandleW(L"d3dx9_43.dll");
        using CreateD3dxFontFunction = HRESULT (WINAPI *)(void*, const void*, void**);
        auto createFont = d3dx == nullptr ? nullptr : reinterpret_cast<CreateD3dxFontFunction>(GetProcAddress(d3dx, "D3DXCreateFontIndirectW"));
        FontDesc description{};
        description.Height = 17;
        description.Width = 10;
        description.Weight = FW_NORMAL;
        description.CharSet = SHIFTJIS_CHARSET;
        description.Quality = PROOF_QUALITY;
        description.PitchAndFamily = FIXED_PITCH;
        wcscpy_s(description.FaceName, L"MS Gothic");
        void* font = nullptr;
        HRESULT hr = createFont == nullptr ? E_NOINTERFACE : createFont(device, &description, &font);
        if (FAILED(hr) || font == nullptr)
        {
            std::printf("FAIL D3DX create hr=%08lX\n", static_cast<DWORD>(hr));
            ++Failures;
        }
        else
        {
            void** vtable = *reinterpret_cast<void***>(font);
            using GetDCFunction = HDC (WINAPI *)(void*);
            HDC dc = reinterpret_cast<GetDCFunction>(vtable[8])(font);
            CheckFont("D3DX_requested_original_family", dc == nullptr ? nullptr : static_cast<HFONT>(GetCurrentObject(dc, OBJ_FONT)), true);
            reinterpret_cast<IUnknown*>(font)->Release();
            d3dxTested = true;
        }
        device->Release();
    }
    if (window != nullptr)
        DestroyWindow(window);
    if (d3d != nullptr)
        d3d->Release();
    std::printf("RESULT failures=%d d3dx_tested=%s\n", Failures, d3dxTested ? "yes" : "no");
    // Do not FreeLibrary: the production DLL's hooks remain installed until
    // process exit, as they do in the actual game.
    return Failures == 0 ? 0 : 1;
}
