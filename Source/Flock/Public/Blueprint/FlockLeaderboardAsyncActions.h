// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockError.h"
#include "Http/FlockResult.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Models/FlockLeaderboardModels.h"
#include "FlockLeaderboardAsyncActions.generated.h"

/**
 * Blueprint async nodes for leaderboards. Each resolves the SDK from its world context on Activate and
 * fires exactly one pin; when the SDK is unavailable the failure pin fires with a Validation error.
 *
 * Every node takes a board **name** — the id is resolved internally, and a designer has a name from the
 * dashboard rather than an id. There is no submit node: a board projects over a player-data field, so a
 * score is written with the Flock command nodes.
 *
 * Build a Window pin with the Make Current / Season / Period Window nodes on UFlockLeaderboardLibrary;
 * leaving it unconnected means the board's live window, which is what most graphs want.
 */

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockLeaderboardPin, const FFlockLeaderboard&, Leaderboard, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockStandingsPin, const FFlockStandings&, Standings, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockPlayerRankPin, const FFlockPlayerRank&, PlayerRank, const FFlockError&, Error);

/** Fetches a board's public configuration by name. Open to signed-out players. */
UCLASS()
class FLOCK_API UFlockGetLeaderboardAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockLeaderboardPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockLeaderboardPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Leaderboard"), Category = "Flock|Leaderboard")
	static UFlockGetLeaderboardAction* GetLeaderboard(UObject* WorldContextObject, const FString& LeaderboardName);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockLeaderboard>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString LeaderboardName;
};

/** Fetches a page of ranked standings. Open to signed-out players. */
UCLASS()
class FLOCK_API UFlockGetLeaderboardStandingsAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockStandingsPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockStandingsPin OnFailure;

	/** An empty Country omits the filter; an unset Window means the board's live window. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Leaderboard Standings", AdvancedDisplay = "Window,Country"), Category = "Flock|Leaderboard")
	static UFlockGetLeaderboardStandingsAction* GetStandings(UObject* WorldContextObject, const FString& LeaderboardName,
		FFlockLeaderboardWindow Window, const FString& Country, int32 Page = 1, int32 Limit = 50);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockStandings>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString LeaderboardName;
	FFlockLeaderboardWindow Window;
	FString Country;
	int32 Page = 1;
	int32 Limit = 50;
};

/**
 * Fetches the signed-in player's placement. Requires a signed-in player. A success whose Ranked is false
 * is the documented "no entry on this board yet" answer — check Ranked before showing Rank or Score.
 */
UCLASS()
class FLOCK_API UFlockGetMyRankAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockPlayerRankPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockPlayerRankPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get My Rank", AdvancedDisplay = "Window,Country"), Category = "Flock|Leaderboard")
	static UFlockGetMyRankAction* GetMyRank(UObject* WorldContextObject, const FString& LeaderboardName,
		FFlockLeaderboardWindow Window, const FString& Country);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockPlayerRank>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString LeaderboardName;
	FFlockLeaderboardWindow Window;
	FString Country;
};

/** Fetches the entries either side of the signed-in player. Requires a signed-in player. */
UCLASS()
class FLOCK_API UFlockGetStandingsAroundMeAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockStandingsPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockStandingsPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Standings Around Me", AdvancedDisplay = "Window,Country"), Category = "Flock|Leaderboard")
	static UFlockGetStandingsAroundMeAction* GetStandingsAroundMe(UObject* WorldContextObject,
		const FString& LeaderboardName, FFlockLeaderboardWindow Window, const FString& Country, int32 Neighbours = 5);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockStandings>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString LeaderboardName;
	FFlockLeaderboardWindow Window;
	FString Country;
	int32 Neighbours = 5;
};
