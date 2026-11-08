#pragma once

// Relaunches the game through Locale Emulator's core DLLs (LoaderDll.dll and
// LocaleEmulator.dll from Locale Emulator 2.5.0.1) using the same settings as
// LE's "Run in Japanese" profile. Both DLLs must be next to the game EXE:
// LeCreateProcess builds the LocaleEmulator.dll path from the main module.
class LocaleEmulator
{
public:
	// True if this process was started by Relaunch(). Clears the marker so
	// processes started by the game itself are not affected.
	static bool ConsumeRelaunchMarker();

	// Starts a new instance of the game under Locale Emulator. Returns false
	// (and leaves the current process untouched) if the DLLs are missing or
	// the new process could not be created.
	static bool Relaunch();

private:
	typedef struct ML_PROCESS_INFORMATION : PROCESS_INFORMATION
	{
		PVOID FirstCallLdrLoadDll;

	} ML_PROCESS_INFORMATION, *PML_PROCESS_INFORMATION;

	typedef struct _TIME_FIELDS
	{
		SHORT Year;        // range [1601...]
		SHORT Month;       // range [1..12]
		SHORT Day;         // range [1..31]
		SHORT Hour;        // range [0..23]
		SHORT Minute;      // range [0..59]
		SHORT Second;      // range [0..59]
		SHORT Milliseconds;// range [0..999]
		SHORT Weekday;     // range [0..6] == [Sunday..Saturday]
	} TIME_FIELDS, *PTIME_FIELDS;

	typedef struct _RTL_TIME_ZONE_INFORMATION
	{
		LONG        Bias;
		WCHAR       StandardName[32];
		TIME_FIELDS StandardStart;
		LONG        StandardBias;
		WCHAR       DaylightName[32];
		TIME_FIELDS DaylightStart;
		LONG        DaylightBias;
	} RTL_TIME_ZONE_INFORMATION, *PRTL_TIME_ZONE_INFORMATION;

	typedef struct _REG_TZI_FORMAT
	{
		int Bias;
		int StandardBias;
		int DaylightBias;
		_SYSTEMTIME StandardDate;
		_SYSTEMTIME DaylightDate;
	} REG_TZI_FORMAT;

	// Registry redirection entries use 64-bit fields in both 32- and 64-bit
	// builds. Buffer and Data hold byte offsets from the start of the LEB.
	typedef struct
	{
		USHORT  Length;
		USHORT  MaximumLength;
		ULONG64 Buffer;
	} UNICODE_STRING64;

	typedef struct
	{
		ULONG64             Root;
		UNICODE_STRING64    SubKey;
		UNICODE_STRING64    ValueName;
		ULONG               DataType;
		ULONG64             Data;
		ULONG64             DataSize;
	} REGISTRY_ENTRY64;

	typedef struct
	{
		REGISTRY_ENTRY64 Original;
		REGISTRY_ENTRY64 Redirected;
	} REGISTRY_REDIRECTION_ENTRY64;

	// Followed in memory by NumberOfRegistryRedirectionEntries entries and the
	// string data they point to.
	typedef struct
	{
		ULONG                           AnsiCodePage;
		ULONG                           OemCodePage;
		ULONG                           LocaleID;
		ULONG                           DefaultCharset;
		ULONG                           HookUILanguageApi;
		WCHAR                           DefaultFaceName[LF_FACESIZE];
		RTL_TIME_ZONE_INFORMATION       Timezone;
		ULONG64                         NumberOfRegistryRedirectionEntries;
	} LOCALE_ENUMLATOR_ENVIRONMENT_BLOCK, *PLOCALE_ENUMLATOR_ENVIRONMENT_BLOCK, LEB, *PLEB;

	// Offsets used by LoaderDll.dll 2.5.0.1 (entry count at +0x100, entries
	// of 0x80 bytes starting at +0x108).
	static_assert(sizeof(REGISTRY_ENTRY64) == 0x40);
	static_assert(sizeof(REGISTRY_REDIRECTION_ENTRY64) == 0x80);
	static_assert(offsetof(LEB, NumberOfRegistryRedirectionEntries) == 0x100);
	static_assert(sizeof(LEB) == 0x108);

	typedef LONG (WINAPI* LeCreateProcess_t)(
		PLEB                    leb,
		PCWSTR                  applicationName,
		PWSTR                   commandLine,
		PCWSTR                  currentDirectory,
		ULONG                   creationFlags,
		LPSTARTUPINFOW          startupInfo,
		PML_PROCESS_INFORMATION processInformation,
		LPSECURITY_ATTRIBUTES   processAttributes,
		LPSECURITY_ATTRIBUTES   threadAttributes,
		PVOID                   environment,
		HANDLE                  token
	);

	static std::vector<BYTE> CreateLeb();
};
