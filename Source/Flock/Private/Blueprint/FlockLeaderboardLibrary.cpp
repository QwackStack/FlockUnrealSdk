// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockLeaderboardLibrary.h"

// Every node here is a straight delegation. Anything computed in this file instead of on the struct
// would be a second implementation that only Blueprint graphs exercise, and the two would drift.

FFlockLeaderboardWindow UFlockLeaderboardLibrary::MakeCurrentWindow()
{
	return FFlockLeaderboardWindow::Current();
}

FFlockLeaderboardWindow UFlockLeaderboardLibrary::MakeSeasonWindow(const FString& SeasonId)
{
	return FFlockLeaderboardWindow::Season(SeasonId);
}

FFlockLeaderboardWindow UFlockLeaderboardLibrary::MakePeriodWindow(const FString& PeriodKey)
{
	return FFlockLeaderboardWindow::Period(PeriodKey);
}

bool UFlockLeaderboardLibrary::IsCurrentWindow(const FFlockLeaderboardWindow& Window)
{
	return Window.IsCurrent();
}

FString UFlockLeaderboardLibrary::FormatScore(const FFlockLeaderboard& Leaderboard, double Score, bool bRanked)
{
	return Leaderboard.FormatScore(Score, bRanked);
}

bool UFlockLeaderboardLibrary::IsHigherBetter(const FFlockLeaderboard& Leaderboard)
{
	return Leaderboard.IsHigherBetter();
}
