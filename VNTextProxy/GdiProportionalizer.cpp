#include "pch.h"
#include <intrin.h>

using namespace std;

namespace
{
    volatile LONG GdiCreateFontCallCount = 0;
    volatile LONG GdiTextOutCallCount = 0;
    volatile LONG NativeFontCallCount = 0;
    thread_local bool InsideNativeFontHook = false;

    struct NativeFontHookScope
    {
        NativeFontHookScope() { InsideNativeFontHook = true; }
        ~NativeFontHookScope() { InsideNativeFontHook = false; }
    };
}

void GdiProportionalizer::Init()
{
    Proportionalizer::Init();
    // Packed Yaneurao resolves the real GDI entry separately from its on-disk
    // IAT. Hook just font creation, not GetProcAddress or the loader APIs.
    InstallNativeFontHook();
    ImportHooker::Hook(
        {
            { "EnumFontsA", EnumFontsAHook },
            { "EnumFontFamiliesExA", EnumFontFamiliesExAHook },
            { "CreateFontA", CreateFontAHook },
            { "CreateFontIndirectA", CreateFontIndirectAHook },
            { "CreateFontW", CreateFontWHook },
            { "CreateFontIndirectW", CreateFontIndirectWHook },
            { "SelectObject", SelectObjectHook },
            { "DeleteObject", DeleteObjectHook },
            { "GetTextExtentPointA", GetTextExtentPointAHook },
            { "GetTextExtentPoint32A", GetTextExtentPoint32AHook },
            { "TextOutA", TextOutAHook },
            { "GetGlyphOutlineA", GetGlyphOutlineAHook }
        }
    );
}

void GdiProportionalizer::InstallNativeFontHook()
{
    if (!ForceCustomFont)
        return;
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    OriginalNativeCreateFontIndirectW = gdi == nullptr ? nullptr :
        reinterpret_cast<NativeCreateFontIndirectWFunction>(GetProcAddress(gdi, "CreateFontIndirectW"));
    if (OriginalNativeCreateFontIndirectW == nullptr)
    {
        TraceLog::Write("FONT native CreateFontIndirectW export unavailable");
        return;
    }

    LONG error = DetourTransactionBegin();
    if (error == NO_ERROR)
    {
        error = DetourUpdateThread(GetCurrentThread());
        if (error == NO_ERROR)
            error = DetourAttach(reinterpret_cast<PVOID*>(&OriginalNativeCreateFontIndirectW),
                reinterpret_cast<PVOID>(NativeCreateFontIndirectWHook));
        if (error == NO_ERROR)
            error = DetourTransactionCommit();
        else
            DetourTransactionAbort();
    }
    TraceLog::Write("FONT native CreateFontIndirectW detour result=%ld trampoline=%p", error, OriginalNativeCreateFontIndirectW);
}

HFONT WINAPI GdiProportionalizer::NativeCreateFontIndirectWHook(const LOGFONTW* info)
{
    if (info == nullptr || !ForceCustomFont || InsideNativeFontHook)
        return OriginalNativeCreateFontIndirectW(info);

    NativeFontHookScope guard;
    LOGFONTW forced = *info;
    ApplyCustomFont(forced);
    LONG number = InterlockedIncrement(&NativeFontCallCount);
    if (TraceLog::Enabled && number <= 64)
    {
        wstring originalFace(info->lfFaceName, wcsnlen_s(info->lfFaceName, LF_FACESIZE));
        TraceLog::Write("FONT force GDI CreateFontIndirectW #%ld caller=%p original=%s forced=%s height=%ld width=%ld weight=%ld charset=%u->%u pitch=%u",
            number, _ReturnAddress(), TraceLog::ToUtf8(originalFace).c_str(),
            TraceLog::ToUtf8(forced.lfFaceName).c_str(), forced.lfHeight, forced.lfWidth,
            forced.lfWeight, info->lfCharSet, forced.lfCharSet, forced.lfPitchAndFamily);
    }
    // Do not route this callback through FontManager: Font constructs its HFONT
    // with CreateFontIndirectW, which would otherwise recurse into this hook.
    HFONT font = OriginalNativeCreateFontIndirectW(&forced);
    DWORD error = GetLastError();
    TraceCustomFontSelection("GDI native create", font);
    SetLastError(error);
    return font;
}

int GdiProportionalizer::EnumFontsAHook(HDC hdc, LPCSTR lpLogfont, FONTENUMPROCA lpProc, LPARAM lParam)
{
    EnumFontsContext context;
    context.OriginalProc = lpProc;
    context.OriginalContext = lParam;
    context.Extended = false;
    return EnumFontsW(hdc, lpLogfont != nullptr ? SjisTunnelEncoding::Decode(lpLogfont).c_str() : nullptr, &EnumFontsProc, (LPARAM)&context);
}

int GdiProportionalizer::EnumFontFamiliesExAHook(HDC hdc, LPLOGFONTA lpLogfont, FONTENUMPROCA lpProc, LPARAM lParam, DWORD dwFlags)
{
    LOGFONTW logFontW = ConvertLogFontAToW(*lpLogfont);
    EnumFontsContext context;
    context.OriginalProc = lpProc;
    context.OriginalContext = lParam;
    context.Extended = true;
    return EnumFontFamiliesExW(hdc, &logFontW, &EnumFontsProc, (LPARAM)&context, dwFlags);
}

int GdiProportionalizer::EnumFontsProc(const LOGFONTW* lplf, const TEXTMETRICW* lptm, DWORD dwType, LPARAM lpData)
{
    EnumFontsContext* pContext = (EnumFontsContext*)lpData;
    ENUMLOGFONTEXDVA logFontExA;
    logFontExA.elfEnumLogfontEx.elfLogFont = ConvertLogFontWToA(*lplf);
    if (pContext->Extended)
    {
        ENUMLOGFONTEXDVW* pLogFontExW = (ENUMLOGFONTEXDVW*)lplf;
        strcpy_s((char*)logFontExA.elfEnumLogfontEx.elfFullName, sizeof(logFontExA.elfEnumLogfontEx.elfFullName), SjisTunnelEncoding::Encode(pLogFontExW->elfEnumLogfontEx.elfFullName).c_str());
        strcpy_s((char*)logFontExA.elfEnumLogfontEx.elfScript,   sizeof(logFontExA.elfEnumLogfontEx.elfScript),   SjisTunnelEncoding::Encode(pLogFontExW->elfEnumLogfontEx.elfScript).c_str());
        strcpy_s((char*)logFontExA.elfEnumLogfontEx.elfStyle,    sizeof(logFontExA.elfEnumLogfontEx.elfStyle),    SjisTunnelEncoding::Encode(pLogFontExW->elfEnumLogfontEx.elfStyle).c_str());
        logFontExA.elfDesignVector = pLogFontExW->elfDesignVector;
    }

    TEXTMETRICA textMetricA = ConvertTextMetricWToA(*lptm);
    return pContext->OriginalProc(&logFontExA.elfEnumLogfontEx.elfLogFont, &textMetricA, dwType, pContext->OriginalContext);
}

HFONT GdiProportionalizer::CreateFontAHook(int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight,
    DWORD bItalic, DWORD bUnderline, DWORD bStrikeOut, DWORD iCharSet, DWORD iOutPrecision, DWORD iClipPrecision,
    DWORD iQuality, DWORD iPitchAndFamily, LPCSTR pszFaceName)
{
    LONG callNumber = InterlockedIncrement(&GdiCreateFontCallCount);
    if (callNumber <= 20)
    {
        std::string faceName = pszFaceName == nullptr ? "<null>" : TraceLog::Hex(pszFaceName, strnlen_s(pszFaceName, 128), 128);
        TraceLog::Write("GDI CreateFontA #%ld height=%d charset=%u face_bytes=[%s]", callNumber, cHeight, iCharSet, faceName.c_str());
    }

    return CreateFontWHook(
        cHeight,
        cWidth,
        cEscapement,
        cOrientation,
        cWeight,
        bItalic,
        bUnderline,
        bStrikeOut,
        iCharSet,
        iOutPrecision,
        iClipPrecision,
        iQuality,
        iPitchAndFamily,
        StringUtil::ToWString(pszFaceName).c_str()
    );
}

HFONT GdiProportionalizer::CreateFontIndirectAHook(LOGFONTA* pFontInfo)
{
    return CreateFontWHook(
        pFontInfo->lfHeight,
        pFontInfo->lfWidth,
        pFontInfo->lfEscapement,
        pFontInfo->lfOrientation,
        pFontInfo->lfWeight,
        pFontInfo->lfItalic,
        pFontInfo->lfUnderline,
        pFontInfo->lfStrikeOut,
        pFontInfo->lfCharSet,
        pFontInfo->lfOutPrecision,
        pFontInfo->lfClipPrecision,
        pFontInfo->lfQuality,
        pFontInfo->lfPitchAndFamily,
        StringUtil::ToWString(pFontInfo->lfFaceName).c_str()
    );
}

HFONT GdiProportionalizer::CreateFontWHook(int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight,
    DWORD bItalic, DWORD bUnderline, DWORD bStrikeOut, DWORD iCharSet, DWORD iOutPrecision, DWORD iClipPrecision,
    DWORD iQuality, DWORD iPitchAndFamily, LPCWSTR pszFaceName)
{
    LOGFONTW fontInfo;
    fontInfo.lfHeight = cHeight;
    fontInfo.lfWidth = cWidth;
    fontInfo.lfEscapement = cEscapement;
    fontInfo.lfOrientation = cOrientation;
    fontInfo.lfWeight = cWeight;
    fontInfo.lfItalic = bItalic;
    fontInfo.lfUnderline = bUnderline;
    fontInfo.lfStrikeOut = bStrikeOut;
    fontInfo.lfCharSet = iCharSet;
    fontInfo.lfOutPrecision = iOutPrecision;
    fontInfo.lfClipPrecision = iClipPrecision;
    fontInfo.lfQuality = iQuality;
    fontInfo.lfPitchAndFamily = iPitchAndFamily;
    wcscpy_s(fontInfo.lfFaceName, pszFaceName);
    return CreateFontIndirectWHook(&fontInfo);
}

HFONT GdiProportionalizer::CreateFontIndirectWHook(LOGFONTW* pFontInfo)
{
    if (ForceCustomFont)
    {
        LOGFONTW forced = *pFontInfo;
        ApplyCustomFont(forced);
        LastFontName = CustomFontName;
        // Preserve the engine's width, weight, quality and style parameters.
        return FontManager.FetchFont(forced)->GetGdiHandle();
    }

    if (CustomFontName.empty())
    {
        LastFontName = pFontInfo->lfFaceName;
        return FontManager.FetchFont(*pFontInfo)->GetGdiHandle();
    }

    LastFontName = CustomFontName;
    return FontManager.FetchFont(CustomFontName, pFontInfo->lfHeight, Bold, Italic, Underline)->GetGdiHandle();
}

HGDIOBJ GdiProportionalizer::SelectObjectHook(HDC hdc, HGDIOBJ obj)
{
    Font* pFont = FontManager.GetFont(static_cast<HFONT>(obj));
    if (pFont != nullptr)
        CurrentFonts[hdc] = pFont;

    return SelectObject(hdc, obj);
}

BOOL GdiProportionalizer::DeleteObjectHook(HGDIOBJ obj)
{
    Font* pFont = FontManager.GetFont(static_cast<HFONT>(obj));
    if (pFont != nullptr)
        return false;

    return DeleteObject(obj);
}

BOOL GdiProportionalizer::GetTextExtentPointAHook(HDC hdc, LPCSTR lpString, int c, LPSIZE lpsz)
{
    wstring str = SjisTunnelEncoding::Decode(lpString, c);
    return GetTextExtentPointW(hdc, str.c_str(), str.size(), lpsz);
}

BOOL GdiProportionalizer::GetTextExtentPoint32AHook(HDC hdc, LPCSTR lpString, int c, LPSIZE psizl)
{
    wstring str = SjisTunnelEncoding::Decode(lpString, c);
    return GetTextExtentPoint32W(hdc, str.c_str(), str.size(), psizl);
}

BOOL GdiProportionalizer::TextOutAHook(HDC dc, int x, int y, LPCSTR pString, int count)
{
    wstring text = SjisTunnelEncoding::Decode(pString, count);
    LONG callNumber = InterlockedIncrement(&GdiTextOutCallCount);
    if (callNumber <= 100)
    {
        size_t rawLength = count < 0 ? strnlen_s(pString, 2048) : static_cast<size_t>(count);
        std::string decoded = TraceLog::ToUtf8(text.substr(0, 1024));
        TraceLog::Write(
            "GDI TextOutA #%ld count=%d raw=[%s] decoded=[%s]",
            callNumber,
            count,
            TraceLog::Hex(pString, rawLength, 2048).c_str(),
            decoded.c_str()
        );
    }

    Font* pFont = CurrentFonts[dc];
    if (pFont == nullptr)
    {
        pFont = FontManager.FetchFont(L"System", 12, false, false, false);
        SelectObjectHook(dc, pFont->GetGdiHandle());
    }

    if (!AdaptRenderArgs(text.c_str(), text.size(), pFont->GetHeight(), x, y))
        return false;

    if (!CustomFontName.empty() && (pFont->IsBold() != Bold || pFont->IsItalic() != Italic || pFont->IsUnderline() != Underline))
    {
        pFont = FontManager.FetchFont(CustomFontName, pFont->GetHeight(), Bold, Italic, Underline);
        SelectObjectHook(dc, pFont->GetGdiHandle());
    }

    return TextOutW(dc, x, y, text.data(), text.size());
}

DWORD GdiProportionalizer::GetGlyphOutlineAHook(HDC hdc, UINT uChar, UINT fuFormat, LPGLYPHMETRICS lpgm, DWORD cjBuffer, LPVOID pvBuffer, MAT2* lpmat2)
{
    string str;
    while (uChar != 0)
    {
        str.insert(0, 1, (char)uChar);
        uChar >>= 8;
    }
    wstring wstr = SjisTunnelEncoding::Decode(str);
    return GetGlyphOutlineW(hdc, wstr[0], fuFormat, lpgm, cjBuffer, pvBuffer, lpmat2);
}

LOGFONTA GdiProportionalizer::ConvertLogFontWToA(const LOGFONTW& logFontW)
{
    LOGFONTA logFontA;
    logFontA.lfCharSet = logFontW.lfCharSet;
    logFontA.lfClipPrecision = logFontW.lfClipPrecision;
    logFontA.lfEscapement = logFontW.lfEscapement;
    strcpy_s(logFontA.lfFaceName, SjisTunnelEncoding::Encode(logFontW.lfFaceName).c_str());
    logFontA.lfHeight = logFontW.lfHeight;
    logFontA.lfItalic = logFontW.lfItalic;
    logFontA.lfOrientation = logFontW.lfOrientation;
    logFontA.lfOutPrecision = logFontW.lfOutPrecision;
    logFontA.lfPitchAndFamily = logFontW.lfPitchAndFamily;
    logFontA.lfQuality = logFontW.lfQuality;
    logFontA.lfStrikeOut = logFontW.lfStrikeOut;
    logFontA.lfUnderline = logFontW.lfUnderline;
    logFontA.lfWeight = logFontW.lfWeight;
    logFontA.lfWidth = logFontW.lfWidth;
    return logFontA;
}

LOGFONTW GdiProportionalizer::ConvertLogFontAToW(const LOGFONTA& logFontA)
{
    LOGFONTW logFontW;
    logFontW.lfCharSet = logFontA.lfCharSet;
    logFontW.lfClipPrecision = logFontA.lfClipPrecision;
    logFontW.lfEscapement = logFontA.lfEscapement;
    wcscpy_s(logFontW.lfFaceName, SjisTunnelEncoding::Decode(logFontA.lfFaceName).c_str());
    logFontW.lfHeight = logFontA.lfHeight;
    logFontW.lfItalic = logFontA.lfItalic;
    logFontW.lfOrientation = logFontA.lfOrientation;
    logFontW.lfOutPrecision = logFontA.lfOutPrecision;
    logFontW.lfPitchAndFamily = logFontA.lfPitchAndFamily;
    logFontW.lfQuality = logFontA.lfQuality;
    logFontW.lfStrikeOut = logFontA.lfStrikeOut;
    logFontW.lfUnderline = logFontA.lfUnderline;
    logFontW.lfWeight = logFontA.lfWeight;
    logFontW.lfWidth = logFontA.lfWidth;
    return logFontW;
}

TEXTMETRICA GdiProportionalizer::ConvertTextMetricWToA(const TEXTMETRICW& textMetricW)
{
    TEXTMETRICA textMetricA;
    textMetricA.tmAscent = textMetricW.tmAscent;
    textMetricA.tmAveCharWidth = textMetricW.tmAveCharWidth;
    textMetricA.tmBreakChar = textMetricW.tmBreakChar < 0x100 ? (BYTE)textMetricW.tmBreakChar : '?';
    textMetricA.tmCharSet = textMetricW.tmCharSet;
    textMetricA.tmDefaultChar = textMetricW.tmDefaultChar < 0x100 ? (BYTE)textMetricW.tmDefaultChar : '?';
    textMetricA.tmDescent = textMetricW.tmDescent;
    textMetricA.tmDigitizedAspectX = textMetricW.tmDigitizedAspectX;
    textMetricA.tmDigitizedAspectY = textMetricW.tmDigitizedAspectY;
    textMetricA.tmExternalLeading = textMetricW.tmExternalLeading;
    textMetricA.tmFirstChar = (BYTE)min(textMetricW.tmFirstChar, 0xFF);
    textMetricA.tmHeight = textMetricW.tmHeight;
    textMetricA.tmInternalLeading = textMetricW.tmInternalLeading;
    textMetricA.tmItalic = textMetricW.tmItalic;
    textMetricA.tmLastChar = (BYTE)min(textMetricW.tmLastChar, 0xFF);
    textMetricA.tmMaxCharWidth = textMetricW.tmMaxCharWidth;
    textMetricA.tmOverhang = textMetricW.tmOverhang;
    textMetricA.tmPitchAndFamily = textMetricW.tmPitchAndFamily;
    textMetricA.tmStruckOut = textMetricW.tmStruckOut;
    textMetricA.tmUnderlined = textMetricW.tmUnderlined;
    textMetricA.tmWeight = textMetricW.tmWeight;
    return textMetricA;
}
