#include "pch.h"

using namespace std;

void GdiProportionalizer::Init()
{
    Proportionalizer::Init();
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
            { "ExtTextOutA", ExtTextOutAHook },
            { "GetGlyphOutlineA", GetGlyphOutlineAHook }
        }
    );
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
    int result = EnumFontFamiliesExW(hdc, &logFontW, &EnumFontsProc, (LPARAM)&context, dwFlags);
    if (lpLogfont->lfCharSet != SHIFTJIS_CHARSET || result == 0)
        return result;

    // Engines such as QLIE only list fonts that support SHIFTJIS_CHARSET, which rules out Chinese fonts like SimHei.
    // Report the GB2312 fonts as well, disguised as SJIS fonts.
    logFontW.lfCharSet = GB2312_CHARSET;
    return EnumFontFamiliesExW(hdc, &logFontW, &EnumExtraFontsProc, (LPARAM)&context, dwFlags);
}

int GdiProportionalizer::EnumExtraFontsProc(const LOGFONTW* lplf, const TEXTMETRICW* lptm, DWORD dwType, LPARAM lpData)
{
    EnumFontsContext* pContext = (EnumFontsContext*)lpData;
    if ((dwType & RASTER_FONTTYPE) || lplf->lfFaceName[0] == L'@' || pContext->ReportedFaceNames.contains(lplf->lfFaceName))
        return 1;

    ENUMLOGFONTEXDVW logFont{};
    memcpy(&logFont.elfEnumLogfontEx, lplf, sizeof(ENUMLOGFONTEXW));
    logFont.elfDesignVector.dvReserved = STAMP_DESIGNVECTOR;
    logFont.elfEnumLogfontEx.elfLogFont.lfCharSet = SHIFTJIS_CHARSET;

    // Localized names such as 黑体 or 仿宋 may need tunnel characters that aren't in sjis_ext.bin,
    // which would break once the game saves the name. Use the English family name instead.
    wstring name = GetEnglishFamilyName(logFont.elfEnumLogfontEx.elfLogFont);
    if (!name.empty())
    {
        if (pContext->ReportedFaceNames.contains(name))
            return 1;

        wcscpy_s(logFont.elfEnumLogfontEx.elfLogFont.lfFaceName, name.c_str());
        wcscpy_s(logFont.elfEnumLogfontEx.elfFullName, name.c_str());
    }

    if (SjisTunnelEncoding::Encode(logFont.elfEnumLogfontEx.elfLogFont.lfFaceName).size() >= LF_FACESIZE ||
        SjisTunnelEncoding::Encode(logFont.elfEnumLogfontEx.elfFullName).size() >= LF_FULLFACESIZE)
    {
        return 1;
    }

    TEXTMETRICW textMetric = *lptm;
    textMetric.tmCharSet = SHIFTJIS_CHARSET;
    return EnumFontsProc(&logFont.elfEnumLogfontEx.elfLogFont, &textMetric, dwType, lpData);
}

wstring GdiProportionalizer::GetEnglishFamilyName(const LOGFONTW& logFont)
{
    static IDWriteFactory* pFactory = nullptr;
    static IDWriteGdiInterop* pInterop = nullptr;
    if (pInterop == nullptr)
    {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&pFactory)) ||
            FAILED(pFactory->GetGdiInterop(&pInterop)))
        {
            return L"";
        }
    }

    IDWriteFont* pFont = nullptr;
    if (FAILED(pInterop->CreateFontFromLOGFONT(&logFont, &pFont)))
        return L"";

    // The Win32 family names are the ones GDI accepts in LOGFONT (e.g. "Microsoft YaHei Light")
    wstring result;
    IDWriteLocalizedStrings* pNames = nullptr;
    BOOL exists = false;
    if (SUCCEEDED(pFont->GetInformationalStrings(DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES, &pNames, &exists)) && exists)
    {
        UINT32 index;
        BOOL found = false;
        if (SUCCEEDED(pNames->FindLocaleName(L"en-us", &index, &found)) && found)
        {
            UINT32 length = 0;
            pNames->GetStringLength(index, &length);
            result.resize(length + 1);
            pNames->GetString(index, result.data(), length + 1);
            result.resize(length);
        }
        pNames->Release();
    }
    pFont->Release();

    if (result.size() >= LF_FACESIZE || std::ranges::any_of(result, [](wchar_t c) { return c < 0x20 || c > 0x7E; }))
        return L"";

    return result;
}

bool GdiProportionalizer::FontSupportsCharSet(const wchar_t* pFaceName, BYTE charSet)
{
    static map<pair<wstring, BYTE>, bool> cache;
    pair<wstring, BYTE> key(pFaceName, charSet);
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;

    LOGFONTW logFont{};
    logFont.lfCharSet = charSet;
    wcscpy_s(logFont.lfFaceName, pFaceName);

    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &logFont, &FontFoundProc, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);

    cache[key] = found;
    return found;
}

int GdiProportionalizer::FontFoundProc(const LOGFONTW* lplf, const TEXTMETRICW* lptm, DWORD dwType, LPARAM lpData)
{
    *(bool*)lpData = true;
    return 0;
}

int GdiProportionalizer::EnumFontsProc(const LOGFONTW* lplf, const TEXTMETRICW* lptm, DWORD dwType, LPARAM lpData)
{
    EnumFontsContext* pContext = (EnumFontsContext*)lpData;
    pContext->ReportedFaceNames.insert(lplf->lfFaceName);
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
    if (CustomFontName.empty())
    {
        LOGFONTW fontInfo = *pFontInfo;

        // Engines such as QLIE always request SHIFTJIS_CHARSET, which makes GDI substitute a Japanese font
        // for fonts that don't support it (e.g. SimHei). All text goes through the W functions anyway.
        if (fontInfo.lfCharSet == SHIFTJIS_CHARSET && fontInfo.lfFaceName[0] != L'\0' && !FontSupportsCharSet(fontInfo.lfFaceName, SHIFTJIS_CHARSET))
            fontInfo.lfCharSet = DEFAULT_CHARSET;

        LastFontName = fontInfo.lfFaceName;
        return FontManager.FetchFont(fontInfo)->GetGdiHandle();
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
    BOOL result = GetTextExtentPoint32W(hdc, str.c_str(), str.size(), psizl);
#ifdef VNTEXTPROXY_DIAG
    DiagLog(hdc, "GetTextExtentPoint32A", lpString, c, str, 0, 0, psizl);
#endif
    return result;
}

#ifdef VNTEXTPROXY_DIAG
void GdiProportionalizer::DiagLog(HDC dc, const char* pFunction, LPCSTR pString, int count, const wstring& text, int x, int y, const SIZE* pSize)
{
    static FILE* pFile = nullptr;
    if (pFile == nullptr)
    {
        _wfopen_s(&pFile, Path::Combine(Path::GetModuleFolderPath(nullptr), L"vntextproxy_diag.log").c_str(), L"wb");
        if (pFile == nullptr)
            return;
    }

    auto toUtf8 = [](const wchar_t* pText, int length)
    {
        string result(length * 4 + 1, '\0');
        result.resize(WideCharToMultiByte(CP_UTF8, 0, pText, length, result.data(), result.size(), nullptr, nullptr));
        return result;
    };

    wchar_t face[LF_FACESIZE]{};
    GetTextFaceW(dc, LF_FACESIZE, face);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);

    if (count < 0)
        count = pString != nullptr ? strlen(pString) : 0;

    fprintf(pFile, "%s bytes=[", pFunction);
    for (int i = 0; i < count; i++)
        fprintf(pFile, i == 0 ? "%02X" : " %02X", (BYTE)pString[i]);

    fprintf(pFile, "] text=\"%s\" chars=[", toUtf8(text.data(), text.size()).c_str());
    for (size_t i = 0; i < text.size(); i++)
        fprintf(pFile, i == 0 ? "%04X" : " %04X", text[i]);

    fprintf(pFile, "] pos=%d,%d", x, y);
    if (pSize != nullptr)
        fprintf(pFile, " size=%dx%d", pSize->cx, pSize->cy);

    fprintf(pFile, " face=\"%s\" height=%d charset=%d\r\n", toUtf8(face, wcslen(face)).c_str(), metrics.tmHeight, metrics.tmCharSet);
    fflush(pFile);
}
#endif

BOOL GdiProportionalizer::TextOutAHook(HDC dc, int x, int y, LPCSTR pString, int count)
{
    wstring text = SjisTunnelEncoding::Decode(pString, count);
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

BOOL GdiProportionalizer::ExtTextOutAHook(HDC dc, int x, int y, UINT options, const RECT* pRect, LPCSTR pString, UINT count, const INT* pDx)
{
    // Delphi's TCanvas.TextOut (used by e.g. QLIE to render each glyph) goes through ExtTextOutA rather than TextOutA
    if ((options & ETO_GLYPH_INDEX) || pString == nullptr || count == 0)
        return ExtTextOutA(dc, x, y, options, pRect, pString, count, pDx);

    if (pDx == nullptr)
    {
        wstring text = SjisTunnelEncoding::Decode(pString, count);
#ifdef VNTEXTPROXY_DIAG
        DiagLog(dc, "ExtTextOutA", pString, count, text, x, y, nullptr);
#endif
        return ExtTextOutW(dc, x, y, options, pRect, text.data(), text.size(), nullptr);
    }

    // The ANSI version has one spacing entry (or two with ETO_PDY) per byte; merge them per character
    int entriesPerByte = (options & ETO_PDY) ? 2 : 1;
    wstring text;
    vector<INT> dx;
    UINT i = 0;
    while (i < count)
    {
        UINT charLength = IsSjisLeadByte((BYTE)pString[i]) && i + 1 < count ? 2 : 1;
        wstring chars = SjisTunnelEncoding::Decode(pString + i, charLength);
        if (chars.empty())
            chars = L"?";

        for (int entry = 0; entry < entriesPerByte; entry++)
        {
            INT distance = 0;
            for (UINT byte = 0; byte < charLength; byte++)
            {
                distance += pDx[(i + byte) * entriesPerByte + entry];
            }
            dx.push_back(distance);
        }
        for (size_t extra = 1; extra < chars.size(); extra++)
        {
            dx.insert(dx.end(), entriesPerByte, 0);
        }

        text += chars;
        i += charLength;
    }
    return ExtTextOutW(dc, x, y, options, pRect, text.data(), text.size(), dx.data());
}

bool GdiProportionalizer::IsSjisLeadByte(BYTE byte)
{
    return (byte >= 0x81 && byte < 0xA0) || (byte >= 0xE0 && byte < 0xFD);
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
