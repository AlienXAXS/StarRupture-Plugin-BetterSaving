#pragma once

#include "plugin_interface.h"
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"
#include "plugin_map.h"

#include <cstdint>

// Every game function and every member of a game class the plugin takes from
// the loader's version object map for the running game build (see
// plugin_map.h), under one name each. The members are resolved once in
// OnPluginLoadHooks and read on the game thread with In(object), which is null
// when the object is null or the member did not resolve. A rename in the map is
// a one-line edit here. The byte patterns the plugin falls back on are in
// save_hook.cpp.
//
// Only the dedicated server build asks the loader. The client build is not on
// the version map yet: it keeps every member at the offset it was compiled
// with, as it always did.

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

#define BETTERSAVING_FIELD(Class, Member) SR_FIELD(decltype(SDK::Class::Member), Class, Member, 0)

// UWorld's time members are engine state the game's reflection does not
// describe, so they are native rows: the plugin gives its own row for the build
// it is compiled for (ResolveNative), and the loader's rows take over on any
// other build.
#define BETTERSAVING_WORLD_TIME(Member, offset) \
	PluginField<double>{ { "UWorld::" #Member, "UWorld", #Member, PLUGIN_FIELD_NATIVE, sizeof(double), (offset) } }

namespace BetterSavingFields
{
	// The functions the plugin hooks or calls, under the game's own names, the
	// same key for every plugin. On the server the loader's map holds rows for
	// ProjectSavedDir and UStructToJsonObjectString; the other two are found by
	// the plugin's own pattern.
	inline constexpr const char* ProjectSavedDir            = "FPaths::ProjectSavedDir";
	inline constexpr const char* WriteUserFile              = "UCrSaveGameUtils::WriteUserFile";
	inline constexpr const char* SaveGameInternal           = "UCrSaveGameUtils::SaveGameInternal";
	inline constexpr const char* UStructToJsonObjectString  = "FJsonObjectConverter::UStructToJsonObjectString";

	// The save subsystem: the save data it holds and the two delegates the
	// plugin takes over from SaveGameInternal.
	inline auto SubsystemSaveData      = BETTERSAVING_FIELD(UCrSaveSubsystem, SaveData);
	inline auto SubsystemOnPreSaveStart = BETTERSAVING_FIELD(UCrSaveSubsystem, OnPreSaveStart);

	// The members of the save data the plugin refreshes before it snapshots it.
	inline auto SaveDataLevel          = SR_FIELD(FStringRaw, FCrSaveGameData, Level, 0);
	inline auto SaveDataTimestamp      = SR_FIELD(FStringRaw, FCrSaveGameData, Timestamp, 0);
	inline auto SaveDataGameVersion    = BETTERSAVING_FIELD(FCrSaveGameData, GameVersion);
	inline auto SaveDataWorldTime      = BETTERSAVING_FIELD(FCrSaveGameData, WorldTimeSeconds);
	inline auto SaveDataWorldUnpaused  = BETTERSAVING_FIELD(FCrSaveGameData, WorldUnpausedTimeSeconds);
	inline auto SaveDataWorldReal      = BETTERSAVING_FIELD(FCrSaveGameData, WorldRealTimeSeconds);
	inline auto SaveDataWorldAudio     = BETTERSAVING_FIELD(FCrSaveGameData, WorldAudioTimeSeconds);

	// The save data is snapshot by value into the save thread's buffer, and the
	// game version is copied by value into it: both need the layout the plugin
	// was compiled with.
	inline auto SaveDataSize           = SR_SIZE_CHECK(FCrSaveGameData);
	inline auto GameVersionSize        = SR_SIZE_CHECK(FCrGameVersion);

	// The world: the game state, and the clocks the save data records.
	inline auto WorldGameState         = BETTERSAVING_FIELD(UWorld, GameState);
	inline auto WorldTimeSeconds       = BETTERSAVING_WORLD_TIME(TimeSeconds, 0x838);
	inline auto WorldUnpausedTimeSeconds = BETTERSAVING_WORLD_TIME(UnpausedTimeSeconds, 0x840);
	inline auto WorldRealTimeSeconds   = BETTERSAVING_WORLD_TIME(RealTimeSeconds, 0x848);
	inline auto WorldAudioTimeSeconds  = BETTERSAVING_WORLD_TIME(AudioTimeSeconds, 0x850);

#ifdef MODLOADER_SERVER_BUILD
	// A native member with the plugin's own row for the build it is compiled
	// for: the offset and size it was compiled with, on PLUGIN_GAME_BUILT_CL.
	// The loader's row for the running build wins when it has one; on any build
	// that has neither, the request fails and the loader refuses the plugin.
	inline bool ResolveNative(IPluginHookScanner* scanner, IPluginSelf* self, PluginFieldSpec& field)
	{
		const PluginFieldCandidate row{ PLUGIN_GAME_BUILT_CL, field.builtOffset, field.size, 0, nullptr };
		return PluginMap::ResolveField(scanner, self, field, &row, 1);
	}
#else
	// The client build's answer: each member at the offset and size it was
	// compiled with.
	template <class... Fields>
	void UseCompiled(Fields&... fields)
	{
		((fields.offset = fields.builtOffset, fields.resolvedSize = fields.size), ...);
	}
#endif

	// Asks the loader for every member above. False when any did not resolve;
	// the loader has then recorded the failure and refuses the plugin.
	inline bool Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
#ifdef MODLOADER_SERVER_BUILD
		bool all = PluginMap::ResolveFields(scanner, self,
			SubsystemSaveData, SubsystemOnPreSaveStart,
			SaveDataLevel, SaveDataTimestamp, SaveDataGameVersion,
			SaveDataWorldTime, SaveDataWorldUnpaused, SaveDataWorldReal, SaveDataWorldAudio,
			SaveDataSize, GameVersionSize, WorldGameState);
		all = ResolveNative(scanner, self, WorldTimeSeconds) && all;
		all = ResolveNative(scanner, self, WorldUnpausedTimeSeconds) && all;
		all = ResolveNative(scanner, self, WorldRealTimeSeconds) && all;
		all = ResolveNative(scanner, self, WorldAudioTimeSeconds) && all;
		return all;
#else
		(void)self;
		(void)scanner;
		UseCompiled(
			SubsystemSaveData, SubsystemOnPreSaveStart,
			SaveDataLevel, SaveDataTimestamp, SaveDataGameVersion,
			SaveDataWorldTime, SaveDataWorldUnpaused, SaveDataWorldReal, SaveDataWorldAudio,
			SaveDataSize, GameVersionSize,
			WorldGameState, WorldTimeSeconds, WorldUnpausedTimeSeconds,
			WorldRealTimeSeconds, WorldAudioTimeSeconds);
		return true;
#endif
	}
}
