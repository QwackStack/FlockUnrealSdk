// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Analytics/FlockAnalyticsJson.h"
#include "Analytics/FlockLogSink.h"
#include "Auth/FlockAuthSession.h"
#include "FlockEvents.h"
#include "FlockLogger.h"
#include "HAL/FileManager.h"
#include "Http/FlockHttpClient.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Providers/FlockAnalyticsProvider.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryEventCache.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockTestSafeIndex.h"

namespace FlockAnalyticsProviderTestHelpers
{
	inline FFlockRetryPolicy NoRetryPolicy()
	{
		FFlockRetryPolicy Policy;
		Policy.MaxRetries = 0;
		return Policy;
	}

	/** Minimal signed-in-looking token so the auth session can report a player id. */
	inline FString MakeTestJwt(const FString& PlayerId)
	{
		const int64 Exp = FDateTime::UtcNow().ToUnixTimestamp() + 3600;
		FString Payload = FBase64::Encode(FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId, Exp));
		Payload.ReplaceInline(TEXT("+"), TEXT("-"));
		Payload.ReplaceInline(TEXT("/"), TEXT("_"));
		Payload.ReplaceInline(TEXT("="), TEXT(""));
		return FString::Printf(TEXT("h.%s.s"), *Payload);
	}

	inline FString TempDir()
	{
		return FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
			FString::Printf(TEXT("prov_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	/** Wires the provider against fakes: in-memory spool, fake transport, temp files for the stores. */
	struct FFixture
	{
		/** Shared with a second fixture to model a relaunch over the same on-disk state. */
		FString Dir;
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockHttpClient> Client;
		TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();
		TSharedRef<FFlockAuthSession> Session;
		UFlockEvents* Events = nullptr;
		TSharedPtr<FFlockMemoryEventCache> Cache;
		TSharedPtr<FFlockAnalyticsProvider> Provider;
		FFlockAnalyticsDependencies Deps;

		/** Pass ExistingDir to reuse another fixture's files — that is what "the next launch" means. */
		explicit FFixture(FFlockAnalyticsConfig Config = FFlockAnalyticsConfig(),
			const FString& ExistingDir = FString())
			: Dir(ExistingDir.IsEmpty() ? TempDir() : ExistingDir)
			, Client(MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>()))
			, Session(MakeShared<FFlockAuthSession>(Client, Store, MakeShared<FFlockNullLogger>(),
				TEXT("http://x/v1"), TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") } }))
		{
			Events = NewObject<UFlockEvents>();

			Cache = MakeShared<FFlockMemoryEventCache>(Config.MaxCachedEvents);
			Deps.LogEventCache = Cache;
			Deps.Session = MakeShared<FFlockSession>(Config, FPaths::Combine(Dir, TEXT("session.json")));
			Deps.TerminationTracker = MakeShared<FFlockTerminationTracker>(true, MarkerPath());
			Deps.ConsentStore = MakeShared<FFlockConsentStore>(FPaths::Combine(Dir, TEXT("consent.json")));
			Deps.Pump = MakeShared<FFlockLifecyclePump>();
			Deps.bEnableLogSink = false; // a GLog tap inside the runner captures the runner's own errors

			Fake->On(TEXT("analytics/sessions"), FFlockFakeTransport::Ok(TEXT("{\"session_id\":\"srv-1\"}")));
			Fake->On(TEXT("log_event"), FFlockFakeTransport::Ok(TEXT("{}")));

			Provider = MakeShared<FFlockAnalyticsProvider>(Client, NoRetryPolicy(), MakeShared<FFlockNullLogger>(),
				Session, Events, TEXT("http://x/v1"), Config, Deps, TEXT("gv-1"), TEXT("0.7.0"));
		}

		FString MarkerPath() const { return FPaths::Combine(Dir, TEXT("marker.json")); }

		~FFixture()
		{
			Provider.Reset();
			IFileManager::Get().DeleteDirectory(*Dir, false, true);
		}

		/** The single spooled payload, parsed. */
		bool FirstSpooled(FFlockLogEventRequest& OutEvent) const
		{
			TArray<FString> Handles;
			TArray<FString> Payloads;
			Cache->PeekBatch(1, Handles, Payloads);
			return Payloads.Num() > 0 && FFlockAnalyticsJson::DeserializeEvent(Payloads[0], OutEvent);
		}
	};
}

using namespace FlockAnalyticsProviderTestHelpers;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsConsentGateTest, "Flock.Analytics.Provider.ConsentGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsConsentGateTest::RunTest(const FString& Parameters)
{
	FFlockAnalyticsConfig Config;
	Config.bRequireExplicitConsent = true;
	FFixture Fix(Config);
	Fix.Provider->Initialize();

	// Consent is a hard gate: nothing is collected, not even locally.
	TestFalse(TEXT("no consent yet"), Fix.Provider->HasConsent());
	Fix.Provider->LogEvent(TEXT("before consent"));
	TestEqual(TEXT("nothing spooled"), Fix.Provider->GetPendingEventCount(), 0);

	// And no session may start.
	bool bStartFailed = false;
	Fix.Provider->StartSession(TEXT("p-1"), [&bStartFailed](TFlockResult<FString> Result)
	{
		bStartFailed = !Result.bSuccess;
	});
	TestTrue(TEXT("session refused without consent"), bStartFailed);
	TestFalse(TEXT("no active session"), Fix.Provider->HasActiveSession());
	TestEqual(TEXT("no session call made"), Fix.Fake->CountTo(TEXT("analytics/sessions")), 0);

	// Granting opens the gate and raises the event.
	Fix.Provider->SetConsent(true);
	TestTrue(TEXT("consent granted"), Fix.Provider->HasConsent());
	Fix.Provider->LogEvent(TEXT("after consent"));
	TestEqual(TEXT("now spooled"), Fix.Provider->GetPendingEventCount(), 1);

	// Revoking drops what was collected — an opt-out must not leave data behind.
	Fix.Provider->SetConsent(false);
	TestFalse(TEXT("consent revoked"), Fix.Provider->HasConsent());
	TestEqual(TEXT("spool dropped on revoke"), Fix.Provider->GetPendingEventCount(), 0);

	// The decision persists, so a later run stays revoked.
	TestTrue(TEXT("decision recorded"), Fix.Deps.ConsentStore->HasDecision());
	TestFalse(TEXT("recorded as revoked"), Fix.Deps.ConsentStore->ResolveEffective(true));
	return true;
}

/**
 * The opt-in flow end to end: a player signs in while collection is gated, so no session can open.
 * Granting consent later must open the session that could not open then — otherwise a GDPR-style
 * project never gets a session at all, because sign-in has already been and gone.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsConsentOptInTest, "Flock.Analytics.Provider.ConsentOptIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsConsentOptInTest::RunTest(const FString& Parameters)
{
	FFlockAnalyticsConfig Config;
	Config.bRequireExplicitConsent = true;
	Config.bAutoStartSession = true;
	FFixture Fix(Config);
	Fix.Provider->Initialize();

	// Sign-in happens while gated: refused, and no call goes out.
	Fix.Provider->StartSession(TEXT("p-1"));
	TestFalse(TEXT("no session while gated"), Fix.Provider->HasActiveSession());
	TestEqual(TEXT("no start call while gated"), Fix.Fake->CountTo(TEXT("analytics/sessions")), 0);

	// Granting opens it, using the player id remembered from the refused attempt.
	Fix.Provider->SetConsent(true);
	TestTrue(TEXT("session opened on consent"), Fix.Provider->HasActiveSession());
	TestEqual(TEXT("start call went out"), Fix.Fake->CountTo(TEXT("analytics/sessions")), 1);
	TestEqual(TEXT("server id adopted"), Fix.Provider->GetCurrentSessionId(), TEXT("srv-1"));

	// Granting again is not a second session.
	Fix.Provider->SetConsent(true);
	TestEqual(TEXT("no duplicate start"), Fix.Fake->CountTo(TEXT("analytics/sessions")), 1);

	// With no player ever seen, granting has nothing to open.
	{
		FFixture Fresh(Config);
		Fresh.Provider->Initialize();
		Fresh.Provider->SetConsent(true);
		TestFalse(TEXT("no session without a known player"), Fresh.Provider->HasActiveSession());
		TestEqual(TEXT("and no call"), Fresh.Fake->CountTo(TEXT("analytics/sessions")), 0);
	}

	// Auto-start off means consent alone never opens one.
	{
		FFlockAnalyticsConfig Manual = Config;
		Manual.bAutoStartSession = false;
		FFixture ManualFix(Manual);
		ManualFix.Provider->Initialize();
		ManualFix.Provider->StartSession(TEXT("p-1"));
		ManualFix.Provider->SetConsent(true);
		TestFalse(TEXT("auto-start off keeps it closed"), ManualFix.Provider->HasActiveSession());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsLogShapesTest, "Flock.Analytics.Provider.LogShapes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsLogShapesTest::RunTest(const FString& Parameters)
{
	// Default config: consent not required, so collection is on.
	FFixture Fix;
	Fix.Provider->Initialize();

	Fix.Provider->LogEvent(TEXT("hello"), TMap<FString, FString>{ { TEXT("playerLevel"), TEXT("7") } });
	{
		FFlockLogEventRequest Event;
		TestTrue(TEXT("spooled"), Fix.FirstSpooled(Event));
		TestTrue(TEXT("plain event is debug"), Event.Data.Type == EFlockLogEventType::Debug);
		TestEqual(TEXT("message"), Event.Message, TEXT("hello"));
		TestEqual(TEXT("game version stamped"), Event.Data.GameVersion, TEXT("gv-1"));
		TestFalse(TEXT("timestamped"), Event.Timestamp.IsEmpty());
		const FString* Level = Event.Data.ExtraData.Find(TEXT("playerLevel"));
		TestTrue(TEXT("caller key preserved end to end"), Level != nullptr);
	}
	Fix.Cache->Clear();

	FFlockLogDetails ErrorDetails;
	ErrorDetails.LogicalExpression = TEXT("hp > 0");
	ErrorDetails.ErrorCode = TEXT("E7");
	Fix.Provider->LogError(TEXT("bad state"), ErrorDetails);
	{
		FFlockLogEventRequest Event;
		TestTrue(TEXT("spooled"), Fix.FirstSpooled(Event));
		TestTrue(TEXT("error is logic_error"), Event.Data.Type == EFlockLogEventType::LogicError);
		TestEqual(TEXT("logical expression"), Event.Data.LogicalExpression, TEXT("hp > 0"));
		TestEqual(TEXT("error code"), Event.Data.ErrorCode, TEXT("E7"));
		TestEqual(TEXT("error message"), Event.Data.ErrorMessage, TEXT("bad state"));
	}
	Fix.Cache->Clear();

	Fix.Provider->LogException(TEXT("boom"), TEXT("at Foo()\nat Bar()"));
	{
		FFlockLogEventRequest Event;
		TestTrue(TEXT("spooled"), Fix.FirstSpooled(Event));
		TestTrue(TEXT("exception type"), Event.Data.Type == EFlockLogEventType::Exception);
		TestEqual(TEXT("traceback kept whole"), Event.Data.ErrorTraceback, TEXT("at Foo()\nat Bar()"));
		TestEqual(TEXT("traceback split into lines"), Event.Data.ErrorTracebackLines.Num(), 2);
		TestEqual(TEXT("first frame"), FlockTestAt(Event.Data.ErrorTracebackLines, 0), TEXT("at Foo()"));
	}

	// Exceptions feed the next launch's termination context — but only once a session is being
	// tombstoned, since that is what BeginTracking sets up.
	TestEqual(TEXT("nothing noted before a session exists"),
		Fix.Deps.TerminationTracker->GetPendingExceptionCount(), 0);

	Fix.Provider->StartSession(TEXT("p-1"));
	Fix.Provider->LogException(TEXT("later boom"));
	TestEqual(TEXT("noted against the tombstone once tracking"),
		Fix.Deps.TerminationTracker->GetPendingExceptionCount(), 1);
	return true;
}

/**
 * The automatic capture path end to end: an engine error reaches the sink, the tick drains it, and it
 * arrives in the spool as an exception carrying its callstack.
 *
 * This exists because the wiring was wrong once and nothing noticed: both capture sites passed an
 * empty stack trace, so every automatically-reported exception reached the backend with no callstack
 * at all. `LogShapes` could not catch it — it exercises the manual API, where the caller supplies
 * the trace.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsAutoCaptureTest, "Flock.Analytics.Provider.AutoCapture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsAutoCaptureTest::RunTest(const FString& Parameters)
{
	FFlockAnalyticsConfig Config;
	Config.EventBufferFlushIntervalSeconds = 0.f; // no interval flush, so the spool holds for inspection
	Config.bTrackFps = false;
	FFixture Fix(Config);
	Fix.Deps.bEnableLogSink = true;

	// Rebuild with the sink enabled — the fixture defaults it off so other tests do not tap GLog.
	Fix.Provider = MakeShared<FFlockAnalyticsProvider>(Fix.Client, NoRetryPolicy(),
		MakeShared<FFlockNullLogger>(), Fix.Session, Fix.Events, TEXT("http://x/v1"), Config, Fix.Deps,
		TEXT("gv-1"), TEXT("0.7.0"));
	Fix.Provider->Initialize();

	FFlockLogSink* Sink = Fix.Provider->GetLogSinkForTesting();
	TestNotNull(TEXT("sink is live"), Sink);
	if (Sink == nullptr)
	{
		return false;
	}

	// Drive the real FOutputDevice entry point rather than a stand-in.
	Sink->Serialize(TEXT("engine side failure"), ELogVerbosity::Error, FName(TEXT("LogGame")));
	Fix.Provider->TickForTesting(0.1f);

	// Other engine errors may land here too, so find ours rather than assuming it is alone.
	TArray<FString> Handles;
	TArray<FString> Payloads;
	Fix.Cache->PeekBatch(50, Handles, Payloads);

	bool bFound = false;
	for (const FString& Payload : Payloads)
	{
		FFlockLogEventRequest Event;
		if (!FFlockAnalyticsJson::DeserializeEvent(Payload, Event) ||
			!Event.Message.Equals(TEXT("engine side failure")))
		{
			continue;
		}
		bFound = true;
		TestTrue(TEXT("reported as an exception"), Event.Data.Type == EFlockLogEventType::Exception);
		TestEqual(TEXT("error message carried"), Event.Data.ErrorMessage, TEXT("engine side failure"));
		TestFalse(TEXT("callstack reached the spooled event"), Event.Data.ErrorTraceback.IsEmpty());
		// Module-relative, so the frame survives ASLR and can be symbolicated from a symbol server.
		TestTrue(TEXT("frames are module+offset"),
			Event.Data.ErrorTraceback.Contains(TEXT(".dll+0x")) ||
			Event.Data.ErrorTraceback.Contains(TEXT(".exe+0x")));
		TestFalse(TEXT("no zero offsets (OffsetInModule is not populated on Windows)"),
			Event.Data.ErrorTraceback.Contains(TEXT("+0x0\n")));
		TestTrue(TEXT("callstack split into lines"), Event.Data.ErrorTracebackLines.Num() > 0);
		const FString* Category = Event.Data.ExtraData.Find(TEXT("category"));
		TestTrue(TEXT("originating category recorded"), Category != nullptr);
		if (Category != nullptr)
		{
			TestEqual(TEXT("category"), *Category, TEXT("LogGame"));
		}
	}
	TestTrue(TEXT("the captured error reached the spool"), bFound);

	Fix.Provider->Shutdown();
	return true;
}

/**
 * A manual LogException with no trace must capture one. Before this, the parameter was required and
 * the SDK's own self-test satisfied it with a hand-written "at SelfTest()" placeholder — so real
 * reports arrived with nothing useful in them.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsManualTraceTest, "Flock.Analytics.Provider.ManualExceptionTrace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsManualTraceTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();

	// No stack trace argument at all.
	Fix.Provider->LogException(TEXT("reported by hand"));

	FFlockLogEventRequest Event;
	TestTrue(TEXT("spooled"), Fix.FirstSpooled(Event));
	TestTrue(TEXT("typed as an exception"), Event.Data.Type == EFlockLogEventType::Exception);
	TestFalse(TEXT("callstack captured without being asked"), Event.Data.ErrorTraceback.IsEmpty());
	TestTrue(TEXT("module-relative frames"),
		Event.Data.ErrorTraceback.Contains(TEXT(".dll+0x")) ||
		Event.Data.ErrorTraceback.Contains(TEXT(".exe+0x")));
	TestTrue(TEXT("split into lines"), Event.Data.ErrorTracebackLines.Num() > 0);
	// The first frame must be the caller, not LogException itself — an off-by-one in the skip count
	// is invisible unless you read a trace, and it silently buries the useful frame.
	TestTrue(TEXT("trace starts at the caller, not inside the SDK"),
		!FlockTestAt(Event.Data.ErrorTracebackLines, 0).Contains(TEXT("FFlockAnalyticsProvider::LogException")));

	// A caller who supplies a better trace keeps it — the SDK must not overwrite it.
	Fix.Cache->Clear();
	Fix.Provider->LogException(TEXT("from a script vm"), TEXT("at Foo()\nat Bar()"));
	TestTrue(TEXT("spooled"), Fix.FirstSpooled(Event));
	TestEqual(TEXT("supplied trace preserved"), Event.Data.ErrorTraceback, TEXT("at Foo()\nat Bar()"));
	TestEqual(TEXT("and split as given"), Event.Data.ErrorTracebackLines.Num(), 2);
	return true;
}

/** Empty player id means "whoever is signed in", so callers need not fetch it themselves. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsImplicitPlayerTest, "Flock.Analytics.Provider.ImplicitPlayerId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsImplicitPlayerTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();

	// Sign in so the auth session knows a player.
	FString TokenError;
	Fix.Session->SetTokens(MakeTestJwt(TEXT("p-implicit")), TEXT("r-1"), TokenError);
	TestTrue(TEXT("session authenticated"), Fix.Session->IsAuthenticated());

	bool bStarted = false;
	Fix.Provider->StartSession(FString(), [&bStarted](TFlockResult<FString> Result) { bStarted = Result.bSuccess; });

	TestTrue(TEXT("session started without being handed a player id"), bStarted);
	TestTrue(TEXT("active"), Fix.Provider->HasActiveSession());
	TestTrue(TEXT("attributed to the signed-in player"),
		Fix.Fake->Requests.Last().JsonBody.Contains(TEXT("\"player_id\":\"p-implicit\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsFlushTest, "Flock.Analytics.Provider.Flush",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsFlushTest::RunTest(const FString& Parameters)
{
	FFlockAnalyticsConfig Config;
	Config.CacheFlushBatchSize = 2;
	FFixture Fix(Config);
	Fix.Provider->Initialize();

	for (int32 Index = 0; Index < 5; ++Index)
	{
		Fix.Provider->LogEvent(FString::Printf(TEXT("event %d"), Index));
	}
	TestEqual(TEXT("five spooled"), Fix.Provider->GetPendingEventCount(), 5);

	bool bFlushed = false;
	Fix.Provider->Flush([&bFlushed](TFlockResult<FFlockAnalyticsAck> Result) { bFlushed = Result.bSuccess; });

	TestTrue(TEXT("flush reported success"), bFlushed);
	TestEqual(TEXT("spool drained"), Fix.Provider->GetPendingEventCount(), 0);
	// 5 entries at 2 per batch = 3 batches.
	TestEqual(TEXT("drained batch by batch"), Fix.Fake->CountTo(TEXT("log_event")), 3);

	// Flushing an empty spool is a no-op that still reports success.
	const int32 Before = Fix.Fake->CountTo(TEXT("log_event"));
	bool bEmptyFlush = false;
	Fix.Provider->Flush([&bEmptyFlush](TFlockResult<FFlockAnalyticsAck> Result) { bEmptyFlush = Result.bSuccess; });
	TestTrue(TEXT("empty flush succeeds"), bEmptyFlush);
	TestEqual(TEXT("and sends nothing"), Fix.Fake->CountTo(TEXT("log_event")), Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsFlushFailureTest, "Flock.Analytics.Provider.FlushFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsFlushFailureTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();
	Fix.Provider->LogEvent(TEXT("keep me"));
	Fix.Provider->LogEvent(TEXT("keep me too"));

	// The whole point of the write-ahead spool: a failed send loses nothing.
	Fix.Fake->On(TEXT("log_event"), FFlockFakeTransport::Offline());

	bool bReportedFailure = false;
	Fix.Provider->Flush([&bReportedFailure](TFlockResult<FFlockAnalyticsAck> Result)
	{
		bReportedFailure = !Result.bSuccess;
	});

	TestTrue(TEXT("failure surfaced"), bReportedFailure);
	TestEqual(TEXT("entries stay spooled"), Fix.Provider->GetPendingEventCount(), 2);

	// Recovery: once the network is back, the same entries go out.
	Fix.Fake->On(TEXT("log_event"), FFlockFakeTransport::Ok(TEXT("{}")));
	bool bRecovered = false;
	Fix.Provider->Flush([&bRecovered](TFlockResult<FFlockAnalyticsAck> Result) { bRecovered = Result.bSuccess; });
	TestTrue(TEXT("recovered"), bRecovered);
	TestEqual(TEXT("drained after recovery"), Fix.Provider->GetPendingEventCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsSessionTest, "Flock.Analytics.Provider.Session",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsSessionTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();

	FString StartedId;
	bool bStarted = false;
	Fix.Provider->StartSession(TEXT("p-1"), [&](TFlockResult<FString> Result)
	{
		bStarted = Result.bSuccess;
		StartedId = Result.Value;
	});

	TestTrue(TEXT("session started"), bStarted);
	TestEqual(TEXT("server id returned"), StartedId, TEXT("srv-1"));
	TestEqual(TEXT("server id adopted"), Fix.Provider->GetCurrentSessionId(), TEXT("srv-1"));
	TestTrue(TEXT("session active"), Fix.Provider->HasActiveSession());
	TestEqual(TEXT("one start call"), Fix.Fake->CountTo(TEXT("analytics/sessions")), 1);

	// Starting again while active does not open a second session.
	Fix.Provider->StartSession(TEXT("p-1"));
	TestEqual(TEXT("still one start call"), Fix.Fake->CountTo(TEXT("analytics/sessions")), 1);

	Fix.Provider->RecordScreenView(TEXT("MainMenu"));
	Fix.Provider->RecordScreenView(TEXT("Shop"));
	TestEqual(TEXT("screens counted"), Fix.Provider->GetCurrentSnapshot().ScreensViewed, 2);

	bool bEnded = false;
	Fix.Provider->EndSession(EFlockSessionEndReason::Manual, [&bEnded](TFlockResult<FFlockAnalyticsAck> Result)
	{
		bEnded = Result.bSuccess;
	});

	TestTrue(TEXT("session ended"), bEnded);
	TestFalse(TEXT("no longer active"), Fix.Provider->HasActiveSession());
	// The end call is a PATCH to the id-scoped route.
	TestEqual(TEXT("ended against the server id"), Fix.Fake->CountTo(TEXT("analytics/sessions/srv-1")), 1);

	// Ending twice is harmless.
	bool bSecondEnd = false;
	Fix.Provider->EndSession(EFlockSessionEndReason::Manual, [&bSecondEnd](TFlockResult<FFlockAnalyticsAck> Result)
	{
		bSecondEnd = Result.bSuccess;
	});
	TestTrue(TEXT("second end is a no-op success"), bSecondEnd);
	TestEqual(TEXT("no extra call"), Fix.Fake->CountTo(TEXT("analytics/sessions/srv-1")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsTerminationReportTest, "Flock.Analytics.Provider.TerminationReport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsTerminationReportTest::RunTest(const FString& Parameters)
{
	FFixture Fix;

	// Leave behind a tombstone as if the previous run died backgrounded.
	const FString Marker =
		TEXT("{\"last_state\":\"background\",\"session_id\":\"old-1\",\"server_session_id\":\"srv-old\",")
		TEXT("\"player_id\":\"p-1\",\"last_alive_utc\":\"2026-07-20T10:00:00Z\",\"exception_count\":2,")
		TEXT("\"app_version\":\"1.2.3\",\"sdk_version\":\"0.6.0\"}");
	FFileHelper::SaveStringToFile(Marker, *Fix.MarkerPath());

	Fix.Provider->Initialize();

	TestEqual(TEXT("one termination event queued"), Fix.Provider->GetPendingEventCount(), 1);

	FFlockLogEventRequest Event;
	TestTrue(TEXT("spooled"), Fix.FirstSpooled(Event));
	TestEqual(TEXT("named app_termination"), Event.Message, TEXT("app_termination"));
	// Debug, not exception: it is a record *about* a crash, and must not inflate exception stats.
	TestTrue(TEXT("reported as debug"), Event.Data.Type == EFlockLogEventType::Debug);

	const FString* Classification = Event.Data.ExtraData.Find(TEXT("classification"));
	TestTrue(TEXT("classification present"), Classification != nullptr);
	if (Classification != nullptr)
	{
		TestEqual(TEXT("died backgrounded"), *Classification, TEXT("background_kill"));
	}
	const FString* Previous = Event.Data.ExtraData.Find(TEXT("previous_session_id"));
	TestTrue(TEXT("previous session present"), Previous != nullptr);
	if (Previous != nullptr)
	{
		TestEqual(TEXT("prefers the server id"), *Previous, TEXT("srv-old"));
	}
	const FString* Exceptions = Event.Data.ExtraData.Find(TEXT("unhandled_exception_count"));
	TestTrue(TEXT("exception count carried"), Exceptions != nullptr);
	if (Exceptions != nullptr)
	{
		TestEqual(TEXT("count"), *Exceptions, TEXT("2"));
	}

	// Reported once only. This must relaunch over the SAME files — a fresh directory would pass even
	// if the marker were never cleared, which is exactly the bug this guards against.
	Fix.Cache->Clear();
	{
		FFixture NextLaunch(FFlockAnalyticsConfig(), Fix.Dir);
		NextLaunch.Provider->Initialize();
		TestEqual(TEXT("the same death is not reported twice"), NextLaunch.Provider->GetPendingEventCount(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsTickTest, "Flock.Analytics.Provider.Tick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsTickTest::RunTest(const FString& Parameters)
{
	FFlockAnalyticsConfig Config;
	Config.EventBufferFlushIntervalSeconds = 10.f;
	Config.HeartbeatIntervalSeconds = 60.f;
	Config.bTrackFps = false;
	FFixture Fix(Config);
	Fix.Provider->Initialize();
	Fix.Provider->StartSession(TEXT("p-1"));

	Fix.Provider->LogEvent(TEXT("queued"));
	TestEqual(TEXT("spooled"), Fix.Provider->GetPendingEventCount(), 1);

	// Below the flush interval nothing goes out.
	Fix.Provider->TickForTesting(5.f);
	TestEqual(TEXT("not flushed yet"), Fix.Provider->GetPendingEventCount(), 1);

	// Crossing it drains the spool without an explicit Flush call.
	Fix.Provider->TickForTesting(6.f);
	TestEqual(TEXT("interval flush drained it"), Fix.Provider->GetPendingEventCount(), 0);

	// Session time accrues from the ticks.
	TestEqual(TEXT("duration accumulated"), Fix.Provider->GetCurrentSnapshot().DurationSeconds, 11.f);

	// The heartbeat is local: it refreshes the on-disk death-time estimate and stamps the session,
	// and must not cost a round trip. The spool was drained above, so an interval flush issues no
	// request either — which makes ANY new request here proof that the heartbeat hit the network.
	const int32 CallsBefore = Fix.Fake->Requests.Num();
	Fix.Provider->TickForTesting(60.f);
	const FFlockSessionSnapshot Snapshot = Fix.Provider->GetCurrentSnapshot();
	TestFalse(TEXT("heartbeat stamped the session"), Snapshot.LastHeartbeatUtc.IsEmpty());
	TestEqual(TEXT("heartbeat issued no request"), Fix.Fake->Requests.Num(), CallsBefore);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
