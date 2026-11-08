#include "pch.h"

namespace
{
    struct D3DXFontDescW
    {
        INT Height;
        UINT Width;
        UINT Weight;
        UINT MipLevels;
        BOOL Italic;
        BYTE CharSet;
        BYTE OutputPrecision;
        BYTE Quality;
        BYTE PitchAndFamily;
        WCHAR FaceName[LF_FACESIZE];
    };

    volatile LONG PreloadCallCount = 0;
    volatile LONG DrawCallCount = 0;
    volatile LONG PreloadWideCallCount = 0;
    volatile LONG DrawWideCallCount = 0;
    volatile LONG FontMethodCallCount = 0;

    size_t GetRawTextLength(LPCSTR text, INT count)
    {
        if (text == nullptr)
            return 0;
        if (count >= 0)
            return static_cast<size_t>(count);
        return strnlen_s(text, 2048);
    }

    bool IsTunnelPair(BYTE high, BYTE low)
    {
        if (!((high >= 0x81 && high < 0xA0) || (high >= 0xE0 && high < 0xFD)))
            return false;
        if (low == 0 || low >= 0x40)
            return false;
        return low != '\t' && low != '\n' && low != '\r' && low != ' ' && low != ',';
    }

    bool ContainsTunnelPair(LPCSTR text, INT count)
    {
        size_t length = GetRawTextLength(text, count);
        if (length < 2)
            return false;

        const BYTE* bytes = reinterpret_cast<const BYTE*>(text);
        length = (std::min)(length, static_cast<size_t>(2048));
        for (size_t i = 0; i + 1 < length; i++)
        {
            if (IsTunnelPair(bytes[i], bytes[i + 1]))
                return true;
        }
        return false;
    }

    void LogTextCall(const char* method, volatile LONG* counter, LPCSTR text, INT count, const std::wstring& decoded)
    {
        LONG callNumber = InterlockedIncrement(counter);
        bool containsTunnel = ContainsTunnelPair(text, count);
        if (callNumber > 200 && (!containsTunnel || callNumber > 1000))
            return;

        size_t rawLength = GetRawTextLength(text, count);
        std::string raw = TraceLog::Hex(text, rawLength, 2048);
        std::wstring preview = decoded.substr(0, 1024);
        std::string wide = TraceLog::ToUtf8(preview);
        TraceLog::Write(
            "%s #%ld count=%d raw_len=%zu tunnel=%s raw=[%s] decoded_len=%zu decoded=[%s]",
            method,
            callNumber,
            count,
            rawLength,
            containsTunnel ? "yes" : "no",
            raw.c_str(),
            decoded.size(),
            wide.c_str()
        );
    }

    void LogWideTextCall(const char* method, volatile LONG* counter, LPCWSTR text, INT count)
    {
        LONG callNumber = InterlockedIncrement(counter);
        if (callNumber > 200)
            return;

        size_t length = 0;
        if (text != nullptr)
            length = count < 0 ? wcsnlen_s(text, 2048) : static_cast<size_t>(count);
        std::wstring value = text == nullptr ? std::wstring() : std::wstring(text, (std::min)(length, static_cast<size_t>(2048)));
        std::string utf8 = TraceLog::ToUtf8(value.substr(0, 1024));

        std::string codePoints;
        for (size_t i = 0; i < value.size() && i < 256; i++)
        {
            char point[16]{};
            sprintf_s(point, "%04X", static_cast<unsigned int>(value[i]));
            if (!codePoints.empty())
                codePoints += ' ';
            codePoints += point;
        }

        TraceLog::Write(
            "%s #%ld count=%d wide_len=%zu utf8=[%s] codepoints=[%s]",
            method,
            callNumber,
            count,
            value.size(),
            utf8.c_str(),
            codePoints.c_str()
        );
    }

    void LogFontMethod(const char* method, const char* detail)
    {
        LONG callNumber = InterlockedIncrement(&FontMethodCallCount);
        if (callNumber <= 200)
            TraceLog::Write("D3DX %s #%ld %s", method, callNumber, detail);
    }
}

void D3DXProportionalizer::Init()
{
    TraceLog::Write("D3DX init begin");
    HMODULE d3dxModule = GetModuleHandleW(L"d3dx9_43.dll");
    if (d3dxModule == nullptr)
        d3dxModule = LoadLibraryW(L"d3dx9_43.dll");

    if (d3dxModule == nullptr)
    {
        TraceLog::Write("D3DX init failed: d3dx9_43.dll is not loaded; error=%lu", GetLastError());
        return;
    }

    OriginalCreateFontIndirectW = reinterpret_cast<CreateFontIndirectWFunction>(
        GetProcAddress(d3dxModule, "D3DXCreateFontIndirectW")
    );
    if (OriginalCreateFontIndirectW == nullptr)
    {
        TraceLog::Write("D3DX init failed: D3DXCreateFontIndirectW export missing; module=%p", d3dxModule);
        return;
    }

    TraceLog::Write(
        "D3DX init module=%p export=%p game_import_hook=%p",
        d3dxModule,
        OriginalCreateFontIndirectW,
        reinterpret_cast<void*>(CreateFontIndirectWHook)
    );

    // The packed engine can resolve the D3DX export dynamically from a
    // secondary module, bypassing the main EXE's IAT.  Detour the export as
    // well as patching the main IAT so both call paths are observable.
    LONG detourError = DetourTransactionBegin();
    if (detourError == NO_ERROR)
    {
        detourError = DetourUpdateThread(GetCurrentThread());
        if (detourError == NO_ERROR)
        {
            detourError = DetourAttach(
                reinterpret_cast<PVOID*>(&OriginalCreateFontIndirectW),
                reinterpret_cast<PVOID>(CreateFontIndirectWHook)
            );
        }

        if (detourError == NO_ERROR)
            detourError = DetourTransactionCommit();
        else
            DetourTransactionAbort();
    }

    TraceLog::Write(
        "D3DX export detour result=%ld trampoline=%p",
        detourError,
        OriginalCreateFontIndirectW
    );

    ImportHooker::Hook(
        {
            { "D3DXCreateFontIndirectW", CreateFontIndirectWHook }
        }
    );
    TraceLog::Write("D3DX init end; import hook requested");
}

HRESULT __stdcall D3DXProportionalizer::CreateFontIndirectWHook(void* device, const void* description, void** font)
{
    TraceLog::Write(
        "D3DXCreateFontIndirectW enter device=%p desc=%p out=%p",
        device,
        description,
        font
    );
    if (description != nullptr)
    {
        const D3DXFontDescW* desc = static_cast<const D3DXFontDescW*>(description);
        size_t faceLength = wcsnlen_s(desc->FaceName, LF_FACESIZE);
        std::string face = TraceLog::ToUtf8(std::wstring(desc->FaceName, faceLength));
        TraceLog::Write(
            "D3DX font desc height=%d width=%u weight=%u charset=%u quality=%u face=%s",
            desc->Height,
            desc->Width,
            desc->Weight,
            desc->CharSet,
            desc->Quality,
            face.c_str()
        );
    }

    D3DXFontDescW forced{};
    const void* effectiveDescription = description;
    if (description != nullptr && Proportionalizer::ForceCustomFont)
    {
        forced = *static_cast<const D3DXFontDescW*>(description);
        wcscpy_s(forced.FaceName, Proportionalizer::CustomFontName.c_str());
        forced.CharSet = DEFAULT_CHARSET;
        effectiveDescription = &forced;
        TraceLog::Write("FONT force D3DXCreateFontIndirectW forced=%s height=%d width=%u weight=%u charset=%u",
            TraceLog::ToUtf8(forced.FaceName).c_str(), forced.Height, forced.Width, forced.Weight, forced.CharSet);
    }

    HRESULT result = OriginalCreateFontIndirectW(device, effectiveDescription, font);
    TraceLog::Write(
        "D3DXCreateFontIndirectW leave hr=0x%08lX out_value=%p",
        static_cast<unsigned long>(result),
        font == nullptr ? nullptr : *font
    );
    if (SUCCEEDED(result) && font != nullptr && *font != nullptr)
    {
        if (Proportionalizer::ForceCustomFont)
        {
            void** vtable = *reinterpret_cast<void***>(*font);
            GetDCFunction getDC = OriginalGetDC != nullptr ? OriginalGetDC : reinterpret_cast<GetDCFunction>(vtable[8]);
            HDC dc = getDC(*font);
            Proportionalizer::TraceCustomFontSelection("D3DX create verification (not a game draw call)",
                dc == nullptr ? nullptr : static_cast<HFONT>(GetCurrentObject(dc, OBJ_FONT)));
        }
        PatchFont(*font);
    }

    return result;
}

void D3DXProportionalizer::PatchFont(void* font)
{
    // ID3DXFont's vtable is IUnknown (3 entries), followed by 15 font
    // methods.  PreloadTextA/W and DrawTextA/W are entries 12 through 15.
    void** vtable = *reinterpret_cast<void***>(font);
    if (vtable == nullptr)
    {
        TraceLog::Write("D3DX PatchFont skipped: null vtable font=%p", font);
        return;
    }

    TraceLog::Write(
        "D3DX PatchFont font=%p vtable=%p slots[0..17]=%p,%p,%p,%p,%p,%p,%p,%p,%p,%p,%p,%p,%p,%p,%p,%p,%p,%p",
        font,
        vtable,
        vtable[0], vtable[1], vtable[2], vtable[3], vtable[4], vtable[5],
        vtable[6], vtable[7], vtable[8], vtable[9], vtable[10], vtable[11],
        vtable[12], vtable[13], vtable[14], vtable[15], vtable[16], vtable[17]
    );

    if (OriginalPreloadTextA == nullptr)
    {
        OriginalPreloadTextA = reinterpret_cast<PreloadTextAFunction>(vtable[12]);
        OriginalPreloadTextW = reinterpret_cast<PreloadTextWFunction>(vtable[13]);
        OriginalGetDC = reinterpret_cast<GetDCFunction>(vtable[8]);
        OriginalGetGlyphData = reinterpret_cast<GetGlyphDataFunction>(vtable[9]);
        OriginalPreloadCharacters = reinterpret_cast<PreloadCharactersFunction>(vtable[10]);
        OriginalPreloadGlyphs = reinterpret_cast<PreloadCharactersFunction>(vtable[11]);
        OriginalDrawTextA = reinterpret_cast<DrawTextAFunction>(vtable[14]);
        OriginalDrawTextW = reinterpret_cast<DrawTextWFunction>(vtable[15]);
        TraceLog::Write(
            "D3DX PatchFont originals GetDC=%p GetGlyphData=%p PreloadChars=%p PreloadGlyphs=%p PreloadA=%p PreloadW=%p DrawA=%p DrawW=%p",
            OriginalGetDC,
            OriginalGetGlyphData,
            OriginalPreloadCharacters,
            OriginalPreloadGlyphs,
            OriginalPreloadTextA,
            OriginalPreloadTextW,
            OriginalDrawTextA,
            OriginalDrawTextW
        );
    }

    MemoryUnprotector unprotect(&vtable[8], sizeof(void*) * 8);
    vtable[8] = reinterpret_cast<void*>(GetDCHook);
    vtable[9] = reinterpret_cast<void*>(GetGlyphDataHook);
    vtable[10] = reinterpret_cast<void*>(PreloadCharactersHook);
    vtable[11] = reinterpret_cast<void*>(PreloadGlyphsHook);
    vtable[12] = reinterpret_cast<void*>(PreloadTextAHook);
    vtable[13] = reinterpret_cast<void*>(PreloadTextWHook);
    vtable[14] = reinterpret_cast<void*>(DrawTextAHook);
    vtable[15] = reinterpret_cast<void*>(DrawTextWHook);
    TraceLog::Write(
        "D3DX PatchFont installed vtable[8]=%p vtable[9]=%p vtable[10]=%p vtable[11]=%p vtable[12]=%p vtable[13]=%p vtable[14]=%p vtable[15]=%p",
        vtable[8],
        vtable[9],
        vtable[10],
        vtable[11],
        vtable[12],
        vtable[13],
        vtable[14],
        vtable[15]
    );
}

INT D3DXProportionalizer::GetWideCount(const std::wstring& text, INT originalCount)
{
    return originalCount < 0 ? -1 : static_cast<INT>(text.size());
}

HRESULT __stdcall D3DXProportionalizer::PreloadTextAHook(void* font, LPCSTR text, INT count)
{
    std::wstring wideText = SjisTunnelEncoding::Decode(text, count);
    LogTextCall("PreloadTextA", &PreloadCallCount, text, count, wideText);
    if (OriginalPreloadTextW == nullptr)
    {
        TraceLog::Write("PreloadTextA failed: original W function is null");
        return E_FAIL;
    }
    return OriginalPreloadTextW(font, wideText.c_str(), GetWideCount(wideText, count));
}

HRESULT __stdcall D3DXProportionalizer::PreloadTextWHook(void* font, LPCWSTR text, INT count)
{
    LogWideTextCall("PreloadTextW", &PreloadWideCallCount, text, count);
    if (OriginalPreloadTextW == nullptr)
    {
        TraceLog::Write("PreloadTextW failed: original W function is null");
        return E_FAIL;
    }
    return OriginalPreloadTextW(font, text, count);
}

HDC __stdcall D3DXProportionalizer::GetDCHook(void* font)
{
    LogFontMethod("GetDC", "called");
    if (OriginalGetDC == nullptr)
        return nullptr;
    return OriginalGetDC(font);
}

HRESULT __stdcall D3DXProportionalizer::GetGlyphDataHook(void* font, UINT glyph, void** texture, RECT* blackBox, POINT* cellInc)
{
    char detail[128]{};
    sprintf_s(detail, "glyph=0x%X", glyph);
    LogFontMethod("GetGlyphData", detail);
    if (OriginalGetGlyphData == nullptr)
        return E_FAIL;
    return OriginalGetGlyphData(font, glyph, texture, blackBox, cellInc);
}

HRESULT __stdcall D3DXProportionalizer::PreloadCharactersHook(void* font, UINT first, UINT last)
{
    char detail[128]{};
    sprintf_s(detail, "first=0x%X last=0x%X", first, last);
    LogFontMethod("PreloadCharacters", detail);
    if (OriginalPreloadCharacters == nullptr)
        return E_FAIL;
    return OriginalPreloadCharacters(font, first, last);
}

HRESULT __stdcall D3DXProportionalizer::PreloadGlyphsHook(void* font, UINT first, UINT last)
{
    char detail[128]{};
    sprintf_s(detail, "first=0x%X last=0x%X", first, last);
    LogFontMethod("PreloadGlyphs", detail);
    if (OriginalPreloadGlyphs == nullptr)
        return E_FAIL;
    return OriginalPreloadGlyphs(font, first, last);
}

INT __stdcall D3DXProportionalizer::DrawTextAHook(void* font, void* sprite, LPCSTR text, INT count, RECT* rect, DWORD format, DWORD color)
{
    std::wstring wideText = SjisTunnelEncoding::Decode(text, count);
    LogTextCall("DrawTextA", &DrawCallCount, text, count, wideText);
    if (OriginalDrawTextW == nullptr)
    {
        TraceLog::Write("DrawTextA failed: original W function is null");
        return 0;
    }
    return OriginalDrawTextW(font, sprite, wideText.c_str(), GetWideCount(wideText, count), rect, format, color);
}

INT __stdcall D3DXProportionalizer::DrawTextWHook(void* font, void* sprite, LPCWSTR text, INT count, RECT* rect, DWORD format, DWORD color)
{
    LogWideTextCall("DrawTextW", &DrawWideCallCount, text, count);
    if (OriginalDrawTextW == nullptr)
    {
        TraceLog::Write("DrawTextW failed: original W function is null");
        return 0;
    }
    return OriginalDrawTextW(font, sprite, text, count, rect, format, color);
}
