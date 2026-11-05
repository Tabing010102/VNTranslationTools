#include "pch.h"

void D3DXProportionalizer::Init()
{
    HMODULE d3dxModule = GetModuleHandleW(L"d3dx9_43.dll");
    if (d3dxModule == nullptr)
        d3dxModule = LoadLibraryW(L"d3dx9_43.dll");

    if (d3dxModule == nullptr)
        return;

    OriginalCreateFontIndirectW = reinterpret_cast<CreateFontIndirectWFunction>(
        GetProcAddress(d3dxModule, "D3DXCreateFontIndirectW")
    );
    if (OriginalCreateFontIndirectW == nullptr)
        return;

    ImportHooker::Hook(
        {
            { "D3DXCreateFontIndirectW", CreateFontIndirectWHook }
        }
    );
}

HRESULT __stdcall D3DXProportionalizer::CreateFontIndirectWHook(void* device, const void* description, void** font)
{
    HRESULT result = OriginalCreateFontIndirectW(device, description, font);
    if (SUCCEEDED(result) && font != nullptr && *font != nullptr)
        PatchFont(*font);

    return result;
}

void D3DXProportionalizer::PatchFont(void* font)
{
    // ID3DXFont's vtable is IUnknown (3 entries), followed by 15 font
    // methods.  PreloadTextA and DrawTextA are entries 12 and 14; their W
    // counterparts are the next entries and remain unmodified.
    void** vtable = *reinterpret_cast<void***>(font);
    if (vtable == nullptr)
        return;

    if (OriginalPreloadTextA == nullptr)
    {
        OriginalPreloadTextA = reinterpret_cast<PreloadTextAFunction>(vtable[12]);
        OriginalPreloadTextW = reinterpret_cast<PreloadTextWFunction>(vtable[13]);
        OriginalDrawTextA = reinterpret_cast<DrawTextAFunction>(vtable[14]);
        OriginalDrawTextW = reinterpret_cast<DrawTextWFunction>(vtable[15]);
    }

    MemoryUnprotector unprotect(&vtable[12], sizeof(void*) * 3);
    vtable[12] = reinterpret_cast<void*>(PreloadTextAHook);
    vtable[14] = reinterpret_cast<void*>(DrawTextAHook);
}

INT D3DXProportionalizer::GetWideCount(const std::wstring& text, INT originalCount)
{
    return originalCount < 0 ? -1 : static_cast<INT>(text.size());
}

HRESULT __stdcall D3DXProportionalizer::PreloadTextAHook(void* font, LPCSTR text, INT count)
{
    std::wstring wideText = SjisTunnelEncoding::Decode(text, count);
    return OriginalPreloadTextW(font, wideText.c_str(), GetWideCount(wideText, count));
}

INT __stdcall D3DXProportionalizer::DrawTextAHook(void* font, void* sprite, LPCSTR text, INT count, RECT* rect, DWORD format, DWORD color)
{
    std::wstring wideText = SjisTunnelEncoding::Decode(text, count);
    return OriginalDrawTextW(font, sprite, wideText.c_str(), GetWideCount(wideText, count), rect, format, color);
}
