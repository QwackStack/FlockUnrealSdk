// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "Http/FlockInFlight.h"
#include "Http/FlockProviderBase.h"
#include "Models/FlockPlayerModels.h"

/**
 * Player data, player templates, and player bans.
 *
 * Templates are read once and memoized; the all-templates fetch is additionally backed by the offline
 * snapshot and coalesces concurrent asks (several widgets opening at once). By-id / by-name template reads
 * memoize but are not snapshotted, mirroring the Unity SDK. A player's own data (GetMyDataByTemplate /
 * GetMyDataByTag) is paginated once per player, cached in memory (never snapshotted) and coalesced per
 * player.
 *
 * Bans and the by-id / list / paged data reads are never cached: a ban is security state that can change
 * server-side at any moment (same rule as inventory), and a direct data read is always fresh.
 *
 * The game-commands provider writes back through this cache: ApplyServerPlayerData folds a mutated row in,
 * TryGetCachedRow reads the row a queued offline write overlays, and EvictPlayerCacheByRow drops a player's
 * rows when an optimistic write turns out to have been rejected.
 *
 * Completion-lambda rule: capture shared refs / weak self / values only — never `this`. Continuations that
 * re-enter the provider pin a TWeakPtr to itself, so teardown with requests in flight is safe.
 */
class FLOCK_API FFlockPlayerProvider
	: public FFlockProviderBase
	, public TSharedFromThis<FFlockPlayerProvider>
{
public:
	FFlockPlayerProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
		const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession,
		const FString& InVersionedApiUrl, const TSharedPtr<FFlockSnapshotStore>& InSnapshotStore,
		const FString& InGameVersionId);

	// ── Player data ──

	/** One data row by its id. Never cached — always a fresh read. */
	void GetDataById(const FString& PlayerDataId, TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete);

	/** A page of data rows, optionally filtered to one player (empty PlayerId = all players). Never cached. */
	void GetAllData(const FString& PlayerId, int32 Page, int32 Limit,
		TFunction<void(TFlockResult<FFlockPlayerDataPage>)> OnComplete);

	/**
	 * A page of the **signed-in player's** rows.
	 *
	 * Note this is not the same default as the PlayerId overload, where empty means *every* player — an
	 * admin-shaped read. Naming the common case explicitly keeps a caller from reaching for the other one
	 * and quietly paging the whole game's data.
	 */
	void GetMyData(int32 Page, int32 Limit, TFunction<void(TFlockResult<FFlockPlayerDataPage>)> OnComplete);

	/**
	 * The signed-in player's data row for a template. Paginates and caches all of the player's rows on the
	 * first call (any template), then serves from memory until ClearCache. A successful fetch with no row
	 * for the template yields Ok with an empty record (empty Id) — the mirror of Unity's null return.
	 */
	void GetMyDataByTemplate(const FString& PlayerTemplateId, TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete);

	/** The signed-in player's data row for the single template carrying the given tag (e.g. "currency"). */
	void GetMyDataByTag(const FString& Tag, TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete);

	// ── Templates ──

	/** All player templates for this game version. Memoized, snapshot-backed, and coalesced. */
	void GetTemplates(TFunction<void(TFlockResult<TArray<FFlockPlayerTemplateSchema>>)> OnComplete);

	/** One template by id. Memoized (not snapshotted). */
	void GetTemplateById(const FString& PlayerTemplateId, TFunction<void(TFlockResult<FFlockPlayerTemplateSchema>)> OnComplete);

	/** One template by name. Memoized (not snapshotted). */
	void GetTemplateByName(const FString& Name, TFunction<void(TFlockResult<FFlockPlayerTemplateSchema>)> OnComplete);

	/** The single template carrying the given tag; a Validation failure when none does. */
	void GetTemplateByTag(const FString& Tag, TFunction<void(TFlockResult<FFlockPlayerTemplateSchema>)> OnComplete);

	/** All data rows for a template (across players). Never cached. */
	void GetTemplatePlayerData(const FString& PlayerTemplateId, TFunction<void(TFlockResult<TArray<FFlockPlayerData>>)> OnComplete);

	// ── Bans ──

	/**
	 * A player's ban record (empty PlayerId = the signed-in player). Never cached — a ban can change
	 * server-side at any time. An unbanned player is a success with an empty record (see FFlockPlayerBan).
	 */
	void GetBan(const FString& PlayerId, TFunction<void(TFlockResult<FFlockPlayerBan>)> OnComplete);

	// ── Game-commands write-through seam ──

	/**
	 * Pushes a server-returned row into the per-player cache so a read straight after a mutation sees the
	 * new values instead of the pre-write ones. No-op for a row with no id, player, or template — those
	 * can't be keyed. Only touches a player whose rows are already cached: seeding a partial map would make
	 * the next GetMyDataByTemplate answer "no row" for every template that hasn't been fetched.
	 */
	void ApplyServerPlayerData(const FFlockPlayerData& Row);

	/** The cached row with this id, across every cached player; false when it isn't cached. */
	bool TryGetCachedRow(const FString& PlayerDataId, FFlockPlayerData& OutRow) const;

	/**
	 * Drops the cached rows of whichever player owns this row, so the next read refetches authoritative
	 * state. The whole player map goes, not the single row: the per-player cache is all-or-nothing, so a
	 * map missing one row would read as "that template has no row" rather than triggering a refetch.
	 */
	void EvictPlayerCacheByRow(const FString& PlayerDataId);

	/** Drops every in-process cache and the player-template snapshot category, so the next fetch hits the backend. */
	void ClearCache();

	/**
	 * Drops only the per-player data cache (the signed-in player's rows), leaving templates and their
	 * snapshot — which are game-version-scoped, not player-scoped — intact. Called by the subsystem on logout.
	 */
	void ClearPlayerDataCache();

private:
	FString MakeUrl(const FString& Path) const { return FString::Printf(TEXT("%s/%s"), *VersionedApiUrl, *Path); }
	TMap<FString, FString> HeadersNow() const { return Session->GetAuthHeaders(); }

	void IndexTemplate(const FFlockPlayerTemplateSchema& Template);
	TArray<FFlockPlayerTemplateSchema> ResolveTemplates(const TArray<FString>& Ids) const;

	/** Returns the cached per-player row map, or (coalesced) paginates and caches it. */
	void GetOrFetchPlayerData(const FString& PlayerId, TFunction<void(TFlockResult<TMap<FString, FFlockPlayerData>>)> OnComplete);
	/** Fetches one page of a player's data and either recurses to the next page or finishes the coalesced fetch. */
	void FetchPlayerDataPage(const FString& PlayerId, int32 Page, const TSharedRef<TMap<FString, FFlockPlayerData>>& Accumulated);

	TSharedRef<FFlockAuthSession> Session;
	FString VersionedApiUrl;

	// Templates live once, by id; the name index maps a name to an id.
	TMap<FString, FFlockPlayerTemplateSchema> TemplatesById;
	TMap<FString, FString> TemplateIdByName;
	bool bAllTemplatesFetched = false;
	TArray<FString> AllTemplateIds;

	// Per-player row cache, keyed by player id then template id. All-or-nothing (no single-row refetch path).
	TMap<FString, TMap<FString, FFlockPlayerData>> PlayerDataByPlayer;

	TFlockInFlight<TArray<FFlockPlayerTemplateSchema>> TemplateListInFlight;
	TFlockInFlight<TMap<FString, FFlockPlayerData>> PlayerDataInFlight;

	static const TCHAR* const SnapshotCategory;
};
