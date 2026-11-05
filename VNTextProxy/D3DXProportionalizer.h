#pragma once

// Yaneurao renders its text through ID3DXFont rather than GDI.  The engine
// calls ID3DXFont::DrawTextA/PreloadTextA with the bytes from the .ks file,
// so the normal GDI hooks cannot decode SJIS tunnel characters on this path.
class D3DXProportionalizer
{
public:
    static void Init();

private:
    using CreateFontIndirectWFunction = HRESULT (__stdcall *)(void* device, const void* description, void** font);
    using PreloadTextAFunction = HRESULT (__stdcall *)(void* font, LPCSTR text, INT count);
    using PreloadTextWFunction = HRESULT (__stdcall *)(void* font, LPCWSTR text, INT count);
    using GetDCFunction = HDC (__stdcall *)(void* font);
    using GetGlyphDataFunction = HRESULT (__stdcall *)(void* font, UINT glyph, void** texture, RECT* blackBox, POINT* cellInc);
    using PreloadCharactersFunction = HRESULT (__stdcall *)(void* font, UINT first, UINT last);
    using DrawTextAFunction = INT (__stdcall *)(void* font, void* sprite, LPCSTR text, INT count, RECT* rect, DWORD format, DWORD color);
    using DrawTextWFunction = INT (__stdcall *)(void* font, void* sprite, LPCWSTR text, INT count, RECT* rect, DWORD format, DWORD color);

    static HRESULT __stdcall CreateFontIndirectWHook(void* device, const void* description, void** font);
    static HRESULT __stdcall PreloadTextAHook(void* font, LPCSTR text, INT count);
    static HRESULT __stdcall PreloadTextWHook(void* font, LPCWSTR text, INT count);
    static HDC __stdcall GetDCHook(void* font);
    static HRESULT __stdcall GetGlyphDataHook(void* font, UINT glyph, void** texture, RECT* blackBox, POINT* cellInc);
    static HRESULT __stdcall PreloadCharactersHook(void* font, UINT first, UINT last);
    static HRESULT __stdcall PreloadGlyphsHook(void* font, UINT first, UINT last);
    static INT __stdcall DrawTextAHook(void* font, void* sprite, LPCSTR text, INT count, RECT* rect, DWORD format, DWORD color);
    static INT __stdcall DrawTextWHook(void* font, void* sprite, LPCWSTR text, INT count, RECT* rect, DWORD format, DWORD color);

    static void PatchFont(void* font);
    static INT GetWideCount(const std::wstring& text, INT originalCount);

    static inline CreateFontIndirectWFunction OriginalCreateFontIndirectW{};
    static inline PreloadTextAFunction OriginalPreloadTextA{};
    static inline PreloadTextWFunction OriginalPreloadTextW{};
    static inline GetDCFunction OriginalGetDC{};
    static inline GetGlyphDataFunction OriginalGetGlyphData{};
    static inline PreloadCharactersFunction OriginalPreloadCharacters{};
    static inline PreloadCharactersFunction OriginalPreloadGlyphs{};
    static inline DrawTextAFunction OriginalDrawTextA{};
    static inline DrawTextWFunction OriginalDrawTextW{};
};
