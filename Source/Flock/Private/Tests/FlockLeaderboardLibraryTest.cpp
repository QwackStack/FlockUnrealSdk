// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Blueprint/FlockLeaderboardAsyncActions.h"
#include "Blueprint/FlockLeaderboardLibrary.h"
#include "Tests/Support/FlockLeaderboardNodeTestListener.h"

namespace
{
	FFlockLeaderboard BoardOfType(EFlockLeaderboardValueType ValueType,
		EFlockLeaderboardDirection Direction = EFlockLeaderboardDirection::Higher)
	{
		FFlockLeaderboard Board;
		Board.ValueType = ValueType;
		Board.Direction = Direction;
		return Board;
	}
}

/**
 * Every library node must return exactly what the struct method returns. They are one-line delegations
 * today; this is what stops someone "fixing" a formatting bug in the Blueprint half only, leaving graphs
 * and C++ disagreeing about what a score reads as.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardLibraryParityTest, "Flock.Leaderboard.Library.CppParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardLibraryParityTest::RunTest(const FString& Parameters)
{
	// Window makers.
	TestEqual(TEXT("current window"), UFlockLeaderboardLibrary::MakeCurrentWindow().Key,
		FFlockLeaderboardWindow::Current().Key);
	TestEqualSensitive(TEXT("season window"), UFlockLeaderboardLibrary::MakeSeasonWindow(TEXT("s7")).Key,
		FFlockLeaderboardWindow::Season(TEXT("s7")).Key);
	TestEqualSensitive(TEXT("period window"), UFlockLeaderboardLibrary::MakePeriodWindow(TEXT("2026-W31")).Key,
		FFlockLeaderboardWindow::Period(TEXT("2026-W31")).Key);

	TestTrue(TEXT("current is current"), UFlockLeaderboardLibrary::IsCurrentWindow(FFlockLeaderboardWindow::Current()));
	TestFalse(TEXT("season is not current"),
		UFlockLeaderboardLibrary::IsCurrentWindow(FFlockLeaderboardWindow::Season(TEXT("s7"))));

	// Direction.
	const FFlockLeaderboard Higher = BoardOfType(EFlockLeaderboardValueType::Integer, EFlockLeaderboardDirection::Higher);
	const FFlockLeaderboard Lower = BoardOfType(EFlockLeaderboardValueType::Integer, EFlockLeaderboardDirection::Lower);
	TestEqual(TEXT("higher"), UFlockLeaderboardLibrary::IsHigherBetter(Higher), Higher.IsHigherBetter());
	TestEqual(TEXT("lower"), UFlockLeaderboardLibrary::IsHigherBetter(Lower), Lower.IsHigherBetter());
	TestTrue(TEXT("higher board is higher-better"), UFlockLeaderboardLibrary::IsHigherBetter(Higher));
	TestFalse(TEXT("lower board is not"), UFlockLeaderboardLibrary::IsHigherBetter(Lower));

	// Formatting, across every value type and every special case the struct handles.
	const TArray<EFlockLeaderboardValueType> Types = {
		EFlockLeaderboardValueType::Integer,
		EFlockLeaderboardValueType::Float,
		EFlockLeaderboardValueType::DurationSeconds
	};
	const TArray<double> Scores = {
		0.0, 1.0, 1.5, 2.0, 0.256, 1234.6, 83.25, 3723.5, -83.25, -12.5,
		FMath::Sqrt(-1.0), TNumericLimits<double>::Max() * 2.0
	};

	for (EFlockLeaderboardValueType Type : Types)
	{
		const FFlockLeaderboard Board = BoardOfType(Type);
		for (double Score : Scores)
		{
			TestEqual(FString::Printf(TEXT("format %f as type %d"), Score, static_cast<int32>(Type)),
				UFlockLeaderboardLibrary::FormatScore(Board, Score), Board.FormatScore(Score));

			// The unranked path too — a UI binds this directly off a Player Rank's Ranked pin.
			TestEqual(FString::Printf(TEXT("format unranked %f as type %d"), Score, static_cast<int32>(Type)),
				UFlockLeaderboardLibrary::FormatScore(Board, Score, false), Board.FormatScore(Score, false));
			TestTrue(FString::Printf(TEXT("unranked is empty for type %d"), static_cast<int32>(Type)),
				UFlockLeaderboardLibrary::FormatScore(Board, Score, false).IsEmpty());
		}
	}
	return true;
}

/**
 * A graph can reach these nodes before the SDK is up. Each must fire exactly one pin — the failure pin,
 * carrying a Validation error — never zero (the graph would hang) and never a crash. A null world context
 * resolves to no subsystem, which is the uninitialized case. The success path is covered by the provider
 * tests, which drive the same provider methods directly.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardNodeUninitializedTest, "Flock.Leaderboard.Node.Uninitialized",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardNodeUninitializedTest::RunTest(const FString& Parameters)
{
	UFlockLeaderboardNodeTestListener* Listener = NewObject<UFlockLeaderboardNodeTestListener>();
	UObject* BadContext = nullptr;

	auto ExpectValidation = [this, Listener](const TCHAR* What)
	{
		TestEqual(FString::Printf(TEXT("%s: validation error"), What),
			static_cast<int32>(Listener->LastError.Type), static_cast<int32>(EFlockErrorType::Validation));
	};

	{
		UFlockGetLeaderboardAction* Action = UFlockGetLeaderboardAction::GetLeaderboard(BadContext, TEXT("HighScoreTest"));
		Action->OnSuccess.AddDynamic(Listener, &UFlockLeaderboardNodeTestListener::HandleLeaderboardPin);
		Action->OnFailure.AddDynamic(Listener, &UFlockLeaderboardNodeTestListener::HandleLeaderboardPin);
		Action->Activate();
		TestEqual(TEXT("get leaderboard fired one pin"), Listener->LeaderboardPinCount, 1);
		ExpectValidation(TEXT("get leaderboard"));
	}
	{
		UFlockGetLeaderboardStandingsAction* Action = UFlockGetLeaderboardStandingsAction::GetStandings(
			BadContext, TEXT("HighScoreTest"), FFlockLeaderboardWindow::Current(), FString(), 1, 50);
		Action->OnSuccess.AddDynamic(Listener, &UFlockLeaderboardNodeTestListener::HandleStandingsPin);
		Action->OnFailure.AddDynamic(Listener, &UFlockLeaderboardNodeTestListener::HandleStandingsPin);
		Action->Activate();
		TestEqual(TEXT("get standings fired one pin"), Listener->StandingsPinCount, 1);
		ExpectValidation(TEXT("get standings"));
	}
	{
		UFlockGetMyRankAction* Action = UFlockGetMyRankAction::GetMyRank(
			BadContext, TEXT("HighScoreTest"), FFlockLeaderboardWindow::Current(), FString());
		Action->OnSuccess.AddDynamic(Listener, &UFlockLeaderboardNodeTestListener::HandlePlayerRankPin);
		Action->OnFailure.AddDynamic(Listener, &UFlockLeaderboardNodeTestListener::HandlePlayerRankPin);
		Action->Activate();
		TestEqual(TEXT("get my rank fired one pin"), Listener->PlayerRankPinCount, 1);
		ExpectValidation(TEXT("get my rank"));
	}
	{
		UFlockGetStandingsAroundMeAction* Action = UFlockGetStandingsAroundMeAction::GetStandingsAroundMe(
			BadContext, TEXT("HighScoreTest"), FFlockLeaderboardWindow::Current(), FString(), 3);
		Action->OnSuccess.AddDynamic(Listener, &UFlockLeaderboardNodeTestListener::HandleStandingsPin);
		Action->OnFailure.AddDynamic(Listener, &UFlockLeaderboardNodeTestListener::HandleStandingsPin);
		Action->Activate();
		TestEqual(TEXT("get standings around me fired one more pin"), Listener->StandingsPinCount, 2);
		ExpectValidation(TEXT("get standings around me"));
	}

	return true;
}

#endif // WITH_AUTOMATION_TESTS
