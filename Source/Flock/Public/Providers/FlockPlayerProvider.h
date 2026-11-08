// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "Http/FlockInFlight.h"
#include "Http/FlockProviderBase.h"
#include "Models/FlockPlayerModels.h"
#include "Models/FlockShopModels.h"

/**
 * Player data, player templates, and player bans.
 *
 * Templates are read once and memoized; the all-templates fetch is additionally backed by the offline
 * snapshot and coalesces concurrent asks (several widgets opening at once). By-id / by-name template reads
 * memoize but are not snapshotted. A player's own data (GetMyDataByTemplate /
 * GetMyDataByTag) is paginated once per player, cached in memory (never snapshotted) and coalesced per
 * player. A read already on its way when the cached data is dropped or a server row is applied (a purchase's wallet, a
 * game command's row) answers its callers but keeps nothing: it may be older than either.
 *
 * Every call that reads a player's data or ban acts for the signed-in player through their token, so it is sent only
 * while the sign-in current at the call lasts; template reads are game-wide. Every call reads the token on each attempt,
 * so the attempt after a token renewal carries the new one.
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
	 * for the template yields Ok with an empty record (empty Id) — having written no row yet is a
	 * normal state, not an error.
	 */
	void GetMyDataByTemplate(const FString& PlayerTemplateId, TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete);

	/** The signed-in player's data row for the single template carrying the given tag (e.g. "currency"). */
	void GetMyDataByTag(const FString& Tag, TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete);

	/**
	 * The signed-in player's wallet: balances by currency code, from the player template tagged "currency". Read through
	 * the same cache as GetMyDataByTemplate, which purchases, store redemptions, consumes and fund grants keep current;
	 * ClearCache reads it afresh. Refused (Validation) when the player has no wallet row in this Game Version.
	 */
	void GetWallet(TFunction<void(TFlockResult<FFlockWallet>)> OnComplete);

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
	 * snapshot — which are game-version-scoped, not player-scoped — intact. A read already on its way keeps nothing.
	 * Called by the subsystem on logout, and when a money call's outcome is unsure.
	 */
	void ClearPlayerDataCache();

	/** ClearPlayerDataCache on the given provider while it lives: what a money call forgets when its outcome is unsure. */
	static TFunction<void()> ClearsPlayerDataCacheOf(const TWeakPtr<FFlockPlayerProvider>& Players)
	{
		return [Players]()
		{
			if (const TSharedPtr<FFlockPlayerProvider> Pinned = Players.Pin())
			{
				Pinned->ClearPlayerDataCache();
			}
		};
	}

private:
	FString MakeUrl(const FString& Path) const { return FString::Printf(TEXT("%s/%s"), *VersionedApiUrl, *Path); }

	/** One page of player data rows, sent only while SignInNumber lasts. */
	void ReadDataPage(const FString& PlayerId, int32 SignInNumber, int32 Page, int32 Limit,
		TFunction<void(TFlockResult<FFlockPlayerDataPage>)> OnComplete);

	/** A paged read of one player's rows on its way: its callers, and what it was sent under. */
	struct FPlayerDataRead
	{
		TArray<TFunction<void(TFlockResult<TMap<FString, FFlockPlayerData>>)>> Waiters;
		/** PlayerDataDrops when it was sent: it keeps its answer only when nothing was dropped or applied since. */
		int32 DropsWhenSent = 0;
		/** Every page goes out only while this sign-in lasts. */
		int32 SignInNumber = 0;
	};

	/** Counts a drop or a server row (PlayerId empty: every player), and forgets the reads on their way so the next is sent afresh. */
	void DropReadsOnTheirWay(const FString& PlayerId);

	/** Ends a read: gives up its own place only, keeps its answer only when nothing was dropped since, and answers its callers. */
	void FinishPlayerDataRead(const FString& PlayerId, const TSharedRef<FPlayerDataRead>& Read,
		const TFlockResult<TMap<FString, FFlockPlayerData>>& Result);

	void IndexTemplate(const FFlockPlayerTemplateSchema& Template);
	TArray<FFlockPlayerTemplateSchema> ResolveTemplates(const TArray<FString>& Ids) const;

	/** Returns the cached per-player row map, or (coalesced) paginates and caches it. */
	void GetOrFetchPlayerData(const FString& PlayerId, TFunction<void(TFlockResult<TMap<FString, FFlockPlayerData>>)> OnComplete);
	/** Fetches one page of a player's data and either recurses to the next page or finishes the coalesced fetch. */
	void FetchPlayerDataPage(const FString& PlayerId, int32 Page, const TSharedRef<TMap<FString, FFlockPlayerData>>& Accumulated,
		const TSharedRef<FPlayerDataRead>& Read);

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
	/** The read of each player's rows on its way, by player id; one per player. */
	TMap<FString, TSharedRef<FPlayerDataRead>> PlayerDataReads;
	int32 PlayerDataDrops = 0;

	static const TCHAR* const SnapshotCategory;
};
