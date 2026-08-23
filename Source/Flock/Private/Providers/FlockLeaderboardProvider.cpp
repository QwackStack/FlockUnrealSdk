// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Providers/FlockLeaderboardProvider.h"

#include "Http/FlockEndpoints.h"
#include "Http/FlockJsonUtils.h"

const TCHAR* const FFlockLeaderboardProvider::SnapshotCategory = TEXT("leaderboard");

FFlockLeaderboardProvider::FFlockLeaderboardProvider(const TSharedRef<FFlockHttpClient>& InClient,
	const FFlockRetryPolicy& InPolicy, const TSharedRef<IFlockLogger>& InLogger,
	const TSharedRef<FFlockAuthSession>& InSession, const FString& InVersionedApiUrl,
	const TSharedPtr<FFlockSnapshotStore>& InSnapshotStore, const FString& InGameVersionId)
	: FFlockProviderBase(InClient, InPolicy, InLogger)
	, Session(InSession)
	, VersionedApiUrl(InVersionedApiUrl)
{
	SetSnapshotStore(InSnapshotStore, InGameVersionId);
	SetAuthSession(InSession);
}

void FFlockLeaderboardProvider::AppendParam(FString& Query, const FString& Key, const FString& Value)
{
	// Optional filters are omitted rather than sent empty — `country=` is not the same request as no
	// country at all, and the server would have to guess which one was meant.
	if (Value.IsEmpty())
	{
		return;
	}
	Query += Query.IsEmpty() ? TEXT("?") : TEXT("&");
	Query += Key + TEXT("=") + FlockEndpoints::Encode(Value);
}

FString FFlockLeaderboardProvider::PlayerScopedKey(const FString& Key) const
{
	return FString::Printf(TEXT("%s_%s"), *Key, *Session->GetPlayerId());
}

void FFlockLeaderboardProvider::GetByName(const FString& LeaderboardName,
	TFunction<void(TFlockResult<FFlockLeaderboard>)> OnComplete)
{
	if (!RequireNotEmpty(LeaderboardName, TEXT("Leaderboard Name"), OnComplete))
	{
		return;
	}
	if (const FFlockLeaderboard* Memoized = BoardsByName.Find(LeaderboardName))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockLeaderboard>::Ok(*Memoized));
		}
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::LeaderboardByName(LeaderboardName));
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockLeaderboardProvider> WeakSelf = AsShared();

	FetchWithSnapshot<FFlockLeaderboard>(SnapshotCategory,
		FString::Printf(TEXT("board_name_%s"), *LeaderboardName),
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockLeaderboard>)> OnAttempt)
		{
			// Enveloped ({error,response,result}) — the enveloped verb unwraps `result`.
			return ClientRef->Get<FFlockLeaderboard>(Url, Headers, MoveTemp(OnAttempt));
		},
		TEXT("Fetch leaderboard"),
		[WeakSelf, LeaderboardName, OnComplete](TFlockResult<FFlockLeaderboard> Result)
		{
			if (const TSharedPtr<FFlockLeaderboardProvider> Self = WeakSelf.Pin())
			{
				if (Result.bSuccess && !Result.Value.Id.IsEmpty())
				{
					Self->BoardsByName.Add(LeaderboardName, Result.Value);
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		});
}

void FFlockLeaderboardProvider::ResolveId(const FString& LeaderboardName,
	TFunction<void(TFlockResult<FString>)> OnComplete)
{
	GetByName(LeaderboardName, [OnComplete](TFlockResult<FFlockLeaderboard> Result)
	{
		if (!OnComplete)
		{
			return;
		}
		OnComplete(Result.bSuccess
			? TFlockResult<FString>::Ok(Result.Value.Id)
			: TFlockResult<FString>::Fail(Result.Error));
	});
}

/**
 * Standings for a board.
 *
 * One request, addressed by name. The board-config memo is not consulted here: `by-name/{name}/standings`
 * takes the name the caller passed, so resolving an id first would spend a round trip to learn something
 * the URL never asks for.
 */
void FFlockLeaderboardProvider::GetStandings(const FString& LeaderboardName, const FFlockLeaderboardWindow& Window,
	const FString& Country, int32 Page, int32 Limit, TFunction<void(TFlockResult<FFlockStandings>)> OnComplete)
{
	if (!RequireNotEmpty(LeaderboardName, TEXT("Leaderboard Name"), OnComplete))
	{
		return;
	}

	const FString WindowKey = Window.Key;
	FString Query;
	AppendParam(Query, TEXT("window"), WindowKey);
	AppendParam(Query, TEXT("country"), Country);
	Query += Query.IsEmpty() ? TEXT("?") : TEXT("&");
	Query += FString::Printf(TEXT("page=%d&limit=%d"), Page, Limit);

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TMap<FString, FString> Headers = HeadersNow();
	const FString Url = MakeUrl(FlockEndpoints::LeaderboardStandings(LeaderboardName) + Query);

	FetchWithSnapshot<FFlockStandings>(SnapshotCategory,
		FString::Printf(TEXT("standings_%s_%s_%s_p%d_l%d"), *LeaderboardName, *WindowKey, *Country, Page, Limit),
		TranslatingUnknownBoard<FFlockStandings>(
			[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockStandings>)> OnAttempt)
			{
				// Enveloped ({error,response,result}) — the enveloped verb unwraps `result`.
				return ClientRef->Get<FFlockStandings>(Url, Headers, MoveTemp(OnAttempt));
			},
			LeaderboardName),
		TEXT("Fetch leaderboard standings"), MoveTemp(OnComplete));
}

void FFlockLeaderboardProvider::GetMyRank(const FString& LeaderboardName, const FFlockLeaderboardWindow& Window,
	const FString& Country, TFunction<void(TFlockResult<FFlockPlayerRank>)> OnComplete)
{
	if (!RequireNotEmpty(LeaderboardName, TEXT("Leaderboard Name"), OnComplete))
	{
		return;
	}
	// Bearer-only route: fail before spending a request, rather than earning a guaranteed 401.
	if (!Session->IsAuthenticated())
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockPlayerRank>::Fail(
				FFlockError::Make(EFlockErrorType::Auth, TEXT("No player is signed in"))));
		}
		return;
	}

	const FString WindowKey = Window.Key;
	FString Query;
	AppendParam(Query, TEXT("window"), WindowKey);
	AppendParam(Query, TEXT("country"), Country);

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TMap<FString, FString> Headers = HeadersNow();
	const FString Url = MakeUrl(FlockEndpoints::LeaderboardMe(LeaderboardName) + Query);

	FetchWithSnapshot<FFlockPlayerRank>(SnapshotCategory,
		PlayerScopedKey(FString::Printf(TEXT("me_%s_%s_%s"), *LeaderboardName, *WindowKey, *Country)),
		TranslatingUnknownBoard<FFlockPlayerRank>(
			[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockPlayerRank>)> OnAttempt)
			{
				return ClientRef->Get<FFlockPlayerRank>(Url, Headers, MoveTemp(OnAttempt));
			},
			LeaderboardName),
		TEXT("Fetch player rank"), MoveTemp(OnComplete));
}

void FFlockLeaderboardProvider::GetAroundMe(const FString& LeaderboardName, int32 Neighbours,
	const FFlockLeaderboardWindow& Window, const FString& Country,
	TFunction<void(TFlockResult<FFlockStandings>)> OnComplete)
{
	if (!RequireNotEmpty(LeaderboardName, TEXT("Leaderboard Name"), OnComplete))
	{
		return;
	}
	if (!Session->IsAuthenticated())
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockStandings>::Fail(
				FFlockError::Make(EFlockErrorType::Auth, TEXT("No player is signed in"))));
		}
		return;
	}

	const FString WindowKey = Window.Key;
	FString Query;
	AppendParam(Query, TEXT("window"), WindowKey);
	AppendParam(Query, TEXT("country"), Country);
	Query += Query.IsEmpty() ? TEXT("?") : TEXT("&");
	Query += FString::Printf(TEXT("n=%d"), Neighbours);

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TMap<FString, FString> Headers = HeadersNow();
	const FString Url = MakeUrl(FlockEndpoints::LeaderboardAroundMe(LeaderboardName) + Query);

	FetchWithSnapshot<FFlockStandings>(SnapshotCategory,
		PlayerScopedKey(FString::Printf(TEXT("around_%s_%s_%s_n%d"),
			*LeaderboardName, *WindowKey, *Country, Neighbours)),
		TranslatingUnknownBoard<FFlockStandings>(
			[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockStandings>)> OnAttempt)
			{
				return ClientRef->Get<FFlockStandings>(Url, Headers, MoveTemp(OnAttempt));
			},
			LeaderboardName),
		TEXT("Fetch standings around player"), MoveTemp(OnComplete));
}

void FFlockLeaderboardProvider::ClearCache()
{
	BoardsByName.Empty();
	DeleteSnapshotCategory(SnapshotCategory);
}
