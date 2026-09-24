// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Misc/FlockEngineCompat.h"

#include "Http/FlockJsonUtils.h"
#include "Models/FlockLeaderboardModels.h"

namespace
{
	/** A board of every stated shape; the enum spellings here are the ones the API actually sends. */
	const TCHAR* const BoardBody =
		TEXT("{\"id\":\"lb-1\",\"name\":\"HighScoreTest\",\"value_type\":\"duration\",\"direction\":\"lower\",")
		TEXT("\"aggregation\":\"latest\",\"window_type\":\"seasonal\",\"scope\":\"country\"}");

	template <typename T>
	bool ParseModel(const FString& Body, T& OutModel, FString& OutError)
	{
		TSharedPtr<FJsonObject> Object;
		if (!FFlockJsonUtils::TryParseObject(Body, Object) || !Object.IsValid())
		{
			return false;
		}
		// Through WireObjectToStruct on purpose: proves the model is routed to its custom FromWireObject
		// rather than quietly falling back to the reflection path, which cannot map these enums.
		return FFlockJsonUtils::WireObjectToStruct(Object.ToSharedRef(), OutModel, OutError);
	}

	FFlockLeaderboard BoardOfType(EFlockLeaderboardValueType ValueType)
	{
		FFlockLeaderboard Board;
		Board.ValueType = ValueType;
		return Board;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardParsesBoardTest, "Flock.Leaderboard.Models.ParsesBoard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardParsesBoardTest::RunTest(const FString& Parameters)
{
	FFlockLeaderboard Board;
	FString Error;
	TestTrue(TEXT("board parses"), ParseModel(BoardBody, Board, Error));

	TestEqual(TEXT("id"), Board.Id, FString(TEXT("lb-1")));
	TestEqual(TEXT("name"), Board.Name, FString(TEXT("HighScoreTest")));
	// "duration" maps to DurationSeconds — the wire value never states the unit, the enumerator does.
	TestTrue(TEXT("value type"), Board.ValueType == EFlockLeaderboardValueType::DurationSeconds);
	TestTrue(TEXT("direction"), Board.Direction == EFlockLeaderboardDirection::Lower);
	TestTrue(TEXT("aggregation"), Board.Aggregation == EFlockLeaderboardAggregation::Latest);
	TestTrue(TEXT("window type"), Board.WindowType == EFlockLeaderboardWindowType::Seasonal);
	TestTrue(TEXT("scope"), Board.Scope == EFlockLeaderboardScope::Country);
	TestFalse(TEXT("lower is better"), Board.IsHigherBetter());

	// Every remaining spelling, so a reordered enum cannot pass by luck on the one sampled above.
	EFlockLeaderboardValueType ValueType = EFlockLeaderboardValueType::DurationSeconds;
	TestTrue(TEXT("integer parses"), FlockLeaderboardValueTypeFromWire(TEXT("integer"), ValueType));
	TestTrue(TEXT("integer value"), ValueType == EFlockLeaderboardValueType::Integer);
	TestTrue(TEXT("float parses"), FlockLeaderboardValueTypeFromWire(TEXT("float"), ValueType));
	TestTrue(TEXT("float value"), ValueType == EFlockLeaderboardValueType::Float);

	EFlockLeaderboardAggregation Aggregation = EFlockLeaderboardAggregation::Latest;
	TestTrue(TEXT("best parses"), FlockLeaderboardAggregationFromWire(TEXT("best"), Aggregation));
	TestTrue(TEXT("best value"), Aggregation == EFlockLeaderboardAggregation::Best);
	TestTrue(TEXT("sum parses"), FlockLeaderboardAggregationFromWire(TEXT("sum"), Aggregation));
	TestTrue(TEXT("sum value"), Aggregation == EFlockLeaderboardAggregation::Sum);

	EFlockLeaderboardWindowType WindowType = EFlockLeaderboardWindowType::Seasonal;
	TestTrue(TEXT("never parses"), FlockLeaderboardWindowTypeFromWire(TEXT("never"), WindowType));
	TestTrue(TEXT("never value"), WindowType == EFlockLeaderboardWindowType::Never);
	TestTrue(TEXT("weekly parses"), FlockLeaderboardWindowTypeFromWire(TEXT("weekly"), WindowType));
	TestTrue(TEXT("weekly value"), WindowType == EFlockLeaderboardWindowType::Weekly);

	// Round trip, so the wire spellings can never drift from what parsing accepts.
	TestEqualSensitive(TEXT("value type to wire"), FlockLeaderboardValueTypeToWire(EFlockLeaderboardValueType::DurationSeconds), FString(TEXT("duration")));
	TestEqualSensitive(TEXT("direction to wire"), FlockLeaderboardDirectionToWire(EFlockLeaderboardDirection::Lower), FString(TEXT("lower")));
	TestEqualSensitive(TEXT("scope to wire"), FlockLeaderboardScopeToWire(EFlockLeaderboardScope::Country), FString(TEXT("country")));
	return true;
}

// A board that silently defaulted its direction would sort a best-time board backwards, and nothing
// downstream could tell that from a correctly-parsed board. So an unknown value fails the parse.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardRejectsUnknownEnumTest, "Flock.Leaderboard.Models.RejectsUnknownEnumValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardRejectsUnknownEnumTest::RunTest(const FString& Parameters)
{
	const FString Body =
		TEXT("{\"id\":\"lb-1\",\"name\":\"B\",\"value_type\":\"quaternion\",\"direction\":\"higher\",")
		TEXT("\"aggregation\":\"best\",\"window_type\":\"never\",\"scope\":\"global\"}");

	FFlockLeaderboard Board;
	FString Error;
	TestFalse(TEXT("unknown value type is rejected"), ParseModel(Body, Board, Error));
	TestTrue(TEXT("error names the field"), Error.Contains(TEXT("value_type")));
	TestTrue(TEXT("error names the bad value"), Error.Contains(TEXT("quaternion")));

	const FString Missing =
		TEXT("{\"id\":\"lb-1\",\"name\":\"B\",\"direction\":\"higher\",")
		TEXT("\"aggregation\":\"best\",\"window_type\":\"never\",\"scope\":\"global\"}");
	FFlockLeaderboard Incomplete;
	TestFalse(TEXT("missing value type is rejected"), ParseModel(Missing, Incomplete, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardParsesStandingsTest, "Flock.Leaderboard.Models.ParsesStandings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardParsesStandingsTest::RunTest(const FString& Parameters)
{
	// total deliberately larger than items.Num(): a UI that pages off items.Num() caps every board at one page.
	const FString Body =
		TEXT("{\"window\":\"2026-W31\",\"total\":874,\"items\":[")
		TEXT("{\"rank\":1,\"player_id\":\"p1\",\"player_name\":\"Ada\",\"score\":9001.5,\"country\":\"SA\",\"achieved_at\":\"2026-08-01T10:00:00Z\"},")
		TEXT("{\"rank\":2,\"player_id\":\"p2\",\"player_name\":null,\"score\":42,\"country\":null,\"achieved_at\":\"2026-08-02T10:00:00Z\"}")
		TEXT("]}");

	FFlockStandings Standings;
	FString Error;
	TestTrue(TEXT("standings parse"), ParseModel(Body, Standings, Error));
	TestEqual(TEXT("window"), Standings.Window, FString(TEXT("2026-W31")));
	TestEqual(TEXT("total is the board count, not the page size"), Standings.Total, 874);
	TestEqual(TEXT("returned rows"), Standings.Items.Num(), 2);

	if (Standings.Items.Num() == 2)
	{
		TestEqual(TEXT("rank"), Standings.Items[0].Rank, 1);
		TestEqual(TEXT("player id"), Standings.Items[0].PlayerId, FString(TEXT("p1")));
		TestEqual(TEXT("player name"), Standings.Items[0].PlayerName, FString(TEXT("Ada")));
		TestEqual(TEXT("score"), Standings.Items[0].Score, 9001.5);
		TestEqual(TEXT("country"), Standings.Items[0].Country, FString(TEXT("SA")));
		TestEqual(TEXT("achieved at stays raw"), Standings.Items[0].AchievedAt, FString(TEXT("2026-08-01T10:00:00Z")));

		// Nullable strings become empty, never the literal "null" — that would render in a leaderboard row.
		TestTrue(TEXT("null player name is empty"), Standings.Items[1].PlayerName.IsEmpty());
		TestTrue(TEXT("null country is empty"), Standings.Items[1].Country.IsEmpty());
	}
	return true;
}

/**
 * A standings row may carry fields this model has no member for — some are studio-facing and are not a
 * game client's business, and the server is free to add more at any time.
 *
 * The model must ignore them rather than fail, and must not carry them into the snapshot: a field the
 * SDK does not model is one it cannot promise to keep correct across a version. Adding a member for one
 * is a deliberate decision, not a gap to be filled because the field appeared on the wire.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardIgnoresUnknownFieldsTest, "Flock.Leaderboard.Models.IgnoresUnknownEntryFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardIgnoresUnknownFieldsTest::RunTest(const FString& Parameters)
{
	// Both shapes an unmodelled field arrives in: absent-but-null, and carrying a value.
	const FString Body =
		TEXT("{\"window\":\"all\",\"total\":2,\"items\":[")
		TEXT("{\"rank\":1,\"player_id\":\"p1\",\"player_name\":\"Ada\",\"score\":10,\"country\":null,\"achieved_at\":\"\",\"some_future_field\":null},")
		TEXT("{\"rank\":2,\"player_id\":\"p2\",\"player_name\":\"Bob\",\"score\":9,\"country\":null,\"achieved_at\":\"\",\"some_future_field\":\"value\"}")
		TEXT("]}");

	FFlockStandings Standings;
	FString Error;
	TestTrue(TEXT("standings parse despite unmodelled fields"), ParseModel(Body, Standings, Error));
	TestEqual(TEXT("both rows kept"), Standings.Items.Num(), 2);

	if (Standings.Items.Num() == 2)
	{
		// The modelled members are unaffected by whatever else rode along.
		TestEqual(TEXT("row still reads correctly"), Standings.Items[1].PlayerName, FString(TEXT("Bob")));
		TestEqual(TEXT("its score is untouched"), Standings.Items[1].Score, 9.0);
	}

	// The reflection path drives the snapshot round trip, so nothing unmodelled may sneak in there either.
	FString Plain;
	TestTrue(TEXT("serializes for the snapshot"), FFlockJsonUtils::StructToPlainJson(Standings, Plain));
	TestFalse(TEXT("unmodelled field is not persisted"), Plain.Contains(TEXT("some_future_field"), ESearchCase::IgnoreCase));
	return true;
}

// The whole justification for the Ranked bool: null means "no entry", but rank 0 and score 0 are
// legitimate values, so a sentinel cannot tell the two apart. Both directions are pinned here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardUnrankedWhenRankNullTest, "Flock.Leaderboard.Models.UnrankedWhenRankNull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardUnrankedWhenRankNullTest::RunTest(const FString& Parameters)
{
	FFlockPlayerRank Rank;
	FString Error;
	TestTrue(TEXT("parses"), ParseModel(
		TEXT("{\"player_id\":\"p1\",\"window\":\"all\",\"rank\":null,\"score\":null}"), Rank, Error));

	TestFalse(TEXT("not ranked"), Rank.Ranked);
	TestEqual(TEXT("rank left at zero"), Rank.Rank, 0);
	TestEqual(TEXT("score left at zero"), Rank.Score, 0.0);
	TestEqual(TEXT("player id still read"), Rank.PlayerId, FString(TEXT("p1")));

	// Absent, rather than explicitly null, must read the same way.
	FFlockPlayerRank Absent;
	TestTrue(TEXT("parses without the fields"), ParseModel(
		TEXT("{\"player_id\":\"p1\",\"window\":\"all\"}"), Absent, Error));
	TestFalse(TEXT("absent is also unranked"), Absent.Ranked);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardRankedWhenRankZeroTest, "Flock.Leaderboard.Models.RankedWhenRankZero",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardRankedWhenRankZeroTest::RunTest(const FString& Parameters)
{
	FFlockPlayerRank Rank;
	FString Error;
	TestTrue(TEXT("parses"), ParseModel(
		TEXT("{\"player_id\":\"p1\",\"window\":\"all\",\"rank\":0,\"score\":0}"), Rank, Error));

	// A sentinel of 0 or -1 would have eaten this row; Ranked is what keeps it distinct from "no entry".
	TestTrue(TEXT("a real zero rank is ranked"), Rank.Ranked);
	TestEqual(TEXT("rank"), Rank.Rank, 0);
	TestEqual(TEXT("score"), Rank.Score, 0.0);
	return true;
}

// The other case a sentinel would have eaten: a lower-is-better board where a negative score is real.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardKeepsNegativeScoreTest, "Flock.Leaderboard.Models.KeepsNegativeScore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardKeepsNegativeScoreTest::RunTest(const FString& Parameters)
{
	FFlockPlayerRank Rank;
	FString Error;
	TestTrue(TEXT("parses"), ParseModel(
		TEXT("{\"player_id\":\"p1\",\"window\":\"all\",\"rank\":3,\"score\":-12.5}"), Rank, Error));

	TestTrue(TEXT("ranked"), Rank.Ranked);
	TestEqual(TEXT("negative score survives"), Rank.Score, -12.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardFormatsByValueTypeTest, "Flock.Leaderboard.Models.FormatsByValueType",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardFormatsByValueTypeTest::RunTest(const FString& Parameters)
{
	const FFlockLeaderboard Integer = BoardOfType(EFlockLeaderboardValueType::Integer);
	const FFlockLeaderboard Float = BoardOfType(EFlockLeaderboardValueType::Float);
	const FFlockLeaderboard Duration = BoardOfType(EFlockLeaderboardValueType::DurationSeconds);

	TestEqual(TEXT("integer"), Integer.FormatScore(1234.0), FString(TEXT("1234")));
	TestEqual(TEXT("integer rounds"), Integer.FormatScore(1234.6), FString(TEXT("1235")));

	TestEqual(TEXT("float keeps needed decimals"), Float.FormatScore(1.5), FString(TEXT("1.5")));
	TestEqual(TEXT("float drops trailing zeros"), Float.FormatScore(2.0), FString(TEXT("2")));
	TestEqual(TEXT("float caps at two decimals"), Float.FormatScore(0.256), FString(TEXT("0.26")));

	// Seconds in, clock out. Past an hour the hour component appears; below it, it does not.
	TestEqual(TEXT("sub-hour duration"), Duration.FormatScore(83.25), FString(TEXT("1:23.250")));
	TestEqual(TEXT("past-hour duration"), Duration.FormatScore(3723.5), FString(TEXT("1:02:03.500")));
	TestEqual(TEXT("negative duration"), Duration.FormatScore(-83.25), FString(TEXT("-1:23.250")));

	// An unranked player has no number to show, and "nan" must never reach a leaderboard row.
	TestEqual(TEXT("unranked formats empty"), Integer.FormatScore(0.0, false), FString());
	TestEqual(TEXT("unranked duration formats empty"), Duration.FormatScore(83.25, false), FString());
	TestEqual(TEXT("NaN formats empty"), Float.FormatScore(FMath::Sqrt(-1.0)), FString());
	TestEqual(TEXT("infinity formats empty"), Float.FormatScore(TNumericLimits<double>::Max() * 2.0), FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardWindowMakersTest, "Flock.Leaderboard.Models.WindowMakers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardWindowMakersTest::RunTest(const FString& Parameters)
{
	// Current sends nothing at all, which is what the API reads as the board's live window.
	const FFlockLeaderboardWindow Current = FFlockLeaderboardWindow::Current();
	TestTrue(TEXT("current is current"), Current.IsCurrent());
	TestTrue(TEXT("current has no key"), Current.Key.IsEmpty());

	const FFlockLeaderboardWindow Season = FFlockLeaderboardWindow::Season(TEXT("abc"));
	TestEqualSensitive(TEXT("season key"), Season.Key, FString(TEXT("season:abc")));
	TestFalse(TEXT("season is not current"), Season.IsCurrent());

	const FFlockLeaderboardWindow Period = FFlockLeaderboardWindow::Period(TEXT("2026-W31"));
	TestEqualSensitive(TEXT("period key is verbatim"), Period.Key, FString(TEXT("2026-W31")));
	TestFalse(TEXT("period is not current"), Period.IsCurrent());

	// A default-constructed window must behave as Current, since that is what a caller who omits it gets.
	const FFlockLeaderboardWindow Defaulted;
	TestTrue(TEXT("default is current"), Defaulted.IsCurrent());
	return true;
}

#endif // WITH_AUTOMATION_TESTS
