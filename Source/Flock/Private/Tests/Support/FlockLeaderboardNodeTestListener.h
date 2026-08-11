// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockError.h"
#include "Models/FlockLeaderboardModels.h"
#include "FlockLeaderboardNodeTestListener.generated.h"

/**
 * Bind target for the leaderboard async-node pin tests (dynamic delegates need UFUNCTION handlers on a
 * UObject). Not guarded by WITH_AUTOMATION_TESTS on purpose — UHT generates its registration unconditionally.
 */
UCLASS()
class UFlockLeaderboardNodeTestListener : public UObject
{
	GENERATED_BODY()

public:
	int32 LeaderboardPinCount = 0;
	int32 StandingsPinCount = 0;
	int32 PlayerRankPinCount = 0;
	FFlockError LastError;

	UFUNCTION()
	void HandleLeaderboardPin(const FFlockLeaderboard& Leaderboard, const FFlockError& Error) { ++LeaderboardPinCount; LastError = Error; }

	UFUNCTION()
	void HandleStandingsPin(const FFlockStandings& Standings, const FFlockError& Error) { ++StandingsPinCount; LastError = Error; }

	UFUNCTION()
	void HandlePlayerRankPin(const FFlockPlayerRank& PlayerRank, const FFlockError& Error) { ++PlayerRankPinCount; LastError = Error; }
};
