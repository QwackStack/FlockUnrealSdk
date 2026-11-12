// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"
#include "Misc/FlockEngineCompat.h"

#if WITH_AUTOMATION_TESTS

#include "Tests/Support/FlockAnalyticsProviderFixture.h"

#include "Analytics/FlockDefaultEvents.h"
#include "GeneralProjectSettings.h"
#include "Misc/ScopeExit.h"

// The SDK's own analytics events (session_end, app_open, first_launch, app_update, screen_view) and the flock_ parameters every
// analytics event carries. store_purchase is tested with the store, in FlockStoreTest.cpp.

#define FLOCK_DEFAULT_EVENTS_TEST(TestClass, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(TestClass, "Flock.Analytics.DefaultEvents." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

namespace FlockDefaultEventsTestHelpers
{
	using namespace FlockAnalyticsProviderTestHelpers;

	/** The spooled events with exactly this name, oldest first. */
	inline TArray<FFlockSpooledAnalyticsEvent> Named(const FFixture& Fix, const FString& Name)
	{
		TArray<FFlockSpooledAnalyticsEvent> Out;
		for (const FFlockSpooledAnalyticsEvent& Entry : Fix.SpooledEvents())
		{
			if (Entry.Event.EventName.Equals(Name, ESearchCase::CaseSensitive))
			{
				Out.Add(Entry);
			}
		}
		return Out;
	}

	inline TArray<FString> NamesOf(const TArray<FFlockSpooledAnalyticsEvent>& Entries)
	{
		TArray<FString> Out;
		for (const FFlockSpooledAnalyticsEvent& Entry : Entries)
		{
			Out.Add(Entry.Event.EventName);
		}
		return Out;
	}

	/** The properties object of a sent event. */
	inline TSharedPtr<FJsonObject> PropertiesOf(const TSharedPtr<FJsonObject>& Event)
	{
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		return Event.IsValid() && Event->TryGetObjectField(TEXT("properties"), Properties) ? *Properties : MakeShared<FJsonObject>();
	}

	inline TSharedPtr<FJsonObject> Props(const FFlockSpooledAnalyticsEvent& Entry)
	{
		return Entry.Event.Properties.ToJsonObject();
	}

	/** A number property under exactly this spelling, or -1. */
	inline double Number(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		double Value = -1.0;
		return FlockTestSpelling::HasMemberSpelled(Object, Key) && Object->TryGetNumberField(Key, Value) ? Value : -1.0;
	}

	/** A bool property under exactly this spelling; unset when absent or not a bool. */
	inline TOptional<bool> Bool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		bool Value = false;
		return FlockTestSpelling::HasMemberSpelled(Object, Key) && Object->TryGetBoolField(Key, Value) ? TOptional<bool>(Value) : TOptional<bool>();
	}

	inline bool Has(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		return FlockTestSpelling::HasMemberSpelled(Object, Key);
	}

	/** The open session record a launch that ended without ending its session leaves behind. */
	inline FFlockSessionSnapshot OpenSession(const FString& SessionId, float DurationSeconds, int32 SessionNumber = 3)
	{
		FFlockSessionSnapshot Snapshot;
		Snapshot.SessionId = SessionId;
		Snapshot.PlayerId = TEXT("player-ended");
		Snapshot.SessionNumber = SessionNumber;
		Snapshot.StartTimeUtc = TEXT("2026-11-12T10:00:00.000Z");
		Snapshot.LastHeartbeatUtc = TEXT("2026-11-12T10:01:00.000Z");
		Snapshot.DurationSeconds = DurationSeconds;
		Snapshot.IsActive = true;
		return Snapshot;
	}

	/** Leaves Snapshot as the open session of a launch under Dir that has ended. */
	inline void PlantOpenSession(const FString& Dir, const FFlockSessionSnapshot& Snapshot, bool bEndEventRecorded = false)
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("session_number"), Snapshot.SessionNumber);
		const TSharedPtr<FJsonObject> Active = FFlockAnalyticsJson::SnapshotToJson(Snapshot);
		if (bEndEventRecorded)
		{
			Active->SetBoolField(TEXT("end_event_recorded"), true);
		}
		Root->SetObjectField(TEXT("active_session"), Active);
		FString Json;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(Root, Writer);
		FFixture::SaveInAnEndedLaunch(Dir, FFlockAnalyticsLaunches::SessionStateFileName, Json);
	}

	/** The requests of one method to a URL containing Fragment, in the order sent. */
	inline TArray<FFlockHttpRequest> RequestsTo(const FFixture& Fix, const TCHAR* Method, const FString& Fragment)
	{
		TArray<FFlockHttpRequest> Out;
		for (const FFlockHttpRequest& Request : Fix.Fake->Requests)
		{
			if (Request.Method == Method && Request.Url.Contains(Fragment))
			{
				Out.Add(Request);
			}
		}
		return Out;
	}

	inline int32 IndexOfRequest(const FFixture& Fix, const TCHAR* Method, const FString& Fragment, int32 Occurrence = 0)
	{
		int32 Seen = 0;
		for (int32 Index = 0; Index < Fix.Fake->Requests.Num(); ++Index)
		{
			const FFlockHttpRequest& Request = Fix.Fake->Requests[Index];
			if (Request.Method == Method && Request.Url.Contains(Fragment) && Seen++ == Occurrence)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	/** Builds a provider with a logger the test reads. */
	inline TSharedRef<FFlockRecordingLogger> RebuildWithLogger(FFixture& Fix, const FFlockAnalyticsConfig& Config)
	{
		const TSharedRef<FFlockRecordingLogger> Logger = MakeShared<FFlockRecordingLogger>();
		Fix.Provider = MakeShared<FFlockAnalyticsProvider>(Fix.Client, NoRetryPolicy(), Logger, Fix.Session, Fix.Events,
			TEXT("http://x/v1"), Fix.Same(Config), Fix.Deps, TEXT("gv-1"), TEXT("0.7.0"));
		return Logger;
	}

	/** What JsonString reads for a parameter the SDK adds only when the device has a value. */
	inline FString Expected(const FString& DeviceValue)
	{
		return DeviceValue.IsEmpty() ? FString(TEXT("<absent>")) : DeviceValue;
	}

	inline FString RecordPath(const FFixture& Fix)
	{
		return Fix.Deps.DefaultEventsRecordPath;
	}
}

using namespace FlockDefaultEventsTestHelpers;

// ── session_end ──

FLOCK_DEFAULT_EVENTS_TEST(FFlockSessionEndRecordedTest, "SessionEnd.AnEndedSessionRecordsSessionEndWithItsReasonLengthScreensAndBounce")
bool FFlockSessionEndRecordedTest::RunTest(const FString& Parameters)
{
	FFixture Fix(FFlockAnalyticsConfig(), FString(), /*bSendDefaultEvents*/ true);
	Fix.Provider->Initialize();
	Fix.Provider->StartSession(TEXT("player-a"));
	Fix.Provider->RecordScreenView(TEXT("menu"));
	Fix.Provider->RecordScreenView(TEXT("level-1"));
	Fix.Provider->EndSession(EFlockSessionEndReason::Manual);

	const TArray<FFlockSpooledAnalyticsEvent> Ends = Named(Fix, FlockDefaultEvents::SessionEnd);
	if (!TestEqual(TEXT("One session_end"), Ends.Num(), 1))
	{
		return false;
	}
	const FFlockSpooledAnalyticsEvent& End = Ends[0];
	TestEqualSensitive(TEXT("Category"), End.Event.EventCategory, FString(TEXT("session")));
	TestEqualSensitive(TEXT("The session's player"), End.Event.PlayerId, FString(TEXT("player-a")));
	TestEqualSensitive(TEXT("The server's id, so the event is tied to its session"), End.Event.SessionId, FString(TEXT("srv-1")));
	const TSharedPtr<FJsonObject> P = Props(End);
	TestEqualSensitive(TEXT("reason"), JsonString(P, TEXT("reason")), FString(TEXT("ended_by_the_game")));
	TestEqual(TEXT("duration_seconds in whole seconds"), Number(P, TEXT("duration_seconds")), 0.0);
	TestEqual(TEXT("screens_viewed"), Number(P, TEXT("screens_viewed")), 2.0);
	TestTrue(TEXT("A session shorter than the bounce threshold (10 s) is a bounce"), Bool(P, TEXT("is_bounce")).Get(false));
	TestEqual(TEXT("session_number"), Number(P, TEXT("session_number")), 1.0);
	TestTrue(TEXT("is_first_session"), Bool(P, TEXT("is_first_session")).Get(false));
	TestEqual(TEXT("pause_count"), Number(P, TEXT("pause_count")), 0.0);
	TestFalse(TEXT("No frame rate without a sample"), Has(P, TEXT("average_fps")) || Has(P, TEXT("min_fps")));
	TestEqual(TEXT("The ended session's number, not the next one's"), Number(P, TEXT("flock_session_number")), 1.0);
	TestEqualSensitive(TEXT("flock_sdk_version"), JsonString(P, TEXT("flock_sdk_version")), FString(TEXT("0.7.0")));
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockSessionEndFrameRateTest, "SessionEnd.ASampledFrameRateIsCarriedRoundedToOneDecimal")
bool FFlockSessionEndFrameRateTest::RunTest(const FString& Parameters)
{
	FFixture Fix(FFlockAnalyticsConfig(), FString(), /*bSendDefaultEvents*/ true);
	Fix.Provider->Initialize();
	Fix.Provider->StartSession(TEXT("player-a"));
	// Three frames in one second: 3 fps, the one sample.
	Fix.Provider->TickForTesting(0.4f);
	Fix.Provider->TickForTesting(0.3f);
	Fix.Provider->TickForTesting(0.3f);
	Fix.Provider->EndSession(EFlockSessionEndReason::Manual);

	const TArray<FFlockSpooledAnalyticsEvent> Ends = Named(Fix, FlockDefaultEvents::SessionEnd);
	const TSharedPtr<FJsonObject> P = Ends.Num() == 1 ? Props(Ends[0]) : MakeShared<FJsonObject>();
	TestEqual(TEXT("average_fps"), Number(P, TEXT("average_fps")), 3.0);
	TestEqual(TEXT("min_fps"), Number(P, TEXT("min_fps")), 3.0);
	TestEqual(TEXT("1 s, rounded"), Number(P, TEXT("duration_seconds")), 1.0);
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockSessionEndSignOutTest, "SessionEnd.ASignOutRecordsSignOutForThePlayerWhoseSessionItWas")
bool FFlockSessionEndSignOutTest::RunTest(const FString& Parameters)
{
	FFixture Fix(FFlockAnalyticsConfig(), FString(), /*bSendDefaultEvents*/ true);
	Fix.Provider->Initialize();
	Fix.Provider->StartSession(TEXT("player-a"));
	Fix.Session->ClearTokens();
	Fix.Provider->HandleLoggedOut();

	const TArray<FFlockSpooledAnalyticsEvent> Ends = Named(Fix, FlockDefaultEvents::SessionEnd);
	TestEqual(TEXT("One session_end"), Ends.Num(), 1);
	TestEqualSensitive(TEXT("sign_out"), Ends.Num() == 1 ? JsonString(Props(Ends[0]), TEXT("reason")) : FString(), FString(TEXT("sign_out")));
	TestEqualSensitive(TEXT("Credited to the session's player, though nobody is signed in now"),
		Ends.Num() == 1 ? Ends[0].Event.PlayerId : FString(), FString(TEXT("player-a")));
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockSessionEndPlayerChangedTest, "SessionEnd.AnotherPlayerSigningInRecordsPlayerChangedAndTheSamePlayerRestarted")
bool FFlockSessionEndPlayerChangedTest::RunTest(const FString& Parameters)
{
	FFixture Fix(FFlockAnalyticsConfig(), FString(), /*bSendDefaultEvents*/ true);
	Fix.Provider->Initialize();
	Fix.Provider->StartSession(TEXT("player-a"));
	Fix.Provider->StartSession(TEXT("player-b"));
	Fix.Provider->StartSession(TEXT("player-b"));
	// Letter case is part of an id: another player.
	Fix.Provider->StartSession(TEXT("Player-B"));

	const TArray<FFlockSpooledAnalyticsEvent> Ends = Named(Fix, FlockDefaultEvents::SessionEnd);
	if (!TestEqual(TEXT("Three ended"), Ends.Num(), 3))
	{
		return false;
	}
	TestEqualSensitive(TEXT("Another player: player_changed"), JsonString(Props(Ends[0]), TEXT("reason")), FString(TEXT("player_changed")));
	TestEqualSensitive(TEXT("Credited to the player who left"), Ends[0].Event.PlayerId, FString(TEXT("player-a")));
	TestEqualSensitive(TEXT("The same player: restarted"), JsonString(Props(Ends[1]), TEXT("reason")), FString(TEXT("restarted")));
	TestEqualSensitive(TEXT("Another spelling: player_changed"), JsonString(Props(Ends[2]), TEXT("reason")), FString(TEXT("player_changed")));
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockSessionEndOtherReasonsTest, "SessionEnd.TimeoutAndQuitAreNamed")
bool FFlockSessionEndOtherReasonsTest::RunTest(const FString& Parameters)
{
	FFixture Fix(FFlockAnalyticsConfig(), FString(), /*bSendDefaultEvents*/ true);
	Fix.Provider->Initialize();
	Fix.Provider->StartSession(TEXT("player-a"));
	Fix.Provider->EndSession(EFlockSessionEndReason::Timeout);
	Fix.Provider->StartSession(TEXT("player-a"));
	Fix.Provider->EndSession(EFlockSessionEndReason::Quit);
	const TArray<FFlockSpooledAnalyticsEvent> Ends = Named(Fix, FlockDefaultEvents::SessionEnd);
	TestEqual(TEXT("Two ended"), Ends.Num(), 2);
	TestEqualSensitive(TEXT("timeout"), Ends.Num() == 2 ? JsonString(Props(Ends[0]), TEXT("reason")) : FString(), FString(TEXT("timeout")));
	TestEqualSensitive(TEXT("quit"), Ends.Num() == 2 ? JsonString(Props(Ends[1]), TEXT("reason")) : FString(), FString(TEXT("quit")));
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockSessionEndConsentTest, "SessionEnd.WithoutConsentNothingIsRecorded")
bool FFlockSessionEndConsentTest::RunTest(const FString& Parameters)
{
	FFixture Owner;
	PlantOpenSession(Owner.Dir, OpenSession(TEXT("s-ended"), 30.f));
	FFlockAnalyticsConfig Config;
	Config.bRequireExplicitConsent = true;
	FFixture Fix(Config, Owner.Dir, /*bSendDefaultEvents*/ true);
	Fix.Provider->Initialize();
	Fix.Provider->StartSession(TEXT("player-a"));
	Fix.Provider->EndSession(EFlockSessionEndReason::Manual);
	TestEqual(TEXT("No event, for the open session or the refused one"), Fix.Provider->GetPendingAnalyticsEventCount(), 0);
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockSessionEndUnsavedKeptTest, "SessionEnd.AnEndTheQueueCouldNotSaveIsRecordedOnceAndItsKeptRecordSaysSo")
bool FFlockSessionEndUnsavedKeptTest::RunTest(const FString& Parameters)
{
	FFixture Fix(FFlockAnalyticsConfig(), FString(), /*bSendDefaultEvents*/ true);
	Fix.Provider->Initialize();
	Fix.Provider->StartSession(TEXT("player-a"));
	// The end cannot be saved, and the server cannot be reached to end it now.
	Fix.EndCache->bFailWrites = true;
	Fix.OnClose(TEXT("srv-1"), FFlockFakeTransport::Offline());
	Fix.Provider->EndSession(EFlockSessionEndReason::Manual);

	TestEqual(TEXT("Recorded once"), Named(Fix, FlockDefaultEvents::SessionEnd).Num(), 1);
	const FFlockSession Reader(FFlockAnalyticsConfig(), Fix.Deps.Launches->GetSessionStatePath());
	FFlockSessionSnapshot Kept;
	bool bRecorded = false;
	TestTrue(TEXT("Its record is kept for the next launch, which ends it on the server"), Reader.RecoverOrphanedSession(Kept, &bRecorded));
	TestTrue(TEXT("So the next launch records no second event"), bRecorded);

	// The next launch: the end is queued, with no second event.
	Fix.EndLaunchWithoutShuttingDown();
	FFixture Next(FFlockAnalyticsConfig(), Fix.Dir, /*bSendDefaultEvents*/ true);
	Next.Provider->Initialize();
	TestEqual(TEXT("The next launch queues the end"), Next.EndCache->PendingCount(), 1);
	TestEqual(TEXT("Without a second session_end"), Named(Next, FlockDefaultEvents::SessionEnd).Num(), 0);
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockSessionEndUnsavedSentFirstTest, "SessionEnd.AnEndTheQueueCouldNotSaveIsSentBeforeTheNextSessionReplacesItsRecord")
bool FFlockSessionEndUnsavedSentFirstTest::RunTest(const FString& Parameters)
{
	for (const bool bRegistered : { true, false })
	{
		const FString Label = bRegistered ? TEXT("Registered") : TEXT("Never registered");
		FFixture Fix(FFlockAnalyticsConfig(), FString(), /*bSendDefaultEvents*/ true);
		Fix.Provider->Initialize();
		if (!bRegistered)
		{
			Fix.OnRegistration(FFlockFakeTransport::Offline());
		}
		Fix.Provider->StartSession(TEXT("player-a"));
		Fix.EndCache->bFailWrites = true;
		Fix.OnClose(TEXT("srv-1"), FFlockFakeTransport::Offline());
		Fix.Provider->EndSession(EFlockSessionEndReason::Manual);
		const int32 ClosesBefore = RequestsTo(Fix, TEXT("PATCH"), TEXT("analytics/sessions/srv-1")).Num();

		// The server answers again; the next session starts.
		Fix.OnRegistration(FFlockFakeTransport::Ok(TEXT("{\"session_id\":\"srv-1\"}")));
		Fix.OnClose(TEXT("srv-1"), FFlockFakeTransport::Ok(TEXT("{}")));
		const int32 RequestsBefore = Fix.Fake->Requests.Num();
		Fix.Provider->StartSession(TEXT("player-a"));

		TestEqual(Label + TEXT(": the kept end is ended on the server"),
			RequestsTo(Fix, TEXT("PATCH"), TEXT("analytics/sessions/srv-1")).Num(), ClosesBefore + 1);
		const int32 Close = IndexOfRequest(Fix, TEXT("PATCH"), TEXT("analytics/sessions/srv-1"), ClosesBefore);
		// The new session's registration is the last registration sent.
		const TArray<FFlockHttpRequest> Registrations = RequestsTo(Fix, TEXT("POST"), TEXT("analytics/sessions"));
		const int32 NewRegistration = IndexOfRequest(Fix, TEXT("POST"), TEXT("analytics/sessions"), Registrations.Num() - 1);
		TestTrue(Label + TEXT(": before the next session registered"), Close != INDEX_NONE && Close >= RequestsBefore && Close < NewRegistration);
	}
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockSessionEndRecoveredMarkedTest, "SessionEnd.ASessionAnEndedLaunchLeftOpenWhoseEventWasRecordedIsEndedWithoutASecondEvent")
bool FFlockSessionEndRecoveredMarkedTest::RunTest(const FString& Parameters)
{
	FFixture Owner;
	PlantOpenSession(Owner.Dir, OpenSession(TEXT("s-recorded"), 30.f), /*bEndEventRecorded*/ true);
	FFixture Fix(FFlockAnalyticsConfig(), Owner.Dir, /*bSendDefaultEvents*/ true);
	Fix.Provider->Initialize();
	TestEqual(TEXT("No second session_end"), Named(Fix, FlockDefaultEvents::SessionEnd).Num(), 0);
	TestEqual(TEXT("The session itself is still ended on the server"), Fix.EndCache->PendingCount(), 1);
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockSessionEndRecoveredTest, "SessionEnd.ASessionAnEndedLaunchLeftOpenIsRecordedAsAnUnexpectedExitWithItsBounceJudged")
bool FFlockSessionEndRecoveredTest::RunTest(const FString& Parameters)
{
	for (const bool bQueue : { true, false })
	{
		const FString Label = bQueue ? TEXT("With the queue") : TEXT("With Analytics Cache Failed Events off");
		FFixture Owner;
		PlantOpenSession(Owner.Dir, OpenSession(TEXT("s-short"), 3.f));
		PlantOpenSession(Owner.Dir, OpenSession(TEXT("s-long"), 50.f));
		FFlockAnalyticsConfig Config;
		Config.bCacheFailedEvents = bQueue;
		FFixture Fix(Config, Owner.Dir, /*bSendDefaultEvents*/ true);
		Fix.Provider->Initialize();

		const TArray<FFlockSpooledAnalyticsEvent> Ends = Named(Fix, FlockDefaultEvents::SessionEnd);
		if (!TestEqual(Label + TEXT(": each open session once"), Ends.Num(), 2))
		{
			continue;
		}
		for (const FFlockSpooledAnalyticsEvent& End : Ends)
		{
			const TSharedPtr<FJsonObject> P = Props(End);
			TestEqualSensitive(Label + TEXT(": unexpected_exit"), JsonString(P, TEXT("reason")), FString(TEXT("unexpected_exit")));
			TestEqualSensitive(Label + TEXT(": the ended launch's player"), End.Event.PlayerId, FString(TEXT("player-ended")));
			const bool bShort = Number(P, TEXT("duration_seconds")) == 3.0;
			TestEqual(Label + TEXT(": the bounce judged on the saved length"), Bool(P, TEXT("is_bounce")).Get(!bShort), bShort);
		}

		if (bQueue)
		{
			// The session record carries the bounce too, which used to be whatever the last save held.
			int32 Bounces = 0;
			TArray<FString> Handles;
			TArray<FString> Payloads;
			Fix.EndCache->PeekBatch(10, Handles, Payloads);
			for (const FString& Payload : Payloads)
			{
				FFlockSessionSnapshot Spooled;
				if (FFlockAnalyticsJson::DeserializeSnapshot(Payload, Spooled) && Spooled.IsBounce)
				{
					++Bounces;
				}
			}
			TestEqual(Label + TEXT(": one queued end is a bounce"), Bounces, 1);
			continue;
		}

		// Nothing is queued with the setting off: both ends go out at this launch's first session, and the ended launches are
		// let go of, where they used to be kept and tried again at every launch.
		TestEqual(Label + TEXT(": nothing queued"), Fix.EndCache->PendingCount(), 0);
		Fix.Provider->StartSession(TEXT("player-a"));
		TestEqual(Label + TEXT(": both ended on the server at the first session"),
			RequestsTo(Fix, TEXT("PATCH"), TEXT("analytics/sessions/srv-1")).Num(), 2);
		Fix.Provider->Shutdown();
		Fix.EndLaunchWithoutShuttingDown();
		FFixture Next(Config, Owner.Dir, /*bSendDefaultEvents*/ true);
		Next.Provider->Initialize();
		TestEqual(Label + TEXT(": a later launch finds neither again"), Named(Next, FlockDefaultEvents::SessionEnd).Num(), 0);
	}
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockSessionEndRecoveredVersionsTest, "SessionEnd.ASessionRecoveredAfterAnUpdateCarriesTheVersionsItRanOn")
bool FFlockSessionEndRecoveredVersionsTest::RunTest(const FString& Parameters)
{
	FFixture Owner;
	FFlockSessionSnapshot Older = OpenSession(TEXT("s-old"), 40.f, /*SessionNumber*/ 7);
	Older.DeviceInfo.Platform = TEXT("Android");
	Older.DeviceInfo.AppVersion = TEXT("0.9.0");
	Older.DeviceInfo.SdkVersion = TEXT("1.90.0");
	PlantOpenSession(Owner.Dir, Older);
	FFixture Fix(FFlockAnalyticsConfig(), Owner.Dir, /*bSendDefaultEvents*/ true);
	Fix.Provider->Initialize();

	const TArray<FFlockSpooledAnalyticsEvent> Ends = Named(Fix, FlockDefaultEvents::SessionEnd);
	const TSharedPtr<FJsonObject> P = Ends.Num() == 1 ? Props(Ends[0]) : MakeShared<FJsonObject>();
	TestEqualSensitive(TEXT("unexpected_exit"), JsonString(P, TEXT("reason")), FString(TEXT("unexpected_exit")));
	TestEqualSensitive(TEXT("The build the session ran on, not this launch's"), JsonString(P, TEXT("flock_app_version")), FString(TEXT("0.9.0")));
	TestEqualSensitive(TEXT("Its SDK"), JsonString(P, TEXT("flock_sdk_version")), FString(TEXT("1.90.0")));
	TestEqualSensitive(TEXT("Its platform"), JsonString(P, TEXT("flock_platform")), FString(TEXT("Android")));
	TestEqual(TEXT("Its session number"), Number(P, TEXT("flock_session_number")), 7.0);

	// A record from a build that kept no device has this launch's.
	FFixture Bare;
	PlantOpenSession(Bare.Dir, OpenSession(TEXT("s-bare"), 40.f));
	FFixture Later(FFlockAnalyticsConfig(), Bare.Dir, /*bSendDefaultEvents*/ true);
	Later.Provider->Initialize();
	const TArray<FFlockSpooledAnalyticsEvent> BareEnds = Named(Later, FlockDefaultEvents::SessionEnd);
	TestEqualSensitive(TEXT("This launch's SDK for a record with no device"),
		BareEnds.Num() == 1 ? JsonString(Props(BareEnds[0]), TEXT("flock_sdk_version")) : FString(), FString(TEXT("0.7.0")));
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockSessionEndNoQueueTest, "SessionEnd.WithTheQueuesOffAnEndWaitsForTheSignInAndGoesOutOnce")
bool FFlockSessionEndNoQueueTest::RunTest(const FString& Parameters)
{
	FFixture Owner;
	PlantOpenSession(Owner.Dir, OpenSession(TEXT("s-ended"), 30.f));
	// Analytics Cache Failed Events off: every queue keeps nothing, as the SDK builds them then. Nobody is signed in at start,
	// and Analytics Auto Start Session is off, so no session start delivers anything.
	FFlockAnalyticsConfig Config;
	Config.bCacheFailedEvents = false;
	Config.MaxCachedEvents = 0;
	Config.bAutoStartSession = false;
	FFixture Fix(Config, Owner.Dir, /*bSendDefaultEvents*/ true);
	Fix.Fake->On(TEXT("analytics/events"), FFlockFakeTransport::Ok(TEXT("{}")));
	Fix.Session->ClearTokens();
	Fix.Provider->Initialize();
	TestEqual(TEXT("Nothing is sent with nobody signed in: the server would refuse it"), Fix.Fake->Requests.Num(), 0);

	// A session the game runs and that ends with nobody signed in any more (a sign-in the server would not refresh).
	FString TokenError;
	Fix.Session->SetTokens(MakeTestJwt(TEXT("player-a")), TEXT("r-a"), TokenError);
	Fix.Provider->StartSession(TEXT("player-a"));
	const int32 RequestsBefore = Fix.Fake->Requests.Num();
	Fix.Session->ClearTokens();
	Fix.Provider->HandleLoggedOut();
	TestEqual(TEXT("Its end waits too"), Fix.Fake->Requests.Num(), RequestsBefore);

	Fix.Session->SetTokens(MakeTestJwt(TEXT("player-b")), TEXT("r-b"), TokenError);
	Fix.Provider->HandleAuthenticated(TEXT("player-b"));

	TArray<FString> EndsFor;
	for (const TArray<TSharedPtr<FJsonObject>>& Batch : Fix.SentEventBatches())
	{
		for (const TSharedPtr<FJsonObject>& Event : Batch)
		{
			if (JsonString(Event, TEXT("event_name")) == TEXT("session_end"))
			{
				EndsFor.Add(JsonString(Event, TEXT("player_id")) + TEXT(":") + JsonString(PropertiesOf(Event), TEXT("reason")));
			}
		}
	}
	EndsFor.Sort();
	TestEqual(TEXT("Each session_end goes out at the sign-in, credited to its own session's player"), EndsFor,
		TArray<FString>{ TEXT("player-a:sign_out"), TEXT("player-ended:unexpected_exit") });
	TestEqual(TEXT("Both sessions are ended on the server"), RequestsTo(Fix, TEXT("PATCH"), TEXT("analytics/sessions/srv-1")).Num(), 2);

	// Delivered once: a session start right after the sign-in sends neither again.
	Fix.Provider->StartSession(TEXT("player-b"));
	TestEqual(TEXT("Not again at the next session start"), RequestsTo(Fix, TEXT("PATCH"), TEXT("analytics/sessions/srv-1")).Num(), 2);
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockSessionEndRevokedTest, "SessionEnd.ConsentWithdrawnDuringASessionRecordsNoSessionEnd")
bool FFlockSessionEndRevokedTest::RunTest(const FString& Parameters)
{
	FFixture Fix(FFlockAnalyticsConfig(), FString(), /*bSendDefaultEvents*/ true);
	Fix.Provider->Initialize();
	Fix.Provider->StartSession(TEXT("player-a"));
	Fix.Provider->SetConsent(false);
	TestFalse(TEXT("Precondition: the session is gone"), Fix.Provider->HasActiveSession());
	TestEqual(TEXT("No session_end, and nothing else kept"), Fix.Provider->GetPendingAnalyticsEventCount(), 0);
	Fix.Provider->SetConsent(true);
	TestEqual(TEXT("Not recorded later either"), Named(Fix, FlockDefaultEvents::SessionEnd).Num(), 0);
	return true;
}

namespace FlockDefaultEventsTestHelpers
{
	/** The record kept for the next launch, read as that launch reads it; false when none is kept. */
	inline bool KeptRecord(const FFixture& Fix, FFlockSessionSnapshot& OutKept, bool* bOutRecorded = nullptr)
	{
		const FFlockSession Reader(FFlockAnalyticsConfig(), Fix.Deps.Launches->GetSessionStatePath());
		return Reader.RecoverOrphanedSession(OutKept, bOutRecorded);
	}

	/** Ends a registered session whose end the queue cannot save, with the server out of reach, so its record is kept. */
	inline void EndKeptForTheNextLaunch(FFixture& Fix)
	{
		Fix.EndCache->bFailWrites = true;
		Fix.OnClose(TEXT("srv-1"), FFlockFakeTransport::Offline());
		Fix.Provider->EndSession(EFlockSessionEndReason::Manual);
	}
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockSessionEndKeptRecordTest, "SessionEnd.TheRecordKeptForAnUnsavedEndHoldsItAsItEnded")
bool FFlockSessionEndKeptRecordTest::RunTest(const FString& Parameters)
{
	// The SDK's own events off: the record is kept all the same, and as the session ended, not as its last save left it.
	FFixture Fix;
	Fix.Provider->Initialize();
	Fix.Provider->StartSession(TEXT("player-a"));
	// Past the bounce threshold, so the record was last saved here; the screen after it is only in the end.
	Fix.Provider->TickForTesting(45.f);
	Fix.Provider->RecordScreenView(TEXT("menu"));
	EndKeptForTheNextLaunch(Fix);

	FFlockSessionSnapshot Kept;
	if (!TestTrue(TEXT("Kept"), KeptRecord(Fix, Kept)))
	{
		return false;
	}
	TestEqual(TEXT("Its whole length"), FlockDefaultEvents::WholeSeconds(Kept.DurationSeconds), 45);
	TestEqual(TEXT("Its screens"), Kept.ScreensViewed, 1);
	TestNotEqual(TEXT("Its own end time, not its start"), Kept.EndTimeUtc, Kept.StartTimeUtc);
	TestEqualSensitive(TEXT("Its server id"), Kept.ServerSessionId, FString(TEXT("srv-1")));
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockSessionEndKeptRecordClearedTest, "SessionEnd.AKeptRecordGoesOnceItsEndIsDeliveredOrConsentIsWithdrawn")
bool FFlockSessionEndKeptRecordClearedTest::RunTest(const FString& Parameters)
{
	{
		FFixture Fix(FFlockAnalyticsConfig(), FString(), /*bSendDefaultEvents*/ true);
		Fix.Provider->Initialize();
		Fix.Provider->StartSession(TEXT("player-a"));
		EndKeptForTheNextLaunch(Fix);
		FFlockSessionSnapshot Kept;
		TestTrue(TEXT("Precondition: kept"), KeptRecord(Fix, Kept));

		// The server is back; the next sign-in delivers the end in this launch.
		Fix.OnClose(TEXT("srv-1"), FFlockFakeTransport::Ok(TEXT("{}")));
		Fix.Provider->HandleAuthenticated(TEXT("p-fixture"));
		TestFalse(TEXT("Delivered in this launch: the next launch does not end it again"), KeptRecord(Fix, Kept));
	}
	{
		FFixture Fix(FFlockAnalyticsConfig(), FString(), /*bSendDefaultEvents*/ true);
		Fix.Provider->Initialize();
		Fix.Provider->StartSession(TEXT("player-a"));
		EndKeptForTheNextLaunch(Fix);
		Fix.Provider->SetConsent(false);
		FFlockSessionSnapshot Kept;
		TestFalse(TEXT("Consent withdrawn: the kept record goes with the rest"), KeptRecord(Fix, Kept));
	}
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockSessionEndRegisteredOnceTest, "SessionEnd.AnEndedSessionIsRegisteredOnceOnTheServer")
bool FFlockSessionEndRegisteredOnceTest::RunTest(const FString& Parameters)
{
	// With the queues off: the end's registration is answered and its close is not; the close is tried again under that id.
	{
		FFlockAnalyticsConfig Config;
		Config.bCacheFailedEvents = false;
		Config.MaxCachedEvents = 0;
		FFixture Fix(Config, FString(), /*bSendDefaultEvents*/ true);
		Fix.Fake->On(TEXT("analytics/events"), FFlockFakeTransport::Ok(TEXT("{}")));
		Fix.Provider->Initialize();
		Fix.OnRegistration(FFlockFakeTransport::Offline());
		Fix.Provider->StartSession(TEXT("player-a"));
		Fix.OnRegistration(FFlockFakeTransport::Ok(TEXT("{\"session_id\":\"srv-1\"}")));
		Fix.OnClose(TEXT("srv-1"), FFlockFakeTransport::Offline());
		Fix.Provider->EndSession(EFlockSessionEndReason::Manual);
		const int32 Registrations = RequestsTo(Fix, TEXT("POST"), TEXT("analytics/sessions")).Num();

		Fix.OnClose(TEXT("srv-1"), FFlockFakeTransport::Ok(TEXT("{}")));
		Fix.Provider->HandleAuthenticated(TEXT("p-fixture"));
		TestEqual(TEXT("Queues off: closed at the next sign-in"), RequestsTo(Fix, TEXT("PATCH"), TEXT("analytics/sessions/srv-1")).Num(), 2);
		TestEqual(TEXT("Queues off: not registered a second time"), RequestsTo(Fix, TEXT("POST"), TEXT("analytics/sessions")).Num(), Registrations);
	}
	// The session ends while its own registration is on its way: the answer gives its end the id.
	{
		FFixture Fix(FFlockAnalyticsConfig(), FString(), /*bSendDefaultEvents*/ true);
		Fix.Provider->Initialize();
		Fix.Fake->bDeferred = true;
		Fix.Provider->StartSession(TEXT("player-a"));
		Fix.Provider->EndSession(EFlockSessionEndReason::Manual);
		while (Fix.Fake->DeliverNext())
		{
		}
		TestEqual(TEXT("Registered once"), RequestsTo(Fix, TEXT("POST"), TEXT("analytics/sessions")).Num(), 1);
		TestEqual(TEXT("And closed under that id"), RequestsTo(Fix, TEXT("PATCH"), TEXT("analytics/sessions/srv-1")).Num(), 1);
		TestEqual(TEXT("Nothing left queued"), Fix.EndCache->PendingCount(), 0);
	}
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockWaitingEventsKeptTest, "EventsWaitingForASignInThatAlreadyEndedWaitForTheNextOne")
bool FFlockWaitingEventsKeptTest::RunTest(const FString& Parameters)
{
	FFlockAnalyticsConfig Config;
	Config.bCacheFailedEvents = false;
	Config.MaxCachedEvents = 0;
	FFixture Fix(Config, FString(), /*bSendDefaultEvents*/ true);
	Fix.Fake->On(TEXT("analytics/events"), FFlockFakeTransport::Ok(TEXT("{}")));
	Fix.Session->ClearTokens();
	Fix.Provider->Initialize();
	// Announced, but the sign-in already ended (a game's handler signed out first).
	Fix.Provider->HandleAuthenticated(TEXT("player-gone"));
	TestEqual(TEXT("Nothing sent"), Fix.Fake->CountTo(TEXT("analytics/events")), 0);

	FString TokenError;
	Fix.Session->SetTokens(MakeTestJwt(TEXT("player-a")), TEXT("r-a"), TokenError);
	Fix.Provider->HandleAuthenticated(TEXT("player-a"));
	TArray<FString> Sent;
	for (const TArray<TSharedPtr<FJsonObject>>& Batch : Fix.SentEventBatches())
	{
		for (const TSharedPtr<FJsonObject>& Event : Batch)
		{
			Sent.Add(JsonString(Event, TEXT("event_name")) + TEXT(":") + JsonString(Event, TEXT("player_id")));
		}
	}
	TestEqual(TEXT("Sent at the next sign-in, for that player"), Sent,
		TArray<FString>{ TEXT("app_open:player-a"), TEXT("first_launch:player-a") });
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockSessionEndRecoveredRoundedTest, "SessionEnd.ARecoveredSessionsLengthReachesTheServerRounded")
bool FFlockSessionEndRecoveredRoundedTest::RunTest(const FString& Parameters)
{
	FFixture Owner;
	PlantOpenSession(Owner.Dir, OpenSession(TEXT("s-rounded"), 3.6f));
	FFixture Fix(FFlockAnalyticsConfig(), Owner.Dir, /*bSendDefaultEvents*/ true);
	Fix.Provider->Initialize();
	Fix.Provider->StartSession(TEXT("player-a"));

	const TArray<FFlockHttpRequest> Closes = RequestsTo(Fix, TEXT("PATCH"), TEXT("analytics/sessions/srv-1"));
	TSharedPtr<FJsonObject> Body;
	const bool bRead = Closes.Num() > 0 && FFlockJsonUtils::TryParseObject(Closes[0].JsonBody, Body) && Body.IsValid();
	TestTrue(TEXT("The recovered session is ended on the server"), bRead);
	TestEqual(TEXT("3.6 s is 4, not 3"), bRead ? Number(Body, TEXT("duration_seconds")) : -1.0, 4.0);
	const TArray<FFlockSpooledAnalyticsEvent> Ends = Named(Fix, FlockDefaultEvents::SessionEnd);
	TestEqual(TEXT("And so is the event's"), Ends.Num() == 1 ? Number(Props(Ends[0]), TEXT("duration_seconds")) : -1.0, 4.0);
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockSessionSavedPastBounceTest, "SessionEnd.TheLiveRecordIsSavedOnceTheSessionOutlastsTheBounceThreshold")
bool FFlockSessionSavedPastBounceTest::RunTest(const FString& Parameters)
{
	const FString Path = FPaths::Combine(TempDir(), TEXT("session_state.json"));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Path), false, true); };
	FFlockAnalyticsConfig Config;
	Config.BounceThresholdSeconds = 1.f;
	Config.bTrackFps = false;
	FFlockSession Session(Config, Path);
	Session.Start(TEXT("player-1"));

	auto RecordIsBounce = [&Config, &Path]()
	{
		FFlockSessionSnapshot Saved;
		return FFlockSession(Config, Path).RecoverOrphanedSession(Saved) && Saved.IsBounce;
	};
	TestTrue(TEXT("Control: the record saved at the start is a bounce"), RecordIsBounce());
	Session.Tick(0.6f);
	TestTrue(TEXT("Control: not saved again before the threshold"), RecordIsBounce());
	Session.Tick(0.6f);
	TestFalse(TEXT("A crash after the threshold is not a bounce, though no heartbeat saved the record"), RecordIsBounce());
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockSessionBlankScreenTest, "SessionEnd.AScreenViewWithNoNameIsNeitherRecordedNorCountedAndTheDeveloperIsTold")
bool FFlockSessionBlankScreenTest::RunTest(const FString& Parameters)
{
	FFixture Fix(FFlockAnalyticsConfig(), FString(), /*bSendDefaultEvents*/ true);
	const TSharedRef<FFlockRecordingLogger> Logger = RebuildWithLogger(Fix, FFlockAnalyticsConfig());
	Fix.Provider->Initialize();
	Fix.Provider->StartSession(TEXT("player-a"));
	Fix.Provider->RecordScreenView(FString());
	Fix.Provider->RecordScreenView(TEXT(" \t "));

	TestEqual(TEXT("Not counted"), Fix.Provider->GetCurrentSnapshot().ScreensViewed, 0);
	TestEqual(TEXT("Not recorded"), Named(Fix, FlockDefaultEvents::ScreenView).Num(), 0);
	TestEqual(TEXT("The developer is told, each time"),
		Logger->Warnings.FilterByPredicate([](const FString& Line) { return Line.Contains(TEXT("a screen view needs the screen's name")); }).Num(), 2);

	// The session owns the rule too, for a caller that reaches it directly.
	FFlockSession Direct(FFlockAnalyticsConfig(), FPaths::Combine(Fix.Dir, TEXT("direct.json")));
	Direct.Start(TEXT("p"));
	Direct.RecordScreenView(TEXT("   "));
	TestEqual(TEXT("A name of spaces is not counted by the session"), Direct.GetScreensViewed(), 0);
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockSessionWholeSecondsTest, "SessionEnd.TheLengthIsRoundedToWholeSecondsAndEveryReasonHasItsName")
bool FFlockSessionWholeSecondsTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("0.9 s"), FlockDefaultEvents::WholeSeconds(0.9f), 1);
	TestEqual(TEXT("0.4 s"), FlockDefaultEvents::WholeSeconds(0.4f), 0);
	TestEqual(TEXT("2.5 s, away from zero"), FlockDefaultEvents::WholeSeconds(2.5f), 3);
	TestEqual(TEXT("-1 s"), FlockDefaultEvents::WholeSeconds(-1.f), 0);
	TestEqual(TEXT("0.5 s"), FlockDefaultEvents::WholeSeconds(0.5f), 1);
	TestEqual(TEXT("3.6 s"), FlockDefaultEvents::WholeSeconds(3.6f), 4);
	TestEqual(TEXT("below zero"), FlockDefaultEvents::WholeSeconds(-2.f), 0);

	TestEqualSensitive(TEXT("Logout"), FlockDefaultEvents::ReasonName(EFlockSessionEndReason::Logout), FString(TEXT("sign_out")));
	TestEqualSensitive(TEXT("Timeout"), FlockDefaultEvents::ReasonName(EFlockSessionEndReason::Timeout), FString(TEXT("timeout")));
	TestEqualSensitive(TEXT("Quit"), FlockDefaultEvents::ReasonName(EFlockSessionEndReason::Quit), FString(TEXT("quit")));
	TestEqualSensitive(TEXT("Restarted"), FlockDefaultEvents::ReasonName(EFlockSessionEndReason::Restarted), FString(TEXT("restarted")));
	TestEqualSensitive(TEXT("Manual"), FlockDefaultEvents::ReasonName(EFlockSessionEndReason::Manual), FString(TEXT("ended_by_the_game")));
	return true;
}

// ── The crash marker ──

FLOCK_DEFAULT_EVENTS_TEST(FFlockLaunchMarkerTest, "TheCrashMarkerRunsFromStartUpNotFromSignIn")
bool FFlockLaunchMarkerTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Session->ClearTokens();
	Fix.Provider->Initialize();
	const FString Marker = Fix.Deps.Launches->GetTerminationMarkerPath();
	TestTrue(TEXT("A marker with nobody signed in"), IFileManager::Get().FileExists(*Marker));

	Fix.Provider->StartSession(TEXT("player-a"));
	Fix.Provider->EndSession(EFlockSessionEndReason::Manual);
	TestTrue(TEXT("The launch goes on after a session ends, and its marker with it"), IFileManager::Get().FileExists(*Marker));

	// The launch dies: the next one reports it, naming no session since none was running.
	Fix.EndLaunchWithoutShuttingDown();
	FFixture Next(FFlockAnalyticsConfig(), Fix.Dir);
	Next.Provider->Initialize();
	const TArray<FFlockLogEventRequest> Reports = Next.AllSpooled().FilterByPredicate([](const FFlockLogEventRequest& Event)
	{
		return Event.Message == TEXT("app_termination");
	});
	TestEqual(TEXT("Reported by the next launch"), Reports.Num(), 1);
	TestFalse(TEXT("With no previous session id, since none was running"),
		Reports.Num() == 1 && Reports[0].Data.ExtraData.Contains(TEXT("previous_session_id")));

	// A clean shutdown takes the marker away.
	Next.Provider->Shutdown();
	TestFalse(TEXT("Gone after a clean shutdown"), IFileManager::Get().FileExists(*Next.Deps.Launches->GetTerminationMarkerPath()));
	return true;
}

// ── The flock_ parameters ──

FLOCK_DEFAULT_EVENTS_TEST(FFlockParametersTest, "EveryEventCarriesTheFlockParametersAndTheGamesOwnValueIsKept")
bool FFlockParametersTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();
	FFlockCommandData Mine;
	Mine.Set(TEXT("level"), 3);
	Mine.Set(TEXT("flock_platform"), TEXT("mine"));
	// Another spelling of a flock_ key is the game's too: the engine's JSON object cannot hold both.
	Mine.Set(TEXT("FLOCK_OS"), TEXT("my os"));
	const FString MineBefore = Mine.ToJsonString();
	TestTrue(TEXT("Recorded"), Fix.Provider->TrackEvent(TEXT("level_start"), Mine));

	const TArray<FFlockSpooledAnalyticsEvent> Events = Named(Fix, TEXT("level_start"));
	const TSharedPtr<FJsonObject> P = Events.Num() == 1 ? Props(Events[0]) : MakeShared<FJsonObject>();
	TestEqualSensitive(TEXT("A property the game set keeps its value"), JsonString(P, TEXT("flock_platform")), FString(TEXT("mine")));
	TestEqualSensitive(TEXT("So does another spelling of one"), JsonString(P, TEXT("FLOCK_OS")), FString(TEXT("my os")));
	TestFalse(TEXT("And the SDK's spelling is not added beside it"), Has(P, TEXT("flock_os")));
	TestEqualSensitive(TEXT("flock_device_model"), JsonString(P, TEXT("flock_device_model")), Expected(FPlatformMisc::GetDeviceMakeAndModel()));
	TestEqualSensitive(TEXT("flock_app_version: the Project Version"), JsonString(P, TEXT("flock_app_version")),
		Expected(GetDefault<UGeneralProjectSettings>()->ProjectVersion));
	TestEqualSensitive(TEXT("flock_sdk_version"), JsonString(P, TEXT("flock_sdk_version")), FString(TEXT("0.7.0")));
	TestEqualSensitive(TEXT("flock_language"), JsonString(P, TEXT("flock_language")), Expected(FPlatformMisc::GetDefaultLanguage()));
	TestFalse(TEXT("No session runs, so no session number"), Has(P, TEXT("flock_session_number")));
	TestEqual(TEXT("The game's own property"), Number(P, TEXT("level")), 3.0);
	TestEqualSensitive(TEXT("The game's own properties are never changed"), Mine.ToJsonString(), MineBefore);

	// The platform the engine names, and the operating system, on an event the game set neither on.
	TestTrue(TEXT("Recorded"), Fix.Provider->TrackEvent(TEXT("plain")));
	const TArray<FFlockSpooledAnalyticsEvent> Plain = Named(Fix, TEXT("plain"));
	const TSharedPtr<FJsonObject> Q = Plain.Num() == 1 ? Props(Plain[0]) : MakeShared<FJsonObject>();
	TestEqualSensitive(TEXT("flock_platform"), JsonString(Q, TEXT("flock_platform")), FString(FPlatformProperties::IniPlatformName()));
	TestEqualSensitive(TEXT("flock_os"), JsonString(Q, TEXT("flock_os")), Expected(FPlatformMisc::GetOSVersion()));
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockParametersSessionNumberTest, "AnEventDuringASessionCarriesItsSessionNumber")
bool FFlockParametersSessionNumberTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();
	Fix.Provider->StartSession(TEXT("p-fixture"));
	Fix.Provider->TrackEvent(TEXT("level_start"));
	const TArray<FFlockSpooledAnalyticsEvent> Events = Named(Fix, TEXT("level_start"));
	TestEqual(TEXT("flock_session_number"), Events.Num() == 1 ? Number(Props(Events[0]), TEXT("flock_session_number")) : -1.0,
		static_cast<double>(Fix.Provider->GetCurrentSnapshot().SessionNumber));
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockAppVersionSettingTest, "TheAppVersionIsTheProjectVersionSetting")
bool FFlockAppVersionSettingTest::RunTest(const FString& Parameters)
{
	UGeneralProjectSettings* Project = GetMutableDefault<UGeneralProjectSettings>();
	const FString Was = Project->ProjectVersion;
	Project->ProjectVersion = TEXT("9.9.9-test");
	ON_SCOPE_EXIT { Project->ProjectVersion = Was; };

	FFixture Fix;
	Fix.Provider->Initialize();
	Fix.Provider->TrackEvent(TEXT("level_start"));
	const TArray<FFlockSpooledAnalyticsEvent> Events = Named(Fix, TEXT("level_start"));
	TestEqualSensitive(TEXT("flock_app_version is read from Project Settings > Project > Description > Project Version"),
		Events.Num() == 1 ? JsonString(Props(Events[0]), TEXT("flock_app_version")) : FString(), FString(TEXT("9.9.9-test")));
	TestEqualSensitive(TEXT("And so is the session's"), Fix.Provider->GetCurrentSnapshot().DeviceInfo.AppVersion, FString(TEXT("9.9.9-test")));
	return true;
}

// ── app_open, first_launch, app_update ──

FLOCK_DEFAULT_EVENTS_TEST(FFlockLaunchEventsTest, "AppOpenAtEachStartAndFirstLaunchOnlyOnceForAGameOnADevice")
bool FFlockLaunchEventsTest::RunTest(const FString& Parameters)
{
	FFixture Fix(FFlockAnalyticsConfig(), FString(), /*bSendDefaultEvents*/ true);
	Fix.Session->ClearTokens();
	Fix.Provider->Initialize();
	const TArray<FFlockSpooledAnalyticsEvent> First = Fix.SpooledEvents();
	TestEqual(TEXT("app_open and first_launch"), NamesOf(First), TArray<FString>{ TEXT("app_open"), TEXT("first_launch") });
	TestTrue(TEXT("Category app"), First.Num() == 2 && First[0].Event.EventCategory == TEXT("app") && First[1].Event.EventCategory == TEXT("app"));
	TestTrue(TEXT("With the flock_ parameters"), First.Num() == 2 && JsonString(Props(First[0]), TEXT("flock_sdk_version")) == TEXT("0.7.0"));
	TestTrue(TEXT("Remembered once queued"), FlockDefaultEvents::FLaunchRecord::Load(RecordPath(Fix)).bFirstLaunchRecorded);

	// Recorded before sign-in, credited to the player who signs in. The server is away, so they stay queued to be read.
	TestFalse(TEXT("Precondition: no player yet"), First.ContainsByPredicate([](const FFlockSpooledAnalyticsEvent& E) { return !E.Event.PlayerId.IsEmpty(); }));
	Fix.Fake->On(TEXT("analytics/events"), FFlockFakeTransport::Offline());
	FString TokenError;
	Fix.Session->SetTokens(MakeTestJwt(TEXT("player-a")), TEXT("r-a"), TokenError);
	Fix.Provider->HandleAuthenticated(TEXT("player-a"));
	const TArray<FFlockSpooledAnalyticsEvent> Credited = Fix.SpooledEvents();
	TestTrue(TEXT("Credited to the player who signed in"), Credited.Num() == 2
		&& !Credited.ContainsByPredicate([](const FFlockSpooledAnalyticsEvent& E) { return E.Event.PlayerId != TEXT("player-a"); }));
	Fix.Provider->Shutdown();
	Fix.EndLaunchWithoutShuttingDown();

	FFixture Next(FFlockAnalyticsConfig(), Fix.Dir, /*bSendDefaultEvents*/ true);
	Next.Provider->Initialize();
	TestEqual(TEXT("app_open again, first_launch once for this game on this device"), NamesOf(Next.SpooledEvents()).FilterByPredicate(
		[](const FString& Name) { return Name == TEXT("app_open") || Name == TEXT("first_launch"); }), TArray<FString>{ TEXT("app_open") });
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockLaunchEventsConsentTest, "WithoutConsentAtStartTheLaunchEventsAreRecordedOnceConsentIsGiven")
bool FFlockLaunchEventsConsentTest::RunTest(const FString& Parameters)
{
	FFlockAnalyticsConfig Config;
	Config.bRequireExplicitConsent = true;
	FFixture Fix(Config, FString(), /*bSendDefaultEvents*/ true);
	Fix.Provider->Initialize();
	TestEqual(TEXT("Nothing before consent"), Fix.Provider->GetPendingAnalyticsEventCount(), 0);
	TestFalse(TEXT("And first_launch is not remembered"), FlockDefaultEvents::FLaunchRecord::Load(RecordPath(Fix)).bFirstLaunchRecorded);
	TestFalse(TEXT("Nor is there a crash marker"), IFileManager::Get().FileExists(*Fix.Deps.Launches->GetTerminationMarkerPath()));

	Fix.Provider->SetConsent(true);
	TestEqual(TEXT("Recorded when consent is given"), NamesOf(Fix.SpooledEvents()), TArray<FString>{ TEXT("app_open"), TEXT("first_launch") });
	TestTrue(TEXT("And the crash marker starts"), IFileManager::Get().FileExists(*Fix.Deps.Launches->GetTerminationMarkerPath()));
	Fix.Provider->SetConsent(false);
	Fix.Provider->SetConsent(true);
	TestEqual(TEXT("Once a launch, however often consent is given"), Fix.Provider->GetPendingAnalyticsEventCount(), 0);
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockLaunchEventsNoQueueTest, "WithNoEventQueueTheLaunchEventsWaitForTheSignIn")
bool FFlockLaunchEventsNoQueueTest::RunTest(const FString& Parameters)
{
	FFlockAnalyticsConfig Config;
	Config.bCacheFailedEvents = false;
	Config.MaxCachedEvents = 0;
	FFixture Fix(Config, FString(), /*bSendDefaultEvents*/ true);
	Fix.Fake->On(TEXT("analytics/events"), FFlockFakeTransport::Ok(TEXT("{}")));
	Fix.Session->ClearTokens();
	Fix.Provider->Initialize();
	TestEqual(TEXT("Nobody to send them for yet"), Fix.Fake->CountTo(TEXT("analytics/events")), 0);
	TestFalse(TEXT("first_launch is remembered only once it went out"), FlockDefaultEvents::FLaunchRecord::Load(RecordPath(Fix)).bFirstLaunchRecorded);

	FString TokenError;
	Fix.Session->SetTokens(MakeTestJwt(TEXT("player-a")), TEXT("r-a"), TokenError);
	Fix.Provider->HandleAuthenticated(TEXT("player-a"));
	const TArray<TArray<TSharedPtr<FJsonObject>>> Batches = Fix.SentEventBatches();
	TArray<FString> Sent;
	bool bAllForThePlayer = true;
	for (const TArray<TSharedPtr<FJsonObject>>& Batch : Batches)
	{
		for (const TSharedPtr<FJsonObject>& Event : Batch)
		{
			Sent.Add(JsonString(Event, TEXT("event_name")));
			bAllForThePlayer &= JsonString(Event, TEXT("player_id")) == TEXT("player-a");
		}
	}
	TestEqual(TEXT("Sent at the sign-in"), Sent, TArray<FString>{ TEXT("app_open"), TEXT("first_launch") });
	TestTrue(TEXT("For the player who signed in"), bAllForThePlayer);
	TestTrue(TEXT("Remembered once sent"), FlockDefaultEvents::FLaunchRecord::Load(RecordPath(Fix)).bFirstLaunchRecorded);
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockAppUpdateTest, "TheFirstStartRemembersTheVersionAndAStartOnAnotherRecordsAppUpdateOnce")
bool FFlockAppUpdateTest::RunTest(const FString& Parameters)
{
	const FString ThisVersion = FlockDefaultEvents::AppVersionOfThisGame();
	{
		FFixture Fix(FFlockAnalyticsConfig(), FString(), /*bSendDefaultEvents*/ true);
		Fix.Provider->Initialize();
		TestEqual(TEXT("No app_update at the first start"), Named(Fix, FlockDefaultEvents::AppUpdate).Num(), 0);
		TestEqualSensitive(TEXT("The version is remembered"), FlockDefaultEvents::FLaunchRecord::Load(RecordPath(Fix)).LastAppVersion, ThisVersion);
	}

	FFixture Owner;
	FlockDefaultEvents::FLaunchRecord Older;
	Older.bFirstLaunchRecorded = true;
	Older.LastAppVersion = TEXT("0.0.1-older");
	Older.Save(RecordPath(Owner));
	FFixture Fix(FFlockAnalyticsConfig(), Owner.Dir, /*bSendDefaultEvents*/ true);
	Fix.Provider->Initialize();
	const TArray<FFlockSpooledAnalyticsEvent> Updates = Named(Fix, FlockDefaultEvents::AppUpdate);
	if (TestEqual(TEXT("One app_update"), Updates.Num(), 1))
	{
		TestEqualSensitive(TEXT("Category app"), Updates[0].Event.EventCategory, FString(TEXT("app")));
		TestEqualSensitive(TEXT("previous_app_version"), JsonString(Props(Updates[0]), TEXT("previous_app_version")), FString(TEXT("0.0.1-older")));
		TestEqualSensitive(TEXT("flock_app_version"), JsonString(Props(Updates[0]), TEXT("flock_app_version")), ThisVersion);
	}
	TestEqualSensitive(TEXT("Remembered once recorded"), FlockDefaultEvents::FLaunchRecord::Load(RecordPath(Fix)).LastAppVersion, ThisVersion);
	Fix.Provider->Shutdown();
	Fix.EndLaunchWithoutShuttingDown();

	FFixture Again(FFlockAnalyticsConfig(), Owner.Dir, /*bSendDefaultEvents*/ true);
	Again.Provider->Initialize();
	TestEqual(TEXT("Once an update"), Named(Again, FlockDefaultEvents::AppUpdate).Num(), 0);
	return true;
}

// ── screen_view ──

FLOCK_DEFAULT_EVENTS_TEST(FFlockScreenViewTest, "AScreenViewIsRecordedAsScreenViewAlsoWithoutASessionAndCountedInOne")
bool FFlockScreenViewTest::RunTest(const FString& Parameters)
{
	FFixture Fix(FFlockAnalyticsConfig(), FString(), /*bSendDefaultEvents*/ true);
	Fix.Provider->Initialize();
	Fix.Provider->RecordScreenView(TEXT("Title"));
	Fix.Provider->StartSession(TEXT("p-fixture"));
	Fix.Provider->RecordScreenView(TEXT("MainMenu"));

	const TArray<FFlockSpooledAnalyticsEvent> Views = Named(Fix, FlockDefaultEvents::ScreenView);
	if (TestEqual(TEXT("Both recorded"), Views.Num(), 2))
	{
		TestEqualSensitive(TEXT("Category screen"), Views[0].Event.EventCategory, FString(TEXT("screen")));
		TestEqualSensitive(TEXT("Without a session"), JsonString(Props(Views[0]), TEXT("screen_name")), FString(TEXT("Title")));
		TestFalse(TEXT("No session number then"), Has(Props(Views[0]), TEXT("flock_session_number")));
		TestEqualSensitive(TEXT("In the session"), JsonString(Props(Views[1]), TEXT("screen_name")), FString(TEXT("MainMenu")));
		TestEqual(TEXT("With its number"), Number(Props(Views[1]), TEXT("flock_session_number")),
			static_cast<double>(Fix.Provider->GetCurrentSnapshot().SessionNumber));
	}
	TestEqual(TEXT("Counted in the session"), Fix.Provider->GetCurrentSnapshot().ScreensViewed, 1);
	return true;
}

// ── The opt-out and the refusals ──

FLOCK_DEFAULT_EVENTS_TEST(FFlockDefaultEventsOffTest, "SendDefaultEventsOffRecordsNoneOfTheSdksEventsButKeepsTheParameters")
bool FFlockDefaultEventsOffTest::RunTest(const FString& Parameters)
{
	FFixture Owner;
	PlantOpenSession(Owner.Dir, OpenSession(TEXT("s-ended"), 30.f));
	FFixture Fix(FFlockAnalyticsConfig(), Owner.Dir, /*bSendDefaultEvents*/ false);
	Fix.Provider->Initialize();
	Fix.Provider->StartSession(TEXT("p-fixture"));
	Fix.Provider->RecordScreenView(TEXT("MainMenu"));
	TestTrue(TEXT("The game's own event"), Fix.Provider->TrackEvent(TEXT("level_start")));
	Fix.Provider->EndSession(EFlockSessionEndReason::Manual);

	TestEqual(TEXT("Only the game's own event"), NamesOf(Fix.SpooledEvents()), TArray<FString>{ TEXT("level_start") });
	TestEqualSensitive(TEXT("Still with the flock_ parameters"),
		JsonString(Props(FlockTestAt(Fix.SpooledEvents(), 0)), TEXT("flock_sdk_version")), FString(TEXT("0.7.0")));
	TestEqual(TEXT("The view is still counted"), Fix.Provider->GetCurrentSnapshot().ScreensViewed, 1);
	TestTrue(TEXT("The sessions themselves still reach the server"), RequestsTo(Fix, TEXT("PATCH"), TEXT("analytics/sessions/")).Num() >= 1);
	return true;
}

FLOCK_DEFAULT_EVENTS_TEST(FFlockDefaultEventNamesRefusedTest, "TheSdksOwnEventNamesAreRefusedToTheGameLetterForLetter")
bool FFlockDefaultEventNamesRefusedTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	const TSharedRef<FFlockRecordingLogger> Logger = RebuildWithLogger(Fix, FFlockAnalyticsConfig());
	Fix.Provider->Initialize();
	const TArray<FString> Names = { TEXT("session_end"), TEXT("app_open"), TEXT("first_launch"), TEXT("app_update"), TEXT("store_purchase"), TEXT("screen_view") };
	TestEqual(TEXT("The six"), FlockDefaultEvents::Names().Num(), Names.Num());
	for (const FString& Name : Names)
	{
		TestFalse(Name + TEXT(" is refused"), Fix.Provider->TrackEvent(Name));
		TestTrue(Name + TEXT(": a warning names it"), Logger->Warnings.ContainsByPredicate([&Name](const FString& Line)
		{
			return Line.Contains(TEXT("'") + Name + TEXT("' is reserved"), ESearchCase::CaseSensitive) && Line.Contains(TEXT("Analytics Send Default Events"));
		}));
	}
	TestEqual(TEXT("None recorded"), Fix.Provider->GetPendingAnalyticsEventCount(), 0);
	TestTrue(TEXT("Another letter case is the game's own name"), Fix.Provider->TrackEvent(TEXT("Session_End")));
	TestTrue(TEXT("And so is App_Open"), Fix.Provider->TrackEvent(TEXT("App_Open")));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
