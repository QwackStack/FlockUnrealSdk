// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "Http/FlockInFlight.h"
#include "Http/FlockProviderBase.h"
#include "Models/FlockConfigModels.h"

/**
 * Game config + patches. Fetches (all patches, patch by id, patches by config, config by id/name, configs
 * by tag / version tag, player features) and the patch-else-config resolver behind ResolveConfigData —
 * which is the codegen entry point: it returns the resolved values as an opaque FFlockStructuredData, so
 * the resolution policy stays out of the caller's type.
 *
 * Every read is memoized in-process and (except player features) backed by the offline snapshot, so a
 * second ask is free and a fetch survives an outage. All routes are enveloped. The game's reads need no
 * sign-in (the bearer rides along via the session's headers when present); ResolveConfigData's read for
 * the signed-in player is sent only as that sign-in.
 *
 * Completion-lambda rule: capture shared refs / weak self / values only — never `this`. Continuations that
 * re-enter the provider pin a TWeakPtr to itself, so teardown with requests in flight is safe.
 */
class FLOCK_API FFlockConfigProvider
	: public FFlockProviderBase
	, public TSharedFromThis<FFlockConfigProvider>
{
public:
	FFlockConfigProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
		const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession,
		const FString& InVersionedApiUrl, const TSharedPtr<FFlockSnapshotStore>& InSnapshotStore,
		const FString& InGameVersionId);

	// ── Patches ──
	void GetAllPatches(TFunction<void(TFlockResult<TArray<FFlockGamePatchSchema>>)> OnComplete);
	void GetPatchById(const FString& PatchId, TFunction<void(TFlockResult<FFlockGamePatchSchema>)> OnComplete);
	void GetPatchesByConfig(const FString& ConfigId, TFunction<void(TFlockResult<TArray<FFlockGamePatchSchema>>)> OnComplete);

	// ── Configs ──
	void GetConfigById(const FString& ConfigId, TFunction<void(TFlockResult<FFlockGameConfigSchema>)> OnComplete);
	void GetConfigByName(const FString& Name, TFunction<void(TFlockResult<FFlockGameConfigSchema>)> OnComplete);
	void GetConfigsByTag(EFlockConfigTag Tag, TFunction<void(TFlockResult<TArray<FFlockGameConfigSchema>>)> OnComplete);
	void GetConfigsByVersionTag(EFlockConfigTag Tag, TFunction<void(TFlockResult<TArray<FFlockGameConfigSchema>>)> OnComplete);

	/**
	 * Per-player resolved features (empty PlayerId = the signed-in player; Validation when signed out).
	 * Kept in memory per player for the run (PlayerFeaturesByPlayer): the first read of a player fetches, later reads of
	 * the same player answer from memory, and ClearCache() or a new SDK instance fetches again. Never written to the
	 * offline snapshot, so a new launch always asks the server.
	 */
	void GetPlayerFeatures(const FString& PlayerId, TFunction<void(TFlockResult<FFlockGameConfigSchema>)> OnComplete);

	/**
	 * Resolves a config's current values. With a player signed in, a gameplay or feature config's are the values the server
	 * resolves for that player: the newest patch, then the overrides of the segments they are in, then their own. Otherwise, and
	 * for any other config, the patch for this game version if one exists, else the config's own base data (never empty
	 * defaults). The codegen entry point.
	 *
	 * The player's values are held for the sign-in and kept on disk per player, so a read offline answers with that player's last
	 * values, else the game's. When the player's read is refused (no such player, no Game Version, no such config) or gets no
	 * answer, the game's values answer; a refusal holds for the rest of the sign-in, no answer for 30 seconds.
	 */
	void ResolveConfigData(const FString& ConfigId, TFunction<void(TFlockResult<FFlockStructuredData>)> OnComplete);

	/** Drops every in-process cache and the config snapshot category, so the next fetch hits the backend. */
	void ClearCache();

	/** How long a config's player read is not sent again after it got no answer (30 seconds). */
	void SetPlayerReadPauseAfterNoAnswerForTesting(double Seconds) { PlayerReadPauseAfterNoAnswerSeconds = Seconds; }

private:
	FString MakeUrl(const FString& Path) const { return FString::Printf(TEXT("%s/%s"), *VersionedApiUrl, *Path); }

	/** The patch for this game version if one exists, else the config's own data: the read with nobody signed in. */
	void ResolveForTheGame(const FString& ConfigId, TFunction<void(TFlockResult<FFlockStructuredData>)> OnComplete);

	/**
	 * The config as the signed-in player reads it, held for that sign-in and on disk for that player. Unset with nobody signed in
	 * or when the player's read cannot answer, so the game's read answers as it does for a player signed out. Fails only when
	 * the read was cancelled.
	 */
	void ReadForTheSignedInPlayer(const FString& ConfigId, TFunction<void(TFlockResult<TOptional<FFlockGameConfigSchema>>)> Then);

	/**
	 * Reads the config by its name, which the read by id gives (kept, so asked once), from the player's route; every try goes
	 * out as SignInNumber's or not at all. Unset for a config the server does not resolve for a player.
	 */
	void FetchForThePlayer(const FString& ConfigId, int32 SignInNumber, bool bHasSavedCopy,
		TFunction<void(TFlockResult<TOptional<FFlockGameConfigSchema>>)> Then);

	/** The game's read answers this config for the rest of the sign-in; a saved copy the server no longer stands behind is deleted. */
	void LeaveToTheGamesRead(int32 SignInNumber, const FString& ConfigId, const FString& Key, bool bDeleteTheSavedCopy);

	/** The player's saved copy of a config, read from disk once a sign-in. */
	TOptional<FFlockGameConfigSchema> SavedPlayerConfig(const FString& ConfigId, const FString& Key);

	/** The player's configs, emptied when the sign-in they were read for is no longer the one asked about. */
	TMap<FString, TOptional<FFlockGameConfigSchema>>& PlayerConfigsOf(int32 SignInNumber);

	void IndexPatch(const FFlockGamePatchSchema& Patch);
	void IndexConfig(const FFlockGameConfigSchema& Config);
	TArray<FFlockGamePatchSchema> ResolvePatches(const TArray<FString>& Ids) const;
	TArray<FFlockGameConfigSchema> ResolveConfigs(const TArray<FString>& Ids) const;

	TSharedRef<FFlockAuthSession> Session;
	FString VersionedApiUrl;

	// Every patch/config lives here once, by id; the index maps hold id lists for the query variants.
	TMap<FString, FFlockGamePatchSchema> PatchesById;
	TMap<FString, FFlockGameConfigSchema> ConfigsById;
	TMap<FString, TArray<FString>> PatchIdsByConfig;
	TMap<FString, TArray<FString>> ConfigIdsByTag;
	TMap<FString, TArray<FString>> ConfigIdsByVersionTag;
	TMap<FString, FString> ConfigIdByName;
	TMap<FString, FFlockGameConfigSchema> PlayerFeaturesByPlayer;
	bool bAllPatchesFetched = false;
	TArray<FString> AllPatchIds;

	/** The configs as the signed-in player reads them, for one sign-in only; an unset entry means the game's read answers that config. */
	TMap<FString, TOptional<FFlockGameConfigSchema>> PlayerConfigsById;
	/** The player's copies on disk, read once a sign-in (unset: none), so offline reads do not read the file each time. */
	TMap<FString, TOptional<FFlockGameConfigSchema>> SavedPlayerConfigsById;
	/** When a config's player read, which got no answer, may be sent again; until then the saved copy, else the game's read, answers. */
	TMap<FString, FDateTime> PlayerReadsPausedUntil;
	int32 PlayerConfigsSignIn = -1;
	/** Counts ClearCache calls, so a player's read on its way when one ran keeps nothing. */
	int32 Clears = 0;
	/** The sign-in last warned that the server got no Game Version, so the warning comes once a sign-in. */
	int32 WarnedOfNoGameVersionSignIn = -1;
	double PlayerReadPauseAfterNoAnswerSeconds = 30.0;

	TFlockInFlight<FFlockGameConfigSchema> ConfigInFlight;
	TFlockInFlight<FFlockGamePatchSchema> PatchInFlight;
	TFlockInFlight<TArray<FFlockGameConfigSchema>> ConfigListInFlight;
	TFlockInFlight<TArray<FFlockGamePatchSchema>> PatchListInFlight;

	static const TCHAR* const SnapshotCategory;
};
