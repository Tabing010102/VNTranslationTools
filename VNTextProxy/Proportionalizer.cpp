#include "pch.h"

using namespace std;

namespace
{
    vector<BYTE> CustomFontData;
    volatile LONG FontSelectionTraceCount = 0;

    DWORD FontFingerprint(const vector<BYTE>& bytes)
    {
        DWORD value = 2166136261u;
        for (BYTE byte : bytes)
            value = (value ^ byte) * 16777619u;
        return value;
    }

    void ReadCustomFontData(const wstring& path)
    {
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr)
            return;
        if (_fseeki64(file, 0, SEEK_END) == 0)
        {
            __int64 length = _ftelli64(file);
            if (length > 0 && length <= 32 * 1024 * 1024 && _fseeki64(file, 0, SEEK_SET) == 0)
            {
                CustomFontData.resize(static_cast<size_t>(length));
                if (fread(CustomFontData.data(), 1, CustomFontData.size(), file) != CustomFontData.size())
                    CustomFontData.clear();
            }
        }
        fclose(file);
    }
}

void Proportionalizer::Init()
{
    static bool initialized = false;
    if (!initialized)
    {
        CustomFontName = LoadCustomFont();
        initialized = true;
    }
    LastFontName = CustomFontName;
}

bool Proportionalizer::ApplyCustomFont(LOGFONTW& info)
{
    if (!ForceCustomFont || CustomFontName.empty())
        return false;
    wcscpy_s(info.lfFaceName, CustomFontName.c_str());
    info.lfCharSet = DEFAULT_CHARSET;
    return true;
}

void Proportionalizer::TraceCustomFontSelection(const char* source, HFONT font)
{
    if (!TraceLog::Enabled)
        return;
    LONG number = InterlockedIncrement(&FontSelectionTraceCount);
    if (number > 32)
        return;
    if (font == nullptr)
    {
        TraceLog::Write("FONT selected source=%s font=null", source);
        return;
    }
    HDC dc = CreateCompatibleDC(nullptr);
    if (dc == nullptr)
        return;
    HGDIOBJ previous = SelectObject(dc, font);
    if (previous == nullptr || previous == HGDI_ERROR)
    {
        DeleteDC(dc);
        TraceLog::Write("FONT selected source=%s SelectObject failed font=%p", source, font);
        return;
    }

    wchar_t face[LF_FACESIZE]{};
    GetTextFaceW(dc, ARRAYSIZE(face), face);
    DWORD size = GetFontData(dc, 0, 0, nullptr, 0);
    vector<BYTE> selectedData;
    if (size != GDI_ERROR && size > 0 && size <= 32 * 1024 * 1024)
    {
        selectedData.resize(size);
        if (GetFontData(dc, 0, 0, selectedData.data(), size) != size)
            selectedData.clear();
    }
    bool match = !CustomFontData.empty() && selectedData == CustomFontData;
    const wchar_t carriers[] = { 0x5F0C, 0x4E10, 0x4E15, 0x4E31, 0x4E36, 0x4E3F };
    WORD glyphs[ARRAYSIZE(carriers)]{};
    DWORD glyphResult = GetGlyphIndicesW(dc, carriers, ARRAYSIZE(carriers), glyphs, GGI_MARK_NONEXISTING_GLYPHS);
    TraceLog::Write(
        "FONT selected #%ld source=%s font=%p requested=%s actual_face=%s sfnt_bytes=%lu expected_bytes=%zu custom_font_match=%s fingerprint=%08lX glyph_result=%lu carriers_gids=%04X,%04X,%04X,%04X,%04X,%04X",
        number, source, font, TraceLog::ToUtf8(CustomFontName).c_str(),
        TraceLog::ToUtf8(face).c_str(), size, CustomFontData.size(), match ? "yes" : "no",
        FontFingerprint(selectedData), glyphResult,
        glyphs[0], glyphs[1], glyphs[2], glyphs[3], glyphs[4], glyphs[5]);
    SelectObject(dc, previous);
    DeleteDC(dc);
}

int Proportionalizer::MeasureStringWidth(const wstring& str, int fontSize)
{
    Font* pFont = FontManager.FetchFont(LastFontName, fontSize, false, false, false);
    return pFont->MeasureStringWidth(str);
}

bool Proportionalizer::AdaptRenderArgs(const wchar_t* pText, int length, int fontSize, int& x, int& y)
{
    static int startX = 0;
    static int lastMonospaceX = 0;
    static int lastMonospaceY = 0;
    static int lastProportionalX = 0;
    static wchar_t lastChar = L'\0';
    static int nextProportionalX = 0;

    if (length != 1)
        return true;

    wchar_t currentChar = pText[0];
    if (HandleFormattingCode(currentChar))
        return false;

    Font* pFont = FontManager.FetchFont(LastFontName, fontSize, Bold, Italic, Underline);

    if (x == 0 || x < lastMonospaceX - 4 || abs(y - lastMonospaceY) > 4)
    {
        // To the left of previously rendered text or different Y -> reset
        startX = x;
        lastMonospaceX = x;
        lastMonospaceY = y;
        lastProportionalX = x;

        lastChar = currentChar;
        nextProportionalX = x + pFont->MeasureCharWidth(currentChar);
    }
    else if (x <= lastMonospaceX + 4)
    {
        // Close to previously rendered text (e.g. shadow) -> calculate offset
        int offset = x - lastMonospaceX;
        x = lastProportionalX + offset;
    }
    else
    {
        // Far to the right of previously rendered text -> next char
        lastMonospaceX = x;

        x = nextProportionalX + pFont->GetKernAmount(lastChar, currentChar);
        lastProportionalX = x;
        lastChar = currentChar;
        nextProportionalX = x + pFont->MeasureCharWidth(currentChar);
    }

    LastLineEnd = nextProportionalX;
    return true;
}

bool Proportionalizer::HandleFormattingCode(wchar_t c)
{
    switch (c)
    {
    case L'龠':
        Bold = !Bold;
        return true;

    case L'籥':
        Italic = !Italic;
        return true;

    case L'鑰':
        Underline = !Underline;
        return true;
    }

    return false;
}

wstring Proportionalizer::LoadCustomFont()
{
    CustomFontFilePath = FindCustomFontFile();
    if (CustomFontFilePath.empty())
    {
        TraceLog::Write("FONT load: no external font found");
        return L"";
    }

    TraceLog::WriteW("FONT load path", CustomFontFilePath);
    int numFonts = AddFontResourceExW(CustomFontFilePath.c_str(), FR_PRIVATE, nullptr);
    TraceLog::Write("FONT AddFontResourceExW result=%d private=yes", numFonts);
    if (numFonts == 0)
        return L"";

    wstring fontFileName = CustomFontFilePath;
    fontFileName.erase(0, fontFileName.rfind(L'\\') + 1);
    fontFileName.erase(fontFileName.find(L'.'), -1);
    if (TraceLog::Enabled)
        ReadCustomFontData(CustomFontFilePath);
    ForceCustomFont = _wcsicmp(fontFileName.c_str(), L"MahoroCHS Gothic") == 0;
    TraceLog::Write("FONT loaded family=%s force=%s file_bytes=%zu fingerprint=%08lX",
        TraceLog::ToUtf8(fontFileName).c_str(), ForceCustomFont ? "yes" : "no",
        CustomFontData.size(), FontFingerprint(CustomFontData));
    return fontFileName;
}

wstring Proportionalizer::FindCustomFontFile()
{
    wchar_t folderPath[MAX_PATH];
    GetModuleFileName(GetModuleHandle(nullptr), folderPath, sizeof(folderPath) / sizeof(wchar_t));
    wchar_t* pLastSlash = wcsrchr(folderPath, L'\\');
    if (pLastSlash != nullptr)
        *pLastSlash = L'\0';

    wstring preferred = wstring(folderPath) + L"\\MahoroCHS Gothic.ttf";
    DWORD attributes = GetFileAttributesW(preferred.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        return preferred;

    vector<wstring> extensions = { L".ttf", L".ttc", L".otf" };
    WIN32_FIND_DATA findData;
    for (const wstring& extension : extensions)
    {
        wstring searchPath = wstring(folderPath) + L"\\*" + extension;
        HANDLE hFind = FindFirstFile(searchPath.c_str(), &findData);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            FindClose(hFind);
            return wstring(folderPath) + L"\\" + findData.cFileName;
        }
    }

    return L"";
}
