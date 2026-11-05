#include "pch.h"

using namespace std;

namespace
{
    bool IsSafeYaneuraoFallback(const char* functionName)
    {
        // This executable uses an import table with OriginalFirstThunk=0.
        // Only the two font entry points need the file-based fallback here.
        // Patching LoadLibraryA, MessageBoxA, or GetProcAddress changes the
        // authentication environment and can break DMM's dynamic loader.
        return strcmp(functionName, "D3DXCreateFontIndirectW") == 0 ||
            strcmp(functionName, "CreateFontA") == 0;
    }

    DWORD RvaToFileOffset(DWORD rva, const IMAGE_NT_HEADERS32* ntHeaders)
    {
        const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(ntHeaders);
        for (WORD i = 0; i < ntHeaders->FileHeader.NumberOfSections; i++)
        {
            DWORD sectionStart = sections[i].VirtualAddress;
            DWORD sectionSize = (std::max)(sections[i].SizeOfRawData, sections[i].Misc.VirtualSize);
            if (rva >= sectionStart && rva < sectionStart + sectionSize)
                return sections[i].PointerToRawData + (rva - sectionStart);
        }
        return 0;
    }

    bool IsValidRva(DWORD rva, size_t fileSize)
    {
        return rva != 0 && static_cast<size_t>(rva) < fileSize;
    }
}

int ImportHooker::PatchImportsFromFile(HMODULE hModule, const map<string, void*>& replacementFuncs)
{
    wchar_t modulePath[32768]{};
    DWORD pathLength = GetModuleFileNameW(hModule, modulePath, ARRAYSIZE(modulePath));
    if (pathLength == 0 || pathLength >= ARRAYSIZE(modulePath))
    {
        TraceLog::Write("ImportHooker fallback: GetModuleFileNameW failed error=%lu", GetLastError());
        return 0;
    }

    HANDLE file = CreateFileW(modulePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        TraceLog::Write("ImportHooker fallback: cannot open exe error=%lu", GetLastError());
        return 0;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart <= 0 || fileSize.QuadPart > 256 * 1024 * 1024)
    {
        CloseHandle(file);
        TraceLog::Write("ImportHooker fallback: invalid exe file size");
        return 0;
    }

    vector<BYTE> image(static_cast<size_t>(fileSize.QuadPart));
    DWORD bytesRead = 0;
    bool readOk = ReadFile(file, image.data(), static_cast<DWORD>(image.size()), &bytesRead, nullptr) != FALSE && bytesRead == image.size();
    CloseHandle(file);
    if (!readOk)
    {
        TraceLog::Write("ImportHooker fallback: cannot read exe error=%lu", GetLastError());
        return 0;
    }

    if (image.size() < sizeof(IMAGE_DOS_HEADER))
        return 0;
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > image.size())
        return 0;

    const IMAGE_NT_HEADERS32* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return 0;

    const IMAGE_DATA_DIRECTORY& importDirectory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    DWORD descriptorOffset = RvaToFileOffset(importDirectory.VirtualAddress, nt);
    if (!IsValidRva(descriptorOffset, image.size()))
        return 0;

    HMODULE loadedModule = hModule == nullptr ? GetModuleHandleW(nullptr) : hModule;
    int patchedCount = 0;
    const IMAGE_IMPORT_DESCRIPTOR* descriptors = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(image.data() + descriptorOffset);
    size_t maxDescriptors = (image.size() - descriptorOffset) / sizeof(IMAGE_IMPORT_DESCRIPTOR);

    for (size_t descriptorIndex = 0; descriptorIndex < maxDescriptors; descriptorIndex++)
    {
        const IMAGE_IMPORT_DESCRIPTOR& descriptor = descriptors[descriptorIndex];
        if (descriptor.OriginalFirstThunk == 0 && descriptor.FirstThunk == 0)
            break;

        DWORD nameOffset = RvaToFileOffset(descriptor.Name, nt);
        DWORD thunkRva = descriptor.OriginalFirstThunk != 0 ? descriptor.OriginalFirstThunk : descriptor.FirstThunk;
        DWORD thunkOffset = RvaToFileOffset(thunkRva, nt);
        if (!IsValidRva(nameOffset, image.size()) || !IsValidRva(thunkOffset, image.size()))
            continue;

        const char* importedModule = reinterpret_cast<const char*>(image.data() + nameOffset);
        const IMAGE_THUNK_DATA32* thunks = reinterpret_cast<const IMAGE_THUNK_DATA32*>(image.data() + thunkOffset);
        size_t maxThunks = (image.size() - thunkOffset) / sizeof(IMAGE_THUNK_DATA32);
        for (size_t thunkIndex = 0; thunkIndex < maxThunks; thunkIndex++)
        {
            const IMAGE_THUNK_DATA32& thunk = thunks[thunkIndex];
            if (thunk.u1.AddressOfData == 0)
                break;
            if (IMAGE_SNAP_BY_ORDINAL32(thunk.u1.Ordinal))
                continue;

            DWORD importByNameOffset = RvaToFileOffset(thunk.u1.AddressOfData, nt);
            if (!IsValidRva(importByNameOffset, image.size()) || importByNameOffset + sizeof(WORD) >= image.size())
                continue;

            const char* functionName = reinterpret_cast<const char*>(image.data() + importByNameOffset + sizeof(WORD));
            auto replacement = replacementFuncs.find(functionName);
            if (replacement == replacementFuncs.end())
                continue;
            if (!IsSafeYaneuraoFallback(functionName))
            {
                TraceLog::Write("IAT fallback skipped authentication-sensitive name=%s", functionName);
                continue;
            }

            void** iatSlot = reinterpret_cast<void**>(reinterpret_cast<BYTE*>(loadedModule) + descriptor.FirstThunk + thunkIndex * sizeof(void*));
            TraceLog::Write(
                "IAT fallback patch module=%s name=%s slot=%p old=%p new=%p",
                importedModule,
                functionName,
                iatSlot,
                *iatSlot,
                replacement->second
            );
            MemoryUnprotector unprotect(iatSlot, sizeof(void*));
            *iatSlot = replacement->second;
            patchedCount++;
        }
    }

    return patchedCount;
}

void ImportHooker::Hook(const map<string, void*>& replacementFuncs)
{
    Init();

    for (const auto& pair : replacementFuncs)
        TraceLog::Write("ImportHooker request name=%s replacement=%p", pair.first.c_str(), pair.second);

    for (auto pair : replacementFuncs)
    {
        ReplacementFuncs.insert(pair);
    }

    HMODULE hExe = GetModuleHandle(nullptr);
    BOOL result = DetourEnumerateImportsEx(hExe, (void*)&replacementFuncs, nullptr, PatchGameImport);
    TraceLog::Write("ImportHooker enumerate exe=%p result=%d error=%lu", hExe, result, result ? 0 : GetLastError());

    int fallbackCount = PatchImportsFromFile(hExe, replacementFuncs);
    TraceLog::Write("ImportHooker fallback patched=%d", fallbackCount);
}

void ImportHooker::ApplyToModule(HMODULE hModule)
{
    DetourEnumerateImportsEx(hModule, (void*)&ReplacementFuncs, nullptr, PatchGameImport);
}

void ImportHooker::Init()
{
    if (Initialized)
        return;

    Initialized = true;
    // Do not replace GetProcAddress globally.  The old implementation could
    // return a VNTextProxy hook for an unrelated DMM function with the same
    // name, which is unsafe during authentication.  Static imports are
    // patched directly by Hook/fallback instead.
    TraceLog::Write("ImportHooker initialized without GetProcAddress hook");
}

BOOL ImportHooker::PatchGameImport(void* pContext, DWORD nOrdinal, LPCSTR pszFunc, void** ppvFunc)
{
    if (pszFunc != nullptr)
    {
        static volatile LONG callbackCount = 0;
        LONG number = InterlockedIncrement(&callbackCount);
        if (number <= 250)
            TraceLog::Write("ImportHooker callback #%ld name=%s slot=%p", number, pszFunc, ppvFunc);
    }

    if (pszFunc == nullptr || ppvFunc == nullptr)
        return true;

    map<string, void*>* pReplacementFuncs = (map<string, void*>*)pContext;
    auto it = pReplacementFuncs->find(pszFunc);
    if (it != pReplacementFuncs->end())
    {
        if (strcmp(pszFunc, "D3DXCreateFontIndirectW") == 0 || strcmp(pszFunc, "GetProcAddress") == 0)
        {
            TraceLog::Write(
                "IAT patch name=%s slot=%p old=%p new=%p",
                pszFunc,
                ppvFunc,
                *ppvFunc,
                it->second
            );
        }
        MemoryUnprotector unprotect(ppvFunc, 4);
        *ppvFunc = it->second;
    }

    return true;
}

FARPROC ImportHooker::GetProcAddressHook(HMODULE hModule, LPCSTR lpProcName)
{
    auto it = ReplacementFuncs.find(lpProcName);
    if (it != ReplacementFuncs.end())
        return (FARPROC)it->second;

    return GetProcAddress(hModule, lpProcName);
}
