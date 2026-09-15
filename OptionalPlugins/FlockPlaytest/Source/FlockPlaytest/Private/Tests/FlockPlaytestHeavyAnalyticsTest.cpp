// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "FlockPlaytestLibrary.h"
#include "FlockPlaytestPerformanceTimeline.h"
#include "Tests/FlockPlaytestSubsystemTestSupport.h"
#include "Tests/FlockPlaytestTestSupport.h"
#include "UObject/UObjectGlobals.h"

using namespace FlockPlaytestSubsystemTesting;
using namespace FlockPlaytestFixtures;

namespace
{
	/** Hands over one frame time in a new engine frame, through the same ticker path the engine drives. */
	void TickInANewEngineFrame(FPlaytestFixture& Fixture, float FrameSeconds)
	{
		++*Fixture.EngineFrameNumber;
		Fixture.Playtest->TickPerformanceTimelineForTesting(FrameSeconds);
	}

	/** Plays Seconds of frames at 60 a second, one engine frame each. */
	void Play(FPlaytestFixture& Fixture, double Seconds)
	{
		const int32 Frames = FMath::RoundToInt(Seconds * 60.0);
		for (int32 Index = 0; Index < Frames; ++Index)
		{
			TickInANewEngineFrame(Fixture, 1.f / 60.f);
		}
	}

	/** Starts the fixture with a player signed in to the Flock SDK, and a playtest that turns heavy analytics on or off. */
	void StartSignedIn(FPlaytestFixture& Fixture, bool bHeavyAnalytics)
	{
		Fixture.AnswerConfig(FFlockPlaytestFakeTransport::Status(200, ConfigBody(GameVersionId, bHeavyAnalytics)));
		Fixture.SignInToFlockOnStart();
		Fixture.StartFlock();
	}

	TArray<TSharedPtr<FJsonObject>> EventsNamed(const TArray<TSharedPtr<FJsonObject>>& Events, const TCHAR* Name)
	{
		return Events.FilterByPredicate([Name](const TSharedPtr<FJsonObject>& Event)
		{
			return StringMember(Event, TEXT("event_name")).Equals(Name, ESearchCase::CaseSensitive);
		});
	}

	int32 WindowsSent(const FPlaytestFixture& Fixture)
	{
		return EventsNamed(Fixture.SentPlaytestEvents(), FlockPlaytestEvents::PerformanceWindow).Num();
	}

	/** An event's properties, or an empty object, so a missing member fails the check that reads it. */
	TSharedPtr<FJsonObject> PropertiesOf(const TSharedPtr<FJsonObject>& Event)
	{
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		if (Event.IsValid() && Event->TryGetObjectField(TEXT("properties"), Properties) && Properties->IsValid())
		{
			return *Properties;
		}
		return MakeShared<FJsonObject>();
	}

	/** A number member, or -1 when the member is missing or not a number. */
	double NumberMember(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
	{
		double Value = -1.0;
		return Object->HasTypedField<EJson::Number>(Name) && Object->TryGetNumberField(Name, Value) ? Value : -1.0;
	}

	bool FollowsAnyLevelLoads(const UFlockPlaytestSubsystem* Playtest)
	{
		return FCoreUObjectDelegates::PreLoadMapWithContext.IsBoundToObject(Playtest)
			|| FCoreUObjectDelegates::PostLoadMapWithWorld.IsBoundToObject(Playtest);
	}

	bool FollowsBothLevelLoadEvents(const UFlockPlaytestSubsystem* Playtest)
	{
		return FCoreUObjectDelegates::PreLoadMapWithContext.IsBoundToObject(Playtest)
			&& FCoreUObjectDelegates::PostLoadMapWithWorld.IsBoundToObject(Playtest);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestHeavyAnalyticsOffRecordsNothingTest,
	"Flock.Playtest.HeavyAnalytics.OffRecordsNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestHeavyAnalyticsOffRecordsNothingTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings FlockAnalytics(true);
	FPlaytestFixture Fixture;
	StartSignedIn(Fixture, /*bHeavyAnalytics*/ false);
	ExpectPlaytestStatus(*this, TEXT("Ready"), Fixture.Playtest->GetStatus(), EFlockPlaytestStatus::Ready);
	TestTrue(TEXT("Precondition: a player is signed in to the Flock SDK"), Fixture.Flock->IsAuthenticated());
	const int32 RequestsBefore = Fixture.AnalyticsEventRequests();

	TestFalse(TEXT("Nothing is measured"), Fixture.Playtest->IsMeasuringPerformance());
	TestFalse(TEXT("No level load is followed"), FollowsAnyLevelLoads(Fixture.Playtest));

	Play(Fixture, 60.0);
	FWorldContext Context;
	Context.OwningGameInstance = Fixture.GameInstance;
	const FScopedTestWorld Arena(TEXT("Arena"), Fixture.GameInstance);
	Fixture.Playtest->HandlePreLoadMapForTesting(Context, TEXT("/Game/Maps/Arena"));
	Fixture.Playtest->HandlePostLoadMapForTesting(Arena.World);
	TestFalse(TEXT("The game's own playtest event is not recorded"), Fixture.Playtest->RecordPlaytestEvent(TEXT("boss_defeated")));

	TestEqual(TEXT("Not one analytics events request is sent"), Fixture.AnalyticsEventRequests() - RequestsBefore, 0);
	TestEqual(TEXT("And no playtest event"), Fixture.SentPlaytestEvents().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestHeavyAnalyticsSixWindowsInAMinuteOfPlayTest,
	"Flock.Playtest.HeavyAnalytics.SixWindowsInAMinuteOfPlay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestHeavyAnalyticsSixWindowsInAMinuteOfPlayTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings FlockAnalytics(true);
	FPlaytestLogCapture Log;
	FPlaytestFixture Fixture;
	StartSignedIn(Fixture, /*bHeavyAnalytics*/ true);
	ExpectPlaytestStatus(*this, TEXT("Ready"), Fixture.Playtest->GetStatus(), EFlockPlaytestStatus::Ready);
	TestTrue(TEXT("Precondition: a player is signed in to the Flock SDK"), Fixture.Flock->IsAuthenticated());

	TestTrue(TEXT("Performance is measured"), Fixture.Playtest->IsMeasuringPerformance());
	TestTrue(TEXT("Level loads are followed"), FollowsBothLevelLoadEvents(Fixture.Playtest));
	TestEqual(TEXT("Starting is logged once"), Log.LinesContaining(TEXT("Heavy analytics is on")).Num(), 1);

	const int32 RequestsBefore = Fixture.AnalyticsEventRequests();
	Play(Fixture, 60.0);

	const TArray<TSharedPtr<FJsonObject>> Windows = EventsNamed(Fixture.SentPlaytestEvents(), FlockPlaytestEvents::PerformanceWindow);
	TestEqual(TEXT("A minute of play sends six performance windows"), Windows.Num(), 6);
	TestEqual(TEXT("Each in a request of its own"), Fixture.AnalyticsEventRequests() - RequestsBefore, 6);

	const TArray<FString> NumberMembers = { TEXT("window_seconds"), TEXT("frames"), TEXT("median_frame_time_ms"),
		TEXT("frame_time_95th_percentile_ms"), TEXT("frame_time_99th_percentile_ms"), TEXT("hitches"),
		TEXT("hitch_threshold_ms"), TEXT("memory_used_mb"), TEXT("memory_peak_mb") };
	for (const TSharedPtr<FJsonObject>& Window : Windows)
	{
		TestEqual(TEXT("It belongs to the signed-in player"), StringMember(Window, TEXT("player_id")), FString(FlockPlayerId));
		const TSharedPtr<FJsonObject> Properties = PropertiesOf(Window);
		for (const FString& Member : NumberMembers)
		{
			TestTrue(FString::Printf(TEXT("It carries %s as a number"), *Member), Properties->HasTypedField<EJson::Number>(Member));
		}
		// No world is loaded here, so there is no map: exactly those members, and nothing the Flock SDK already sends.
		TestEqual(TEXT("It carries nothing else"), Properties->Values.Num(), NumberMembers.Num());
		for (const TCHAR* SessionMember : { TEXT("duration_seconds"), TEXT("pause_count"), TEXT("min_fps"), TEXT("max_fps"), TEXT("average_fps") })
		{
			TestFalse(FString::Printf(TEXT("It does not repeat the session's %s"), SessionMember), Properties->HasField(SessionMember));
		}
		TestEqual(TEXT("Ten seconds of frames at 60 a second"), NumberMember(Properties, TEXT("frames")), 600.0);
		const double Seconds = NumberMember(Properties, TEXT("window_seconds"));
		TestTrue(TEXT("Covering ten seconds"), Seconds >= 10.0 && Seconds <= 10.05);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestHeavyAnalyticsBackgroundTimeIsNotPlayTest,
	"Flock.Playtest.HeavyAnalytics.BackgroundTimeIsNotPlay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestHeavyAnalyticsBackgroundTimeIsNotPlayTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings FlockAnalytics(true);
	FPlaytestFixture Fixture;
	StartSignedIn(Fixture, /*bHeavyAnalytics*/ true);
	TestTrue(TEXT("Precondition: performance is measured"), Fixture.Playtest->IsMeasuringPerformance());

	Play(Fixture, 25.0);
	Fixture.Playtest->SetBackgroundedForTesting(true);
	Play(Fixture, 30.0);
	// The first frame time after the return carries the time spent away.
	Fixture.Playtest->SetBackgroundedForTesting(false);
	TickInANewEngineFrame(Fixture, 5.f);
	TestEqual(TEXT("Only play counts: 25 seconds make two windows"), WindowsSent(Fixture), 2);

	Play(Fixture, 5.0);
	TestEqual(TEXT("The five seconds left over and five more make the third"), WindowsSent(Fixture), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestHeavyAnalyticsStopsWithPlaytestingTest,
	"Flock.Playtest.HeavyAnalytics.StopsWithPlaytestingAndDropsTheUnfinishedWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestHeavyAnalyticsStopsWithPlaytestingTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings FlockAnalytics(true);

	// The game instance shutting down.
	{
		FPlaytestFixture Fixture;
		StartSignedIn(Fixture, /*bHeavyAnalytics*/ true);
		Play(Fixture, 65.0);
		TestEqual(TEXT("Precondition: six windows, with five seconds unfinished"), WindowsSent(Fixture), 6);

		Fixture.Playtest->Deinitialize();
		TestFalse(TEXT("Shutting down stops the measuring"), Fixture.Playtest->IsMeasuringPerformance());
		TestFalse(TEXT("And the following of level loads"), FollowsAnyLevelLoads(Fixture.Playtest));
		Play(Fixture, 20.0);
		TestEqual(TEXT("The unfinished window is dropped, and nothing more is sent"), WindowsSent(Fixture), 6);
	}

	// The Flock SDK shutting down, then initializing again.
	{
		FPlaytestFixture Fixture;
		StartSignedIn(Fixture, /*bHeavyAnalytics*/ true);
		Play(Fixture, 15.0);
		TestEqual(TEXT("Precondition: one window, with five seconds unfinished"), WindowsSent(Fixture), 1);
		Fixture.Flock->ShutdownSdk();
		TestFalse(TEXT("The Flock SDK shutting down stops the measuring"), Fixture.Playtest->IsMeasuringPerformance());

		Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
		ExpectPlaytestStatus(*this, TEXT("Ready again"), Fixture.Playtest->GetStatus(), EFlockPlaytestStatus::Ready);
		TestTrue(TEXT("Measuring starts again"), Fixture.Playtest->IsMeasuringPerformance());
		Play(Fixture, 5.0);
		TestEqual(TEXT("The window the shutdown cut short is not finished by later play"), WindowsSent(Fixture), 1);
		Play(Fixture, 5.0);
		TestEqual(TEXT("Ten seconds since the restart make a window"), WindowsSent(Fixture), 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestHeavyAnalyticsLevelLoadsOfItsOwnGameInstanceTest,
	"Flock.Playtest.HeavyAnalytics.LevelLoadsOfItsOwnGameInstance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestHeavyAnalyticsLevelLoadsOfItsOwnGameInstanceTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings FlockAnalytics(true);
	FPlaytestFixture Fixture;
	StartSignedIn(Fixture, /*bHeavyAnalytics*/ true);
	TestTrue(TEXT("Level loads are followed"), FollowsBothLevelLoadEvents(Fixture.Playtest));

	FWorldContext Context;
	Context.OwningGameInstance = Fixture.GameInstance;
	const FScopedTestWorld Arena(TEXT("Arena"), Fixture.GameInstance);
	const FScopedTestWorld Harbor(TEXT("Harbor"), Fixture.GameInstance);
	UGameInstance* OtherGameInstance = NewObject<UGameInstance>(GetTransientPackage());
	FWorldContext OtherContext;
	OtherContext.OwningGameInstance = OtherGameInstance;
	const FScopedTestWorld OtherMap(TEXT("AnotherGameInstancesMap"), OtherGameInstance);

	// A load that holds the game up, in the order the engine runs a frame: the engine's update loads the map, then the
	// same engine frame hands over a frame time measured before the load, and the next engine frame's time carries it.
	Play(Fixture, 9.0);
	++*Fixture.EngineFrameNumber;
	Fixture.Playtest->HandlePreLoadMapForTesting(Context, TEXT("/Game/Maps/Arena"));
	Fixture.Playtest->HandlePostLoadMapForTesting(Arena.World);
	Fixture.Playtest->TickPerformanceTimelineForTesting(1.f / 60.f);
	TickInANewEngineFrame(Fixture, 30.f);
	Play(Fixture, 59.0 / 60.0);

	// Another game instance's load, as when several play side by side in the editor.
	Fixture.Playtest->HandlePreLoadMapForTesting(OtherContext, TEXT("/Game/Maps/AnotherGameInstancesMap"));
	Fixture.Playtest->HandlePostLoadMapForTesting(OtherMap.World);

	// A seamless travel: the map changes while frames keep coming, so the frames that follow are play. It held nothing
	// up, whatever loads came before it.
	++*Fixture.EngineFrameNumber;
	Fixture.Playtest->HandlePostLoadMapForTesting(Harbor.World);
	Fixture.Playtest->TickPerformanceTimelineForTesting(5.f);
	TickInANewEngineFrame(Fixture, 5.f);

	// A load of its own that fails: the engine names no world, and the frame time it stretched is still not play.
	++*Fixture.EngineFrameNumber;
	Fixture.Playtest->HandlePreLoadMapForTesting(Context, TEXT("/Game/Maps/Missing"));
	Fixture.Playtest->HandlePostLoadMapForTesting(nullptr);
	TickInANewEngineFrame(Fixture, 20.f);
	Play(Fixture, 10.0);

	// Back to the first map by seamless travel: a failed load leaves no start behind either.
	++*Fixture.EngineFrameNumber;
	Fixture.Playtest->HandlePostLoadMapForTesting(Arena.World);

	const TArray<TSharedPtr<FJsonObject>> Events = Fixture.SentPlaytestEvents();
	const TArray<TSharedPtr<FJsonObject>> Levels = EventsNamed(Events, FlockPlaytestEvents::LevelLoaded);
	if (TestEqual(TEXT("Three level loads of its own, none for another game instance and none for the load that failed"), Levels.Num(), 3))
	{
		const TSharedPtr<FJsonObject> First = PropertiesOf(Levels[0]);
		TestEqual(TEXT("The first names the loaded world's map"), StringMember(First, TEXT("map")), FString(TEXT("Arena")));
		TestFalse(TEXT("With no map before it"), First->HasField(TEXT("previous_map")));
		TestTrue(TEXT("And how long it held the game up"), NumberMember(First, TEXT("load_seconds")) >= 0.0);

		const TSharedPtr<FJsonObject> Second = PropertiesOf(Levels[1]);
		TestEqual(TEXT("The second names its map"), StringMember(Second, TEXT("map")), FString(TEXT("Harbor")));
		TestEqual(TEXT("And the map before it"), StringMember(Second, TEXT("previous_map")), FString(TEXT("Arena")));
		TestFalse(TEXT("A seamless travel after a load, and after another game instance's load start, held nothing up"),
			Second->HasField(TEXT("load_seconds")));

		const TSharedPtr<FJsonObject> Third = PropertiesOf(Levels[2]);
		TestEqual(TEXT("The third names its map"), StringMember(Third, TEXT("map")), FString(TEXT("Arena")));
		TestEqual(TEXT("After the map the failed load did not change"), StringMember(Third, TEXT("previous_map")), FString(TEXT("Harbor")));
		TestFalse(TEXT("A seamless travel after a failed load held nothing up"), Third->HasField(TEXT("load_seconds")));
	}

	const TArray<TSharedPtr<FJsonObject>> Windows = EventsNamed(Events, FlockPlaytestEvents::PerformanceWindow);
	if (TestEqual(TEXT("Three windows"), Windows.Num(), 3))
	{
		const TSharedPtr<FJsonObject> First = PropertiesOf(Windows[0]);
		const double Seconds = NumberMember(First, TEXT("window_seconds"));
		TestTrue(TEXT("The frame time that carried the load is not in the first window"), Seconds >= 10.0 && Seconds <= 10.05);
		TestEqual(TEXT("The loading engine frame's own time is"), NumberMember(First, TEXT("frames")), 600.0);
		TestEqual(TEXT("On the map loaded during it"), StringMember(First, TEXT("map")), FString(TEXT("Arena")));

		const TSharedPtr<FJsonObject> Second = PropertiesOf(Windows[1]);
		TestEqual(TEXT("Both frames after a seamless travel count, and another game instance's load leaves none out"),
			NumberMember(Second, TEXT("frames")), 2.0);
		TestEqual(TEXT("On the map travelled to"), StringMember(Second, TEXT("map")), FString(TEXT("Harbor")));

		const TSharedPtr<FJsonObject> Third = PropertiesOf(Windows[2]);
		TestEqual(TEXT("The frame time the failed load stretched is not in the third window"), NumberMember(Third, TEXT("frames")), 600.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestHeavyAnalyticsRecordPlaytestEventTest,
	"Flock.Playtest.HeavyAnalytics.RecordPlaytestEvent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestHeavyAnalyticsRecordPlaytestEventTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings FlockAnalytics(true);
	FPlaytestLogCapture Log;
	FPlaytestFixture Fixture;
	StartSignedIn(Fixture, /*bHeavyAnalytics*/ true);

	FFlockCommandData Properties;
	Properties.Set(TEXT("boss"), TEXT("Kraken")).Set(TEXT("attempts"), 3);
	TestTrue(TEXT("The game's own event is recorded"), Fixture.Playtest->RecordPlaytestEvent(TEXT("boss_defeated"), Properties));

	TestFalse(TEXT("performance_window is the plugin's own"), Fixture.Playtest->RecordPlaytestEvent(FlockPlaytestEvents::PerformanceWindow));
	TestFalse(TEXT("level_loaded is the plugin's own"), Fixture.Playtest->RecordPlaytestEvent(FlockPlaytestEvents::LevelLoaded));
	const TArray<FPlaytestLogCapture::FLine> Refusals = Log.LinesContaining(TEXT("the plugin sends events with that name itself"));
	TestEqual(TEXT("Each refusal is logged"), Refusals.Num(), 2);
	for (const FPlaytestLogCapture::FLine& Line : Refusals)
	{
		TestEqual(TEXT("As a warning"), static_cast<int32>(Line.Verbosity), static_cast<int32>(ELogVerbosity::Warning));
	}
	TestTrue(TEXT("A name differing only in letter case is the game's"), Fixture.Playtest->RecordPlaytestEvent(TEXT("Level_Loaded")));
	TestFalse(TEXT("A name the Flock SDK cannot store is refused"),
		Fixture.Playtest->RecordPlaytestEvent(FString::ChrN(FFlockAnalyticsProvider::MaxEventNameLength + 1, TEXT('n'))));
	TestFalse(TEXT("The Blueprint node does nothing without a playtest subsystem"),
		UFlockPlaytestLibrary::RecordPlaytestEvent(nullptr, TEXT("boss_defeated"), Properties));

	const TArray<TSharedPtr<FJsonObject>> Events = Fixture.SentPlaytestEvents();
	const TArray<TSharedPtr<FJsonObject>> Boss = EventsNamed(Events, TEXT("boss_defeated"));
	if (TestEqual(TEXT("The game's event is sent once"), Boss.Num(), 1))
	{
		TestEqual(TEXT("For the signed-in player"), StringMember(Boss[0], TEXT("player_id")), FString(FlockPlayerId));
		const TSharedPtr<FJsonObject> Sent = PropertiesOf(Boss[0]);
		TestEqual(TEXT("With its text property"), StringMember(Sent, TEXT("boss")), FString(TEXT("Kraken")));
		TestEqual(TEXT("And its number property, still a number"), NumberMember(Sent, TEXT("attempts")), 3.0);
	}
	TestEqual(TEXT("The name differing in case is sent too"), EventsNamed(Events, TEXT("Level_Loaded")).Num(), 1);
	TestEqual(TEXT("And nothing else"), Events.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestHeavyAnalyticsNeedsTheFlockSdksAnalyticsTest,
	"Flock.Playtest.HeavyAnalytics.NeedsTheFlockSdksAnalytics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestHeavyAnalyticsNeedsTheFlockSdksAnalyticsTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings FlockAnalytics(false);
	FPlaytestLogCapture Log;
	FPlaytestFixture Fixture;
	StartSignedIn(Fixture, /*bHeavyAnalytics*/ true);
	ExpectPlaytestStatus(*this, TEXT("Ready"), Fixture.Playtest->GetStatus(), EFlockPlaytestStatus::Ready);
	TestNull(TEXT("Precondition: the Flock SDK's analytics is off"), Fixture.Flock->GetAnalyticsProvider());

	TestFalse(TEXT("Nothing is measured"), Fixture.Playtest->IsMeasuringPerformance());
	TestFalse(TEXT("No level load is followed"), FollowsAnyLevelLoads(Fixture.Playtest));
	TestFalse(TEXT("The game's own playtest event is not recorded"), Fixture.Playtest->RecordPlaytestEvent(TEXT("boss_defeated")));

	Fixture.Flock->ShutdownSdk();
	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
	ExpectPlaytestStatus(*this, TEXT("Ready again"), Fixture.Playtest->GetStatus(), EFlockPlaytestStatus::Ready);

	const TArray<FPlaytestLogCapture::FLine> Lines = Log.LinesContaining(TEXT("but the Flock SDK's analytics is off"));
	if (TestEqual(TEXT("It is logged once per launch"), Lines.Num(), 1))
	{
		TestEqual(TEXT("As a warning"), static_cast<int32>(Lines[0].Verbosity), static_cast<int32>(ELogVerbosity::Warning));
		TestTrue(TEXT("Naming the setting to turn on"), Lines[0].Message.Contains(TEXT("Analytics Enabled"), ESearchCase::CaseSensitive));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestHeavyAnalyticsReadsTheEngineFrameCounterTest,
	"Flock.Playtest.HeavyAnalytics.ReadsTheEngineFrameCounter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestHeavyAnalyticsReadsTheEngineFrameCounterTest::RunTest(const FString& Parameters)
{
	// Every other heavy analytics test hands the subsystem its engine frame numbers. This is the one check that a
	// subsystem left to itself reads the engine's own frame counter.
	UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
	const UFlockPlaytestSubsystem* Playtest = NewObject<UFlockPlaytestSubsystem>(GameInstance);
	TestTrue(TEXT("Precondition: the engine has counted frames"), GFrameCounter > 0);
	TestEqual(TEXT("It reads the engine's frame counter"), static_cast<int64>(Playtest->GetEngineFrameNumberForTesting()),
		static_cast<int64>(GFrameCounter));
	return true;
}

#endif
