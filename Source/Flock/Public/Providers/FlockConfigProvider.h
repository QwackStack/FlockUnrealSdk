// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "Http/FlockProviderBase.h"
#include "Models/FlockConfigModels.h"

/**
 * De-duplicates concurrent asks for one key: the first caller starts the fetch, later callers queue, and
 * the single result fans out to all. With Blueprint async nodes in play, several widgets asking for the
 * same config on screen-open is the norm, so this collapses N identical round trips into one.
 */
template <typename T>
class TFlockInFlight
{
public:
	/** Registers OnComplete under Key. Returns true only for the first caller — the one that starts the fetch. */
	bool Register(const FString& Key, TFunction<void(TFlockResult<T>)> OnComplete)
	{
		TArray<TFunction<void(TFlockResult<T>)>>& Waiters = Map.FindOrAdd(Key);
		const bool bFirst = Waiters.Num() == 0;
		Waiters.Add(MoveTemp(OnComplete));
		return bFirst;
	}

	/** Delivers Result to every caller waiting on Key and clears it. */
	void Complete(const FString& Key, const TFlockResult<T>& Result)
	{
		TArray<TFunction<void(TFlockResult<T>)>> Waiters;
		Map.RemoveAndCopyValue(Key, Waiters);
		for (TFunction<void(TFlockResult<T>)>& Waiter : Waiters)
		{
			if (Waiter)
			{
				Waiter(Result);
			}
		}
	}

private:
	TMap<FString, TArray<TFunction<void(TFlockResult<T>)>>> Map;
};

/**
 * Game config + patches. Fetches (all patches, patch by id, patches by config, config by id/name, configs
 * by tag / version tag, player features) and the patch-else-config resolver behind ResolveConfigData —
 * which is the codegen entry point: it returns the resolved values as an opaque FFlockGameConfigData, so
 * the resolution policy stays out of the caller's type.
 *
 * Every read is memoized in-process and (except player features) backed by the offline snapshot, so a
 * second ask is free and a fetch survives an outage. All routes are public/enveloped — no sign-in gate;
 * the bearer rides along via the session's headers when present.
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

	/** Per-player resolved features. Memoized per player, but never snapshotted — stale features are worse than a refetch. */
	void GetPlayerFeatures(const FString& PlayerId, TFunction<void(TFlockResult<FFlockGameConfigSchema>)> OnComplete);

	/**
	 * Resolves a config's current values: the patch for this game version if one exists, otherwise the
	 * config's own base data (never empty defaults). The codegen entry point.
	 */
	void ResolveConfigData(const FString& ConfigId, TFunction<void(TFlockResult<FFlockGameConfigData>)> OnComplete);

	/** Drops every in-process cache and the config snapshot category, so the next fetch hits the backend. */
	void ClearCache();

private:
	FString MakeUrl(const FString& Path) const { return FString::Printf(TEXT("%s/%s"), *VersionedApiUrl, *Path); }
	TMap<FString, FString> HeadersNow() const { return Session->GetAuthHeaders(); }

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

	TFlockInFlight<FFlockGameConfigSchema> ConfigInFlight;
	TFlockInFlight<FFlockGamePatchSchema> PatchInFlight;
	TFlockInFlight<TArray<FFlockGameConfigSchema>> ConfigListInFlight;
	TFlockInFlight<TArray<FFlockGamePatchSchema>> PatchListInFlight;

	static const TCHAR* const SnapshotCategory;
};
