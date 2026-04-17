#include "save_hook.h"
#include "save_stage.h"
#include "save_widget.h"
#include "plugin_helpers.h"
#include "plugin_config.h"

#include "Chimera_classes.hpp"
#include "miniz.h"

#include <windows.h>
#include <psapi.h>
#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <cstdint>
#include <limits>

// ==========================================================================
// UE5 layout mirrors
// ==========================================================================

struct FTArrayRaw
{
	void*   data;
	int32_t arrayNum;
	int32_t arrayMax;
};

struct FStringRaw  { FTArrayRaw chars; };
struct FDateTimeRaw { int64_t ticks; };   // FDateTime is just a Ticks int64

// FCrSaveGameData lives at UCrSaveSubsystem+0x30, size 0xC0.
static constexpr size_t    kSaveDataSize   = 0xC0;
static constexpr ptrdiff_t kSaveDataOffset = 0x30;

// ==========================================================================
// UCrSaveSubsystem field offsets  (all relative to the 'this' pointer)
// Derived from IDA assembly listing of SaveGameInternal @ 0x1475B9D60.
// ==========================================================================

// Delegate fields
static constexpr ptrdiff_t kOnPreSaveStartOffset = 0x110; // lea rcx,[rsi+110h]
static constexpr ptrdiff_t kOnAfterSaveOffset    = 0x188; // lea rcx,[rsi+188h]

// GetWorld() vtable slot — call qword ptr [rax+180h]
static constexpr size_t kGetWorldVtableSlot = 0x180 / 8;  // = 0x30

// SaveData field offsets within UCrSaveSubsystem (= SaveData-relative + 0x30)
static constexpr ptrdiff_t kSaveDataLevelOffset         = 0x80;  // FString Level
static constexpr ptrdiff_t kSaveDataWorldTimeOffset     = 0x90;  // double WorldTimeSeconds
static constexpr ptrdiff_t kSaveDataWorldUnpausedOffset = 0x98;  // double WorldUnpausedTimeSeconds
static constexpr ptrdiff_t kSaveDataWorldRealOffset     = 0xA0;  // double WorldRealTimeSeconds
static constexpr ptrdiff_t kSaveDataWorldAudioOffset    = 0xA8;  // double WorldAudioTimeSeconds
static constexpr ptrdiff_t kSaveDataTimeStampOffset     = 0xD0;  // FString TimeStamp
static constexpr ptrdiff_t kSaveDataGameVersionOffset   = 0xE0;  // FCrGameVersion (12 bytes)
static constexpr size_t    kGameVersionSize             = 12;    // Major(4)+Minor(4)+Patch(4)

// UWorld field offsets
static constexpr ptrdiff_t kWorldTimeSecondsOffset         = 0x838;
static constexpr ptrdiff_t kWorldUnpausedTimeSecondsOffset = 0x840;
static constexpr ptrdiff_t kWorldRealTimeSecondsOffset     = 0x848;
static constexpr ptrdiff_t kWorldAudioTimeSecondsOffset    = 0x850;

// Offsets of specific CALL / LEA instructions within SaveGameInternal body
// (instruction address - function base 0x1475B9D60)
static constexpr size_t kSGI_Off_FMemoryFree      = 0x00BE; // first FMemory::Free call
static constexpr size_t kSGI_Off_BroadcastPre     = 0x012C; // FOnFlashlightDeactivated_DelegateWrapper (OnPreSaveStart)
static constexpr size_t kSGI_Off_GetPathName      = 0x0148; // UObjectBaseUtility::GetPathName
static constexpr size_t kSGI_Off_AssignRange      = 0x0167; // FString::AssignRange
static constexpr size_t kSGI_Off_DateTimeNow      = 0x01D8; // FDateTime::Now
static constexpr size_t kSGI_Off_DateTimeToString = 0x01EC; // FDateTime::ToString
static constexpr size_t kSGI_Off_StaticStruct     = 0x0272; // FCrSaveGameData::StaticStruct()
static constexpr size_t kSGI_Off_UStructToJson    = 0x02A0; // FJsonObjectConverter::UStructToJsonObjectString
static constexpr size_t kSGI_Off_WriteMetaFile    = 0x05D2; // UCrSaveGameUtils::WriteMetaFile
static constexpr size_t kSGI_Off_SaveLastName     = 0x0642; // UCrSaveGameUtils::SaveLastSaveGameName
static constexpr size_t kSGI_Off_GameVersionLea   = 0x05AF; // LEA RCX, GameVersion::CurrentGameVersion_0

// ==========================================================================
// Function pointer typedefs
// ==========================================================================

typedef void(__fastcall* SaveGameInternal_t)(void* self, FStringRaw* Name, bool bAsync);

// FJsonObjectConverter::UStructToJsonObjectString
typedef bool(__fastcall* UStructToJsonString_t)(
	void* ScriptStruct, const void* StructPtr,
	FStringRaw* OutJson,
	int64_t CheckFlags, int64_t SkipFlags,
	void* ExportCb, void* Unk, bool bPretty);

// FCrSaveGameData::StaticStruct()
typedef void*(__fastcall* StaticStruct_t)();

// FMemory::Free
typedef void(__fastcall* FMemoryFree_t)(void* ptr);

// FOnFlashlightDeactivated_DelegateWrapper — broadcasts a multicast delegate
typedef void(__fastcall* BroadcastDelegate_t)(void* delegateField);

// FDateTime::Now  (static, hidden-return-ptr convention: RCX = out FDateTimeRaw*)
typedef void(__fastcall* FDateTimeNow_t)(FDateTimeRaw* outResult);

// FDateTime::ToString  (RCX=this, RDX=outFString, R8=fmt)
typedef FStringRaw*(__fastcall* FDateTimeToString_t)(FDateTimeRaw* self, FStringRaw* outResult, const wchar_t* fmt);

// UObjectBaseUtility::GetPathName  (RCX=obj, RDX=outFString, R8=StopOuter)
typedef void(__fastcall* GetPathName_t)(void* obj, FStringRaw* outResult, void* stopOuter);

// FString::AssignRange  (RCX=this, RDX=data, R8=len)
typedef void(__fastcall* FStringAssignRange_t)(FStringRaw* self, const wchar_t* data, int32_t len);

// UCrSaveGameUtils::SaveLastSaveGameName  (RCX=FString&)
typedef void(__fastcall* SaveLastSaveGameName_t)(FStringRaw* name);

// UCrSaveGameUtils::WriteMetaFile
// (XMM0=worldTime, RDX=levelName, R8=saveName, R9=bIsInTutorial, stack=gameVersion, stack=world)
typedef void(__fastcall* WriteMetaFile_t)(
	double worldTime,
	FStringRaw* levelName,
	FStringRaw* saveName,
	char bIsInTutorial,
	const void* gameVersion,
	void* world);

// ISteamRemoteStorage — vtable confirmed from WriteUserFile assembly:
//   slot 0  [rax+0x00] = FileWrite      (sync,  returns bool)
//   slot 1  [rax+0x08] = FileRead
//   slot 2  [rax+0x10] = FileWriteAsync (async, returns SteamAPICall_t)
struct ISteamRemoteStorage_Vtbl
{
	bool(__fastcall* FileWrite)(void* self, const char* file, const void* data, int32_t size);
	void*            FileRead;
	void*            FileWriteAsync;
};
struct ISteamRemoteStorage_Minimal { ISteamRemoteStorage_Vtbl* vtable; };

// SteamInternal_ContextInit — exported from steam_api64.dll.
// The game's inline SteamRemoteStorage() thunk uses this with its own
// s_CallbackCounterAndContext static; NOT a flat "SteamRemoteStorage" export.
// Signature: void* SteamInternal_ContextInit(void* pContext)
// Returns a pointer; dereference once to get ISteamRemoteStorage*.
typedef void*(__cdecl* SteamContextInit_t)(void*);

// ==========================================================================
// Static instances
// ==========================================================================

static HookHandle            g_hookHandle          = nullptr;
static SaveGameInternal_t    g_originalSGI         = nullptr;
static std::atomic<bool>     g_saveInProgress      { false };

static UStructToJsonString_t g_uStructToJson          = nullptr;
static StaticStruct_t        g_crSaveDataStaticStruct  = nullptr;
static FStringRaw*           g_cloudSaveFolder         = nullptr;

// Steam — resolved lazily at save time via SteamInternal_ContextInit
static SteamContextInit_t    g_steamContextInit        = nullptr;
static uintptr_t             g_steamContextAddr        = 0;    // s_CallbackCounterAndContext

static FMemoryFree_t         g_fMemoryFree         = nullptr;
static BroadcastDelegate_t   g_broadcastDelegate   = nullptr;
static FDateTimeNow_t        g_dateTimeNow         = nullptr;
static FDateTimeToString_t   g_dateTimeToString    = nullptr;
static GetPathName_t         g_getPathName         = nullptr;
static FStringAssignRange_t  g_fStringAssignRange  = nullptr;
static SaveLastSaveGameName_t g_saveLastSaveGameName = nullptr;
static WriteMetaFile_t       g_writeMetaFile       = nullptr;
static uintptr_t             g_gameVersionAddr     = 0;

// ==========================================================================
// Save task
// ==========================================================================

struct SaveTask
{
	uint8_t      saveDataRaw[kSaveDataSize]; // snapshot of FCrSaveGameData (post-metadata update)
	std::wstring slotName;
	void*        subsystemSelf;              // for OnAfterSave (game thread marshal TODO)
};

// ==========================================================================
// Steam lazy accessor
// ==========================================================================

// Called at save time (background thread).  SteamInternal_ContextInit handles
// lazy init, so this is safe to call even if we init before Steam is ready.
static ISteamRemoteStorage_Minimal* GetSteamStorage()
{
	if (!g_steamContextInit || !g_steamContextAddr) return nullptr;
	void* result = g_steamContextInit(reinterpret_cast<void*>(g_steamContextAddr));
	if (!result) return nullptr;
	return *reinterpret_cast<ISteamRemoteStorage_Minimal**>(result);
}

// ==========================================================================
// UTF-8 helper
// ==========================================================================

static std::string WideToUtf8(const wchar_t* str, int len)
{
	if (!str || len <= 0) return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, str, len, nullptr, 0, nullptr, nullptr);
	if (n <= 0) return {};
	std::string out(static_cast<size_t>(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, str, len, out.data(), n, nullptr, nullptr);
	return out;
}

// Safe wrapper: free a UE-allocated buffer via FMemory::Free if available,
// else fall back to HeapFree (valid for UE5 Shipping on Windows).
static void UEFree(void* ptr)
{
	if (!ptr) return;
	if (g_fMemoryFree)
		g_fMemoryFree(ptr);
	else
		HeapFree(GetProcessHeap(), 0, ptr);
}

// ==========================================================================
// Background thread — JSON → UTF-8 → compress → Steam write
// ==========================================================================

static void RunSaveTask(SaveTask task)
{
	LOG_INFO("BetterSaving: [1/4] Starting save — slot='%ls'", task.slotName.c_str());

	bool jsonSucceeded = false;

	// ------------------------------------------------------------------
	// Step 1: JSON serialisation
	// ------------------------------------------------------------------

	SaveWidget::SetStage(SaveStage::Compressing);

	LOG_INFO("BetterSaving: [1/4] Fetching FCrSaveGameData::StaticStruct...");
	void* scriptStruct = g_crSaveDataStaticStruct();
	if (!scriptStruct)
	{
		LOG_ERROR("BetterSaving: [1/4] FAILED — FCrSaveGameData::StaticStruct() returned null");
		goto Fail;
	}
	LOG_INFO("BetterSaving: [1/4] StaticStruct OK (0x%p)", scriptStruct);

	{
		LOG_INFO("BetterSaving: [1/4] Serialising save data to JSON...");
		FStringRaw jsonStr{};
		const bool jsonOk = g_uStructToJson(
			scriptStruct,
			task.saveDataRaw,
			&jsonStr,
			0x1000000,  // CPF_SaveGame
			0x2000,     // CPF_Transient
			nullptr, nullptr,
			false);

		if (!jsonOk || !jsonStr.chars.data || jsonStr.chars.arrayNum <= 1)
		{
			LOG_ERROR("BetterSaving: [1/4] FAILED — UStructToJsonObjectString returned ok=%d arrayNum=%d",
				static_cast<int>(jsonOk), jsonStr.chars.arrayNum);
			UEFree(jsonStr.chars.data);
			goto FailNoPost;
		}
		LOG_INFO("BetterSaving: [1/4] JSON serialisation OK — %d wchars", jsonStr.chars.arrayNum - 1);

		const int wideLen = jsonStr.chars.arrayNum - 1;
		const std::string jsonUtf8 = WideToUtf8(
			static_cast<const wchar_t*>(jsonStr.chars.data), wideLen);
		UEFree(jsonStr.chars.data);

		if (jsonUtf8.empty())
		{
			LOG_ERROR("BetterSaving: [1/4] FAILED — UTF-8 conversion produced empty string");
			goto FailNoPost;
		}
		LOG_INFO("BetterSaving: [1/4] UTF-8 conversion OK — %zu bytes", jsonUtf8.size());

		// Sanity-check: valid JSON must start with '{'
		if (jsonUtf8[0] != '{')
		{
			LOG_ERROR("BetterSaving: [1/4] FAILED — JSON sanity check: first byte is 0x%02X ('%c'), expected '{'. Aborting to avoid corrupt save.",
				static_cast<unsigned char>(jsonUtf8[0]),
				jsonUtf8[0] >= 0x20 ? jsonUtf8[0] : '?');
			goto FailNoPost;
		}
		LOG_INFO("BetterSaving: [1/4] JSON sanity check OK — first='%c' last='%c'",
			jsonUtf8.front(),
			jsonUtf8.back() >= 0x20 ? jsonUtf8.back() : '?');

		jsonSucceeded = true;

		// ------------------------------------------------------------------
		// Step 2: Compress (zlib via miniz)
		// ------------------------------------------------------------------

		LOG_INFO("BetterSaving: [2/4] Compressing %zu bytes...", jsonUtf8.size());

		const int32_t uncompSize = static_cast<int32_t>(jsonUtf8.size());
		mz_ulong compressedSize  = mz_compressBound(static_cast<mz_ulong>(jsonUtf8.size()));
		std::vector<uint8_t> compressed(static_cast<size_t>(compressedSize));

		const int mzResult = mz_compress2(
			compressed.data(), &compressedSize,
			reinterpret_cast<const unsigned char*>(jsonUtf8.data()),
			static_cast<mz_ulong>(jsonUtf8.size()),
			MZ_DEFAULT_COMPRESSION);

		if (mzResult != MZ_OK)
		{
			LOG_ERROR("BetterSaving: [2/4] FAILED — mz_compress2 returned %d", mzResult);
			goto Fail;
		}

		// Sanity-check: zlib streams always start with 0x78
		if (compressedSize < 2 || compressed[0] != 0x78)
		{
			LOG_ERROR("BetterSaving: [2/4] FAILED — zlib header sanity check: expected 0x78, got 0x%02X (compressedSize=%lu)",
				compressed[0], compressedSize);
			goto Fail;
		}
		LOG_INFO("BetterSaving: [2/4] Compression OK — %d → %lu bytes (%.2fx) zlib header=0x%02X%02X",
			uncompSize, compressedSize,
			static_cast<double>(uncompSize) / static_cast<double>(compressedSize),
			compressed[0], compressed[1]);

		// Payload: 4-byte LE uncompressed size + compressed data
		std::vector<uint8_t> payload(4 + static_cast<size_t>(compressedSize));
		payload[0] = static_cast<uint8_t>(uncompSize         & 0xFF);
		payload[1] = static_cast<uint8_t>((uncompSize >>  8) & 0xFF);
		payload[2] = static_cast<uint8_t>((uncompSize >> 16) & 0xFF);
		payload[3] = static_cast<uint8_t>((uncompSize >> 24) & 0xFF);
		memcpy(payload.data() + 4, compressed.data(), static_cast<size_t>(compressedSize));

		LOG_INFO("BetterSaving: [2/4] Payload built — %zu bytes total (4-byte header + %lu compressed)",
			payload.size(), compressedSize);

		// ------------------------------------------------------------------
		// Step 3: Steam write
		// ------------------------------------------------------------------

		SaveWidget::SetStage(SaveStage::Writing);

		LOG_INFO("BetterSaving: [3/4] Checking CloudSaveFolder...");
		if (!g_cloudSaveFolder || !g_cloudSaveFolder->chars.data || g_cloudSaveFolder->chars.arrayNum <= 1)
		{
			LOG_ERROR("BetterSaving: [3/4] FAILED — CloudSaveFolder not readable");
			goto Fail;
		}

		{
			LOG_INFO("BetterSaving: [3/4] Acquiring ISteamRemoteStorage...");
			ISteamRemoteStorage_Minimal* steam = GetSteamStorage();
			if (!steam)
			{
				LOG_ERROR("BetterSaving: [3/4] FAILED — ISteamRemoteStorage not available");
				goto Fail;
			}
			LOG_INFO("BetterSaving: [3/4] ISteamRemoteStorage OK (0x%p)", static_cast<void*>(steam));

			const int folderLen = g_cloudSaveFolder->chars.arrayNum - 1;
			const std::string folderUtf8 = WideToUtf8(
				static_cast<const wchar_t*>(g_cloudSaveFolder->chars.data), folderLen);
			const std::string slotUtf8 = WideToUtf8(task.slotName.c_str(),
				static_cast<int>(task.slotName.size()));
			const std::string steamPath = folderUtf8 + slotUtf8 + ".sav";

			if (payload.size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)()))
			{
				LOG_ERROR("BetterSaving: [3/4] FAILED — Payload too large (%zu bytes)", payload.size());
				goto Fail;
			}

			LOG_INFO("BetterSaving: [3/4] FileWrite → '%s' (%zu bytes)...", steamPath.c_str(), payload.size());
			const bool writeOk = steam->vtable->FileWrite(steam, steamPath.c_str(),
				payload.data(), static_cast<int32_t>(payload.size()));

			if (!writeOk)
			{
				LOG_ERROR("BetterSaving: [3/4] FAILED — ISteamRemoteStorage::FileWrite returned false for '%s' (%zu bytes). Steam quota or file count limit may have been reached.",
					steamPath.c_str(), payload.size());
				goto Fail;
			}

			LOG_INFO("BetterSaving: [3/4] FileWrite OK — '%s' (%zu bytes written)", steamPath.c_str(), payload.size());
		}
	}

	// ------------------------------------------------------------------
	// Post-save: SaveLastSaveGameName
	// (OnAfterSave skipped — FNotThreadSafeDelegateMode; TODO: game-thread marshal)
	// ------------------------------------------------------------------
	LOG_INFO("BetterSaving: [4/4] Calling SaveLastSaveGameName — slot='%ls'", task.slotName.c_str());
	if (g_saveLastSaveGameName && !task.slotName.empty())
	{
		const size_t wlen = task.slotName.size();
		void* buf = HeapAlloc(GetProcessHeap(), 0, (wlen + 1) * sizeof(wchar_t));
		if (buf)
		{
			memcpy(buf, task.slotName.c_str(), (wlen + 1) * sizeof(wchar_t));
			FStringRaw nameStr{};
			nameStr.chars.data     = buf;
			nameStr.chars.arrayNum = static_cast<int32_t>(wlen + 1);
			nameStr.chars.arrayMax = static_cast<int32_t>(wlen + 1);
			g_saveLastSaveGameName(&nameStr);
			if (nameStr.chars.data) HeapFree(GetProcessHeap(), 0, nameStr.chars.data);
			LOG_INFO("BetterSaving: [4/4] SaveLastSaveGameName OK");
		}
		else
		{
			LOG_WARN("BetterSaving: [4/4] HeapAlloc failed — SaveLastSaveGameName skipped");
		}
	}
	else
	{
		LOG_WARN("BetterSaving: [4/4] SaveLastSaveGameName skipped — fn=0x%p slotEmpty=%d",
			reinterpret_cast<void*>(g_saveLastSaveGameName),
			static_cast<int>(task.slotName.empty()));
	}

	LOG_INFO("BetterSaving: Save complete — slot='%ls'", task.slotName.c_str());
	SaveWidget::SetStage(SaveStage::Done);
	Sleep(2000);
	SaveWidget::SetStage(SaveStage::Idle);
	g_saveInProgress.store(false, std::memory_order_release);
	return;

Fail:
	LOG_ERROR("BetterSaving: Save FAILED — slot='%ls'", task.slotName.c_str());
	SaveWidget::SetStage(SaveStage::Failed);
	Sleep(2000);
	SaveWidget::SetStage(SaveStage::Idle);
FailNoPost:
	g_saveInProgress.store(false, std::memory_order_release);
	LOG_ERROR("BetterSaving: Save aborted (no post-save) — slot='%ls'", task.slotName.c_str());
}

// ==========================================================================
// Detour
// ==========================================================================

static void __fastcall Detour_SaveGameInternal(void* self, FStringRaw* Name, bool bAsync)
{
	LOG_TRACE("BetterSaving: Detour_SaveGameInternal — self=0x%p bAsync=%d", self, static_cast<int>(bAsync));

	// If a save is already in progress, drop this request immediately.
	// Do NOT fall back to g_originalSGI — it writes raw uncompressed JSON
	// which the game can no longer decompress. Do NOT block the game thread.
	bool expected = false;
	if (!g_saveInProgress.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
	{
		LOG_WARN("BetterSaving: Save already in progress — dropping duplicate request for slot='%ls'",
			(Name && Name->chars.data && Name->chars.arrayNum > 1)
				? static_cast<const wchar_t*>(Name->chars.data)
				: L"(unknown)");
		return;
	}

	uint8_t* const selfBytes = reinterpret_cast<uint8_t*>(self);

	// ------------------------------------------------------------------
	// Game-thread phase: update SaveData metadata fields
	// These mirror exactly what SaveGameInternal does before its JSON call.
	// ------------------------------------------------------------------

	// 1. Fire OnPreSaveStart delegate
	if (g_broadcastDelegate)
		g_broadcastDelegate(selfBytes + kOnPreSaveStartOffset);

	// 2. GetWorld via vtable slot 0x30
	void** const vtable = *reinterpret_cast<void***>(self);
	typedef void*(__fastcall* GetWorld_t)(void*);
	const GetWorld_t getWorldFn = reinterpret_cast<GetWorld_t>(vtable[kGetWorldVtableSlot]);
	void* const world = getWorldFn(self);

	if (!world)
	{
		LOG_ERROR("BetterSaving: GetWorld() returned null — aborting save");
		g_saveInProgress.store(false, std::memory_order_release);
		return;
	}

	const uint8_t* const worldBytes = reinterpret_cast<const uint8_t*>(world);

	// 3. Update SaveData.Level from current level path
	if (g_getPathName && g_fStringAssignRange)
	{
		FStringRaw levelName{};
		g_getPathName(world, &levelName, nullptr);

		const int32_t len = (levelName.chars.arrayNum > 0) ? (levelName.chars.arrayNum - 1) : 0;
		g_fStringAssignRange(
			reinterpret_cast<FStringRaw*>(selfBytes + kSaveDataLevelOffset),
			static_cast<const wchar_t*>(levelName.chars.data), len);

		UEFree(levelName.chars.data);
	}

	// 4. Update world time fields
	*reinterpret_cast<double*>(selfBytes + kSaveDataWorldTimeOffset) =
		*reinterpret_cast<const double*>(worldBytes + kWorldTimeSecondsOffset);
	*reinterpret_cast<double*>(selfBytes + kSaveDataWorldUnpausedOffset) =
		*reinterpret_cast<const double*>(worldBytes + kWorldUnpausedTimeSecondsOffset);
	*reinterpret_cast<double*>(selfBytes + kSaveDataWorldRealOffset) =
		*reinterpret_cast<const double*>(worldBytes + kWorldRealTimeSecondsOffset);
	*reinterpret_cast<double*>(selfBytes + kSaveDataWorldAudioOffset) =
		*reinterpret_cast<const double*>(worldBytes + kWorldAudioTimeSecondsOffset);

	// 5. Update TimeStamp via FDateTime::Now + ToString
	if (g_dateTimeNow && g_dateTimeToString && g_fStringAssignRange)
	{
		FDateTimeRaw now{};
		g_dateTimeNow(&now);

		FStringRaw tsResult{};
		g_dateTimeToString(&now, &tsResult, L"%Y%m%d%H%M%S");

		const int32_t tsLen = (tsResult.chars.arrayNum > 0) ? (tsResult.chars.arrayNum - 1) : 0;
		g_fStringAssignRange(
			reinterpret_cast<FStringRaw*>(selfBytes + kSaveDataTimeStampOffset),
			static_cast<const wchar_t*>(tsResult.chars.data), tsLen);

		UEFree(tsResult.chars.data);
	}

	// 6. Update GameVersion from static
	if (g_gameVersionAddr)
	{
		memcpy(selfBytes + kSaveDataGameVersionOffset,
			reinterpret_cast<const void*>(g_gameVersionAddr),
			kGameVersionSize);
	}

	// 7. Write meta file (game thread — mirrors what SaveGameInternal does before JSON)
	if (g_writeMetaFile)
	{
		const double worldTime = *reinterpret_cast<const double*>(worldBytes + kWorldTimeSecondsOffset);

		// Get ACrGameStateBase from UWorld::GameState (offset 0x1B0) and query tutorial state
		static constexpr ptrdiff_t kWorldGameStateOffset = 0x1B0;
		auto* gameState = *reinterpret_cast<SDK::ACrGameStateBase* const*>(worldBytes + kWorldGameStateOffset);
		const char bIsInTutorial = (gameState && gameState->IsInTutorial()) ? 1 : 0;

		g_writeMetaFile(
			worldTime,
			reinterpret_cast<FStringRaw*>(selfBytes + kSaveDataTimeStampOffset),
			Name,
			bIsInTutorial,
			reinterpret_cast<const void*>(g_gameVersionAddr),
			world);

		LOG_DEBUG("BetterSaving: WriteMetaFile called (tutorial=%d)", static_cast<int>(bIsInTutorial));
	}

	// 8. Snapshot SaveData — background thread takes it from here
	SaveTask task{};
	memcpy(task.saveDataRaw, selfBytes + kSaveDataOffset, kSaveDataSize);
	task.subsystemSelf = self;

	if (Name && Name->chars.data && Name->chars.arrayNum > 1)
	{
		task.slotName = std::wstring(
			static_cast<const wchar_t*>(Name->chars.data),
			static_cast<size_t>(Name->chars.arrayNum - 1));
	}
	else
	{
		task.slotName = L"UnknownSave";
	}

	LOG_INFO("BetterSaving: Metadata updated — spawning save thread for slot='%ls'", task.slotName.c_str());
	SaveWidget::SetStage(SaveStage::Queued);

	std::thread(RunSaveTask, std::move(task)).detach();
	// g_saveInProgress cleared by RunSaveTask
}

// ==========================================================================
// World-end guard
// ==========================================================================

static void OnBeforeWorldEndPlay(SDK::UWorld* /*world*/, const char* worldName)
{
	if (!g_saveInProgress.load(std::memory_order_acquire)) return;

	LOG_INFO("BetterSaving: World '%s' ending — waiting for in-flight save...",
		worldName ? worldName : "(null)");

	constexpr int kMaxWaitMs = 5000;
	constexpr int kSleepMs   = 10;
	int waited = 0;
	while (waited < kMaxWaitMs && g_saveInProgress.load(std::memory_order_acquire))
	{
		Sleep(kSleepMs);
		waited += kSleepMs;
	}

	if (g_saveInProgress.load(std::memory_order_acquire))
		LOG_WARN("BetterSaving: Timed out after %d ms — proceeding anyway", waited);
	else
		LOG_INFO("BetterSaving: Save finished after %d ms", waited);
}

// ==========================================================================
// Initialisation helpers
// ==========================================================================

// Read a near-CALL target (E8 rel32) at byte offset `off` from `base`.
static uintptr_t ReadCallTarget(uintptr_t base, size_t off)
{
	const uint8_t* p = reinterpret_cast<const uint8_t*>(base + off);
	if (*p != 0xE8)
	{
		LOG_ERROR("BetterSaving: Expected E8 at SGI+0x%zX, got 0x%02X", off, *p);
		return 0;
	}
	const int32_t rel = *reinterpret_cast<const int32_t*>(p + 1);
	return base + off + 5 + static_cast<int64_t>(rel);
}

// Read a LEA RCX,[RIP+rel32] target (48 8D 0D xx xx xx xx) at byte offset `off`.
static uintptr_t ReadLeaRcxTarget(uintptr_t base, size_t off)
{
	const uint8_t* p = reinterpret_cast<const uint8_t*>(base + off);
	if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x0D)
	{
		LOG_ERROR("BetterSaving: Expected LEA RCX at SGI+0x%zX, got %02X %02X %02X",
			off, p[0], p[1], p[2]);
		return 0;
	}
	const int32_t rel = *reinterpret_cast<const int32_t*>(p + 3);
	return base + off + 7 + static_cast<int64_t>(rel);
}

static void InitFunctionPointers(uintptr_t sgiBase)
{
	const uintptr_t fmFree = ReadCallTarget(sgiBase, kSGI_Off_FMemoryFree);
	g_fMemoryFree = reinterpret_cast<FMemoryFree_t>(fmFree);
	LOG_INFO("BetterSaving: FMemory::Free         at 0x%llX", static_cast<unsigned long long>(fmFree));

	const uintptr_t broadcast = ReadCallTarget(sgiBase, kSGI_Off_BroadcastPre);
	g_broadcastDelegate = reinterpret_cast<BroadcastDelegate_t>(broadcast);
	LOG_INFO("BetterSaving: BroadcastDelegate     at 0x%llX", static_cast<unsigned long long>(broadcast));

	const uintptr_t pathName = ReadCallTarget(sgiBase, kSGI_Off_GetPathName);
	g_getPathName = reinterpret_cast<GetPathName_t>(pathName);
	LOG_INFO("BetterSaving: GetPathName           at 0x%llX", static_cast<unsigned long long>(pathName));

	const uintptr_t assignRange = ReadCallTarget(sgiBase, kSGI_Off_AssignRange);
	g_fStringAssignRange = reinterpret_cast<FStringAssignRange_t>(assignRange);
	LOG_INFO("BetterSaving: FString::AssignRange  at 0x%llX", static_cast<unsigned long long>(assignRange));

	const uintptr_t dtNow = ReadCallTarget(sgiBase, kSGI_Off_DateTimeNow);
	g_dateTimeNow = reinterpret_cast<FDateTimeNow_t>(dtNow);
	LOG_INFO("BetterSaving: FDateTime::Now        at 0x%llX", static_cast<unsigned long long>(dtNow));

	const uintptr_t dtToStr = ReadCallTarget(sgiBase, kSGI_Off_DateTimeToString);
	g_dateTimeToString = reinterpret_cast<FDateTimeToString_t>(dtToStr);
	LOG_INFO("BetterSaving: FDateTime::ToString   at 0x%llX", static_cast<unsigned long long>(dtToStr));

	const uintptr_t staticStruct = ReadCallTarget(sgiBase, kSGI_Off_StaticStruct);
	g_crSaveDataStaticStruct = reinterpret_cast<StaticStruct_t>(staticStruct);
	LOG_INFO("BetterSaving: FCrSaveGameData::StaticStruct at 0x%llX", static_cast<unsigned long long>(staticStruct));

	const uintptr_t uStructJson = ReadCallTarget(sgiBase, kSGI_Off_UStructToJson);
	g_uStructToJson = reinterpret_cast<UStructToJsonString_t>(uStructJson);
	LOG_INFO("BetterSaving: UStructToJsonObjectString     at 0x%llX", static_cast<unsigned long long>(uStructJson));

	const uintptr_t saveLastName = ReadCallTarget(sgiBase, kSGI_Off_SaveLastName);
	g_saveLastSaveGameName = reinterpret_cast<SaveLastSaveGameName_t>(saveLastName);
	LOG_INFO("BetterSaving: SaveLastSaveGameName  at 0x%llX", static_cast<unsigned long long>(saveLastName));

	const uintptr_t writeMetaFile = ReadCallTarget(sgiBase, kSGI_Off_WriteMetaFile);
	g_writeMetaFile = reinterpret_cast<WriteMetaFile_t>(writeMetaFile);
	LOG_INFO("BetterSaving: WriteMetaFile         at 0x%llX", static_cast<unsigned long long>(writeMetaFile));

	g_gameVersionAddr = ReadLeaRcxTarget(sgiBase, kSGI_Off_GameVersionLea);
	LOG_INFO("BetterSaving: GameVersion static    at 0x%llX", static_cast<unsigned long long>(g_gameVersionAddr));
}

// WUF body offset of the LEA RCX that loads s_CallbackCounterAndContext
// (0x14774DD0D - 0x14774D710 = 0x5FD)
static constexpr size_t kWUF_Off_SteamContext = 0x5FD;

static void InitSteam(uintptr_t wufAddr)
{
	const HMODULE hSteam = GetModuleHandleW(L"steam_api64.dll");
	if (!hSteam) { LOG_WARN("BetterSaving: steam_api64.dll not loaded"); return; }

	// The game compiles SteamRemoteStorage() as an inline thunk using
	// SteamInternal_ContextInit — there is no flat "SteamRemoteStorage" export.
	const auto ctxInit = reinterpret_cast<SteamContextInit_t>(
		GetProcAddress(hSteam, "SteamInternal_ContextInit"));
	if (!ctxInit) { LOG_WARN("BetterSaving: SteamInternal_ContextInit not found in steam_api64.dll"); return; }

	// Read the s_CallbackCounterAndContext address from WriteUserFile body
	const uintptr_t ctxAddr = ReadLeaRcxTarget(wufAddr, kWUF_Off_SteamContext);
	if (!ctxAddr) { LOG_WARN("BetterSaving: Failed to locate SteamContext LEA in WriteUserFile"); return; }

	g_steamContextInit = ctxInit;
	g_steamContextAddr = ctxAddr;

	LOG_INFO("BetterSaving: SteamInternal_ContextInit at 0x%p  context at 0x%llX",
		reinterpret_cast<void*>(ctxInit), static_cast<unsigned long long>(ctxAddr));
	LOG_INFO("BetterSaving: ISteamRemoteStorage will be resolved lazily at save time");
}

static void InitCloudSaveFolder(uintptr_t writeUserFileAddr)
{
	HMODULE hMain = GetModuleHandle(nullptr);
	if (!hMain) return;

	MODULEINFO mi{};
	GetModuleInformation(GetCurrentProcess(), hMain, &mi, sizeof(mi));
	const uintptr_t modStart = reinterpret_cast<uintptr_t>(hMain);
	const uintptr_t modEnd   = modStart + mi.SizeOfImage;

	const uint8_t* code = reinterpret_cast<const uint8_t*>(writeUserFileAddr);
	constexpr size_t kScan = 6000;

	auto* self = GetSelf();
	if (!self) return;

	static const char* kFPathsPattern = "4C 89 74 24 ?? 55 48 8B EC 48 83 EC ?? E8";
	const uintptr_t fPathsAddr = self->scanner->FindPatternInMainModule(kFPathsPattern);
	if (!fPathsAddr) { LOG_WARN("BetterSaving: FPaths::ProjectSavedDir not found"); return; }

	size_t fPathsOffset = 0;
	for (size_t i = 0; i < kScan - 5; ++i)
	{
		if (code[i] != 0xE8) continue;
		const int32_t rel = *reinterpret_cast<const int32_t*>(code + i + 1);
		if (writeUserFileAddr + i + 5 + static_cast<int64_t>(rel) == fPathsAddr)
		{
			fPathsOffset = i; break;
		}
	}

	if (!fPathsOffset) { LOG_WARN("BetterSaving: FPaths CALL not found in WriteUserFile"); return; }

	for (size_t i = 0; i + 7 < fPathsOffset; ++i)
	{
		if (code[i] != 0x48 || code[i + 1] != 0x8D || code[i + 2] != 0x15) continue;
		const int32_t   rel  = *reinterpret_cast<const int32_t*>(code + i + 3);
		const uintptr_t addr = writeUserFileAddr + i + 7 + static_cast<int64_t>(rel);
		if (addr < modStart || addr + 16 > modEnd) continue;

		const int32_t arrayNum = *reinterpret_cast<const int32_t*>(addr + 8);
		const int32_t arrayMax = *reinterpret_cast<const int32_t*>(addr + 12);
		if (arrayNum <= 1 || arrayNum > 512 || arrayMax < arrayNum) continue;

		bool hasCall = false;
		for (size_t j = i + 7; j < i + 37 && j + 5 < kScan; ++j)
			if (code[j] == 0xE8) { hasCall = true; break; }
		if (!hasCall) continue;

		g_cloudSaveFolder = reinterpret_cast<FStringRaw*>(addr);
		LOG_INFO("BetterSaving: CloudSaveFolder at 0x%llX (arrayNum=%d)",
			static_cast<unsigned long long>(addr), arrayNum);
		break;
	}

	if (!g_cloudSaveFolder) LOG_WARN("BetterSaving: CloudSaveFolder not found");
}

// ==========================================================================
// Public API
// ==========================================================================

namespace SaveHook
{

	bool Initialize()
	{
		LOG_DEBUG("BetterSaving: SaveHook::Initialize");

		auto* self = GetSelf();
		if (!self) { LOG_ERROR("BetterSaving: GetSelf() null"); return false; }

		// Locate WriteUserFile for CloudSaveFolder + Steam context
		static const char* kWritePattern =
			"48 89 5C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 "
			"48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? "
			"48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? "
			"65 48 8B 04 25 ?? ?? ?? ?? 33 F6";

		const uintptr_t wufAddr = self->scanner->FindPatternInMainModule(kWritePattern);
		if (!wufAddr) { LOG_ERROR("BetterSaving: WriteUserFile not found"); return false; }
		LOG_INFO("BetterSaving: WriteUserFile at 0x%llX", static_cast<unsigned long long>(wufAddr));

		InitCloudSaveFolder(wufAddr);
		if (!g_cloudSaveFolder) { LOG_ERROR("BetterSaving: CloudSaveFolder required"); return false; }

		InitSteam(wufAddr);
		if (!g_steamContextInit || !g_steamContextAddr)
		{
			LOG_ERROR("BetterSaving: Steam context not resolved — cannot write saves");
			return false;
		}

		// Locate SaveGameInternal and hook it
		static const char* kSGIPattern =
			"48 89 5C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 "
			"48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? "
			"48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? "
			"33 DB 44 88 44 24";

		const uintptr_t sgiAddr = self->scanner->FindPatternInMainModule(kSGIPattern);
		if (!sgiAddr) { LOG_ERROR("BetterSaving: SaveGameInternal not found"); return false; }
		LOG_INFO("BetterSaving: SaveGameInternal at 0x%llX", static_cast<unsigned long long>(sgiAddr));

		// Resolve all function pointers from the SGI body before hooking
		InitFunctionPointers(sgiAddr);

		if (!g_uStructToJson || !g_crSaveDataStaticStruct || !g_writeMetaFile)
		{
			LOG_ERROR("BetterSaving: JSON helpers or WriteMetaFile not resolved from SGI body");
			return false;
		}

		g_hookHandle = self->hooks->Hooks->Install(
			sgiAddr,
			reinterpret_cast<void*>(Detour_SaveGameInternal),
			reinterpret_cast<void**>(&g_originalSGI));

		if (!g_hookHandle) { LOG_ERROR("BetterSaving: Hook install failed"); return false; }
		LOG_INFO("BetterSaving: SaveGameInternal hooked");

		self->hooks->World->RegisterOnBeforeWorldEndPlay(OnBeforeWorldEndPlay);
		return true;
	}

	void Shutdown()
	{
		LOG_DEBUG("BetterSaving: SaveHook::Shutdown");

		if (auto* self = GetSelf())
		{
			self->hooks->World->UnregisterOnBeforeWorldEndPlay(OnBeforeWorldEndPlay);
			if (g_hookHandle)
			{
				self->hooks->Hooks->Remove(g_hookHandle);
				g_hookHandle = nullptr;
			}
		}

		constexpr int kMaxWaitMs = 3000;
		constexpr int kSleepMs   = 50;
		int waited = 0;
		while (waited < kMaxWaitMs && g_saveInProgress.load(std::memory_order_acquire))
		{
			Sleep(kSleepMs); waited += kSleepMs;
		}

		LOG_DEBUG("BetterSaving: SaveHook::Shutdown complete");
	}

} // namespace SaveHook
