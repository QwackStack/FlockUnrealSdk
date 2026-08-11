// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "Http/FlockProviderBase.h"
#include "Models/FlockLeaderboardModels.h"

/**
 * Read-only leaderboard access, addressed by board **name**.
 *
 * There is no score-submit call by design: a board projects over a player-data field, so a player moves
 * up it by writing that field through the commands surface. Anyone looking for SubmitScore wants
 * FFlockCommandProvider::UpdatePlayerDataField.
 *
 * Every read takes a name. The board id is resolved once through the by-name route and memoized for the
 * session, because resolving a name should not cost a round trip on every read. ResolveId exists for
 * logging and deep links only — nothing in this provider's read API takes an id.
 *
 * Reads are snapshot-backed, so a board UI shows the last-known standings when the network is down
 * rather than an error screen. The /me and /around-me keys are **player-scoped**: one player's placement
 * must never be served to the next player on a shared device.
 *
 * Auth: no leaderboard route declares `security`, so there is no blanket sign-in guard. But /me and
 * /around-me each declare an Authorization header and answer with a player-scoped schema, so those two
 * fail fast when signed out — before the name lookup is spent — rather than earning a guaranteed 401.
 *
 * Completion-lambda rule: capture shared refs / weak self / values only — never `this`. Continuations
 * that re-enter the provider pin a TWeakPtr to itself, so teardown with requests in flight is safe.
 */
class FLOCK_API FFlockLeaderboardProvider
	: public FFlockProviderBase
	, public TSharedFromThis<FFlockLeaderboardProvider>
{
public:
	FFlockLeaderboardProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
		const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession,
		const FString& InVersionedApiUrl, const TSharedPtr<FFlockSnapshotStore>& InSnapshotStore,
		const FString& InGameVersionId);

	/** A board's public configuration. Open to signed-out players; memoized after the first call. */
	void GetByName(const FString& LeaderboardName, TFunction<void(TFlockResult<FFlockLeaderboard>)> OnComplete);

	/** The board's id, for logging or a deep link. Reads take the name — nothing here consumes an id. */
	void ResolveId(const FString& LeaderboardName, TFunction<void(TFlockResult<FString>)> OnComplete);

	/**
	 * Ranked standings for a board. Open to signed-out players. An empty Country omits the filter; the
	 * default window is whatever the board is currently serving.
	 */
	void GetStandings(const FString& LeaderboardName, const FFlockLeaderboardWindow& Window, const FString& Country,
		int32 Page, int32 Limit, TFunction<void(TFlockResult<FFlockStandings>)> OnComplete);

	/** Standings with the usual defaults — the board's live window, no country filter, first page of 50. */
	void GetStandings(const FString& LeaderboardName, TFunction<void(TFlockResult<FFlockStandings>)> OnComplete)
	{
		GetStandings(LeaderboardName, FFlockLeaderboardWindow::Current(), FString(), 1, 50, MoveTemp(OnComplete));
	}

	/**
	 * The signed-in player's own placement. A result with Ranked false is the documented "no entry on this
	 * board yet" answer, not a failure.
	 */
	void GetMyRank(const FString& LeaderboardName, const FFlockLeaderboardWindow& Window, const FString& Country,
		TFunction<void(TFlockResult<FFlockPlayerRank>)> OnComplete);

	/** The signed-in player's placement in the board's live window. */
	void GetMyRank(const FString& LeaderboardName, TFunction<void(TFlockResult<FFlockPlayerRank>)> OnComplete)
	{
		GetMyRank(LeaderboardName, FFlockLeaderboardWindow::Current(), FString(), MoveTemp(OnComplete));
	}

	/** The Neighbours entries either side of the signed-in player, for a "you are here" view. */
	void GetAroundMe(const FString& LeaderboardName, int32 Neighbours, const FFlockLeaderboardWindow& Window,
		const FString& Country, TFunction<void(TFlockResult<FFlockStandings>)> OnComplete);

	/** Five either side, in the board's live window. */
	void GetAroundMe(const FString& LeaderboardName, TFunction<void(TFlockResult<FFlockStandings>)> OnComplete)
	{
		GetAroundMe(LeaderboardName, 5, FFlockLeaderboardWindow::Current(), FString(), MoveTemp(OnComplete));
	}

	/** Drops the name memo and the leaderboard snapshot category, so the next read hits the backend. */
	void ClearCache();

private:
	FString MakeUrl(const FString& Path) const { return FString::Printf(TEXT("%s/%s"), *VersionedApiUrl, *Path); }
	TMap<FString, FString> HeadersNow() const { return Session->GetAuthHeaders(); }

	/**
	 * Resolves a name to an id and hands it to Continue, or fails. A name this game does not have is a
	 * caller mistake, so it is a Validation failure rather than an empty result.
	 */
	void WithBoardId(const FString& LeaderboardName, TFunction<void(const FString&)> Continue,
		TFunction<void(const FFlockError&)> OnFailure);

	/** Appends an optional query parameter, percent-encoded. Empty values are omitted, never sent blank. */
	static void AppendParam(FString& Query, const FString& Key, const FString& Value);

	/** Suffixes a snapshot key with the current player id, so placements cannot cross accounts. */
	FString PlayerScopedKey(const FString& Key) const;

	TSharedRef<FFlockAuthSession> Session;
	FString VersionedApiUrl;

	/** Name → board. Every read resolves a name first, and that must not cost a round trip each time. */
	TMap<FString, FFlockLeaderboard> BoardsByName;

	static const TCHAR* const SnapshotCategory;
};
