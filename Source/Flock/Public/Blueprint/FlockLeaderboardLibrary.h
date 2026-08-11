// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Models/FlockLeaderboardModels.h"
#include "FlockLeaderboardLibrary.generated.h"

/**
 * Pure Blueprint helpers over the leaderboard models: the three window makers and the two display
 * helpers that hang off a board.
 *
 * Every node delegates straight to the struct method, so Blueprint and C++ cannot drift on how a score
 * is formatted or which end of the scale wins — pinned by Flock.Leaderboard.Library.CppParity.
 *
 * The window makers return FFlockLeaderboardWindow so UE's context menu surfaces them when you drag off
 * a Window pin, which is the point of them being nodes at all.
 */
UCLASS()
class FLOCK_API UFlockLeaderboardLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * The board's live window — all-time, the current week, or the current season depending on how the
	 * board buckets. Sends no window parameter, which is what the API reads as "current".
	 */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Make Current Window"), Category = "Flock|Leaderboard")
	static FFlockLeaderboardWindow MakeCurrentWindow();

	/** One finished season on a seasonal board. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Make Season Window"), Category = "Flock|Leaderboard")
	static FFlockLeaderboardWindow MakeSeasonWindow(const FString& SeasonId);

	/** A raw period key, e.g. "2026-W31" on a weekly board. Sent verbatim. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Make Period Window"), Category = "Flock|Leaderboard")
	static FFlockLeaderboardWindow MakePeriodWindow(const FString& PeriodKey);

	/** True when no window parameter will be sent. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Is Current Window"), Category = "Flock|Leaderboard")
	static bool IsCurrentWindow(const FFlockLeaderboardWindow& Window);

	/**
	 * Formats a score the way the board measures it — a duration board reads as a clock rather than raw
	 * seconds. Pass a Player Rank's Ranked into bRanked; an unranked player and a non-finite
	 * score both format as an empty string, so a UI can bind this directly.
	 */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Format Score", AdvancedDisplay = "bRanked"), Category = "Flock|Leaderboard")
	static FString FormatScore(const FFlockLeaderboard& Leaderboard, double Score, bool bRanked = true);

	/** True when a bigger number ranks better. Sort and label from this rather than reading Direction. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Is Higher Better"), Category = "Flock|Leaderboard")
	static bool IsHigherBetter(const FFlockLeaderboard& Leaderboard);
};
