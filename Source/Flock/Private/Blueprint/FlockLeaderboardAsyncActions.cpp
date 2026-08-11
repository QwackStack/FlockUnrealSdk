// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockLeaderboardAsyncActions.h"

#include "Engine/BlueprintGeneratedClass.h"
#include "FlockSubsystem.h"
#include "Providers/FlockLeaderboardProvider.h"

namespace
{
	FString ResolveCallOrigin(const UObject* WorldContextObject)
	{
		if (const UBlueprintGeneratedClass* BlueprintClass =
			WorldContextObject ? Cast<UBlueprintGeneratedClass>(WorldContextObject->GetClass()) : nullptr)
		{
			FString AssetName = BlueprintClass->GetName();
			AssetName.RemoveFromEnd(TEXT("_C"));
			return FString::Printf(TEXT("Blueprint '%s'"), *AssetName);
		}
		return TEXT("Blueprint node");
	}

	FFlockLeaderboardProvider* ResolveLeaderboard(UObject* WorldContextObject, FFlockError& OutError)
	{
		UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
		FFlockLeaderboardProvider* Provider = Sdk ? Sdk->GetLeaderboardProvider() : nullptr;
		if (!Provider)
		{
			OutError = FFlockError::Make(EFlockErrorType::Validation,
				TEXT("Flock leaderboards are not available. Initialize the SDK first."));
		}
		return Provider;
	}
}

// ───────────────────────────── Get Leaderboard ─────────────────────────────

UFlockGetLeaderboardAction* UFlockGetLeaderboardAction::GetLeaderboard(UObject* WorldContextObject,
	const FString& LeaderboardName)
{
	UFlockGetLeaderboardAction* Action = NewObject<UFlockGetLeaderboardAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->LeaderboardName = LeaderboardName;
	return Action;
}

void UFlockGetLeaderboardAction::Activate()
{
	FFlockError Error;
	FFlockLeaderboardProvider* Provider = ResolveLeaderboard(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockLeaderboard>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetLeaderboardAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->GetByName(LeaderboardName, [WeakThis](TFlockResult<FFlockLeaderboard> Result)
	{
		if (UFlockGetLeaderboardAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockGetLeaderboardAction::Complete(const TFlockResult<FFlockLeaderboard>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockLeaderboard(), Result.Error);
	}
	SetReadyToDestroy();
}

// ────────────────────────── Get Leaderboard Standings ──────────────────────

UFlockGetLeaderboardStandingsAction* UFlockGetLeaderboardStandingsAction::GetStandings(UObject* WorldContextObject,
	const FString& LeaderboardName, FFlockLeaderboardWindow Window, const FString& Country, int32 Page, int32 Limit)
{
	UFlockGetLeaderboardStandingsAction* Action = NewObject<UFlockGetLeaderboardStandingsAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->LeaderboardName = LeaderboardName;
	Action->Window = Window;
	Action->Country = Country;
	Action->Page = Page;
	Action->Limit = Limit;
	return Action;
}

void UFlockGetLeaderboardStandingsAction::Activate()
{
	FFlockError Error;
	FFlockLeaderboardProvider* Provider = ResolveLeaderboard(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockStandings>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetLeaderboardStandingsAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->GetStandings(LeaderboardName, Window, Country, Page, Limit,
		[WeakThis](TFlockResult<FFlockStandings> Result)
		{
			if (UFlockGetLeaderboardStandingsAction* Self = WeakThis.Get())
			{
				Self->Complete(Result);
			}
		});
}

void UFlockGetLeaderboardStandingsAction::Complete(const TFlockResult<FFlockStandings>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockStandings(), Result.Error);
	}
	SetReadyToDestroy();
}

// ─────────────────────────────── Get My Rank ───────────────────────────────

UFlockGetMyRankAction* UFlockGetMyRankAction::GetMyRank(UObject* WorldContextObject, const FString& LeaderboardName,
	FFlockLeaderboardWindow Window, const FString& Country)
{
	UFlockGetMyRankAction* Action = NewObject<UFlockGetMyRankAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->LeaderboardName = LeaderboardName;
	Action->Window = Window;
	Action->Country = Country;
	return Action;
}

void UFlockGetMyRankAction::Activate()
{
	FFlockError Error;
	FFlockLeaderboardProvider* Provider = ResolveLeaderboard(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockPlayerRank>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetMyRankAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->GetMyRank(LeaderboardName, Window, Country, [WeakThis](TFlockResult<FFlockPlayerRank> Result)
	{
		if (UFlockGetMyRankAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockGetMyRankAction::Complete(const TFlockResult<FFlockPlayerRank>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockPlayerRank(), Result.Error);
	}
	SetReadyToDestroy();
}

// ───────────────────────── Get Standings Around Me ─────────────────────────

UFlockGetStandingsAroundMeAction* UFlockGetStandingsAroundMeAction::GetStandingsAroundMe(UObject* WorldContextObject,
	const FString& LeaderboardName, FFlockLeaderboardWindow Window, const FString& Country, int32 Neighbours)
{
	UFlockGetStandingsAroundMeAction* Action = NewObject<UFlockGetStandingsAroundMeAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->LeaderboardName = LeaderboardName;
	Action->Window = Window;
	Action->Country = Country;
	Action->Neighbours = Neighbours;
	return Action;
}

void UFlockGetStandingsAroundMeAction::Activate()
{
	FFlockError Error;
	FFlockLeaderboardProvider* Provider = ResolveLeaderboard(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockStandings>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetStandingsAroundMeAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->GetAroundMe(LeaderboardName, Neighbours, Window, Country,
		[WeakThis](TFlockResult<FFlockStandings> Result)
		{
			if (UFlockGetStandingsAroundMeAction* Self = WeakThis.Get())
			{
				Self->Complete(Result);
			}
		});
}

void UFlockGetStandingsAroundMeAction::Complete(const TFlockResult<FFlockStandings>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockStandings(), Result.Error);
	}
	SetReadyToDestroy();
}
