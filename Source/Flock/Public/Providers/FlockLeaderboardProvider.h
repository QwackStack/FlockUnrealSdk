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
 * Every read takes a name, and the name goes on the wire — all four `/v1` leaderboard routes are
 * by-name. There is no id-addressed read to resolve *for*, so a read spends exactly one request: the
 * board-config memo below serves GetByName, not a routing step. ResolveId exists for logging and deep
 * links only — nothing in this provider's read API takes an id.
 *
 * A board name the game does not have answers 404 on all three read routes, and that surfaces as a
 * **Validation** failure rather than a 404 or an empty page: it is a caller mistake, and standings with
 * no rows would send someone hunting for a data problem that isn't there.
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
	 * Rewrites a 404 from a by-name read into a Validation failure naming the board. The 404 status is
	 * **kept** on the translated error so the snapshot layer still reads it as an authoritative answer and
	 * propagates it, rather than quietly serving a cached page for a board that no longer exists.
	 */
	template <typename T>
	static TFlockResult<T> AsCallerMistake(TFlockResult<T> Result, const FString& LeaderboardName)
	{
		if (!Result.bSuccess && Result.Error.StatusCode == 404)
		{
			// The name is the overwhelmingly likely cause, but it is not the only one a 404 can carry — a
			// deleted season window or a misrouted API URL would land here too, and blaming the board name
			// would send someone to check a spelling that is correct. So the server's own words are appended
			// when it sent any, rather than replaced.
			FString Message = FString::Printf(TEXT("No leaderboard named '%s'"), *LeaderboardName);
			if (!Result.Error.ServerMessage.IsEmpty())
			{
				Message += FString::Printf(TEXT(" (server said: %s)"), *Result.Error.ServerMessage);
			}
			Result.Error = FFlockError::Make(EFlockErrorType::Validation, Message,
				Result.Error.StatusCode, Result.Error.Body, Result.Error.Code, Result.Error.ServerMessage);
		}
		return Result;
	}

	/**
	 * Wraps a by-name read so every failure it can produce runs through AsCallerMistake. The translation
	 * happens **inside** the operation, before the retry and snapshot layers see the result, so those two
	 * classify the same error the caller will be handed.
	 */
	template <typename T>
	static FFlockRetryHandler::FOperation<T> TranslatingUnknownBoard(
		FFlockRetryHandler::FOperation<T> Operation, const FString& LeaderboardName)
	{
		return [Operation, LeaderboardName](TFunction<void(TFlockResult<T>)> OnAttempt)
		{
			return Operation([OnAttempt, LeaderboardName](TFlockResult<T> Result)
			{
				OnAttempt(AsCallerMistake<T>(MoveTemp(Result), LeaderboardName));
			});
		};
	}

	/** Appends an optional query parameter, percent-encoded. Empty values are omitted, never sent blank. */
	static void AppendParam(FString& Query, const FString& Key, const FString& Value);

	/** Suffixes a snapshot key with the current player id, so placements cannot cross accounts. */
	FString PlayerScopedKey(const FString& Key) const;

	TSharedRef<FFlockAuthSession> Session;
	FString VersionedApiUrl;

	/**
	 * Name → board config, for GetByName/ResolveId only. **Not a routing step**: the reads put the name on
	 * the wire, so nothing consults this to build a URL. Re-adding a resolve in front of a read would
	 * reintroduce the defect this memo used to serve.
	 */
	TMap<FString, FFlockLeaderboard> BoardsByName;

	static const TCHAR* const SnapshotCategory;
};
