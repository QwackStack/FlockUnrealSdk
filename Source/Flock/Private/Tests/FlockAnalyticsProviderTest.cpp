// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Analytics/FlockAnalyticsJson.h"
#include "Analytics/FlockLogSink.h"
#include "Auth/FlockAuthSession.h"
#include "FlockEvents.h"
#include "FlockLogger.h"
#include "HAL/FileManager.h"
#include "Http/FlockHttpClient.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Providers/FlockAnalyticsProvider.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Script.h"
#include "Tests/Support/FlockEventTestListener.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryEventCache.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockRecordingLogger.h"
#include "Tests/Support/FlockTestSafeIndex.h"
#include "HAL/PlatformProperties.h"

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
		TSharedPtr<FFlockMemoryEventCache> EndCache;
		TSharedPtr<FFlockMemoryEventCache> EventCache;
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

			// Signed in by default, because that is the only state analytics really runs in: a session
			// needs a player, and every session route needs a bearer. Tests that care about the
			// signed-out path clear this explicitly.
			FString TokenError;
			Session->SetTokens(MakeTestJwt(TEXT("p-fixture")), TEXT("r-1"), TokenError);

			Cache = MakeShared<FFlockMemoryEventCache>(Config.MaxCachedEvents);
			Deps.LogEventCache = Cache;
			EndCache = MakeShared<FFlockMemoryEventCache>(Config.MaxCachedEvents);
			Deps.SessionEndCache = EndCache;
			EventCache = MakeShared<FFlockMemoryEventCache>(Config.MaxCachedEvents);
			Deps.AnalyticsEventCache = EventCache;
			Deps.Session = MakeShared<FFlockSession>(Config, FPaths::Combine(Dir, TEXT("session.json")));
			Deps.TerminationTracker = MakeShared<FFlockTerminationTracker>(true, MarkerPath());
			Deps.ConsentStore = MakeShared<FFlockConsentStore>(FPaths::Combine(Dir, TEXT("consent.json")));
			Deps.Pump = MakeShared<FFlockLifecyclePump>();
			Deps.bEnableLogSink = false; // a GLog tap inside the runner captures the runner's own errors
			Deps.CoverageNoticeMarkerPath = FPaths::Combine(Dir, TEXT("coverage_notice.txt"));

			ApplyRoutes();

			Provider = MakeShared<FFlockAnalyticsProvider>(Client, NoRetryPolicy(), MakeShared<FFlockNullLogger>(),
				Session, Events, TEXT("http://x/v1"), Config, Deps, TEXT("gv-1"), TEXT("0.7.0"));
		}

		FString MarkerPath() const { return FPaths::Combine(Dir, TEXT("marker.json")); }

		// ── routing ──
		// The fake matches by URL fragment in insertion order, and "analytics/sessions" is a prefix of
		// every "analytics/sessions/{id}" close URL. Routing them through here keeps the id-scoped
		// closes ahead of the generic registration route; setting them directly on the fake would
		// silently be shadowed by it, and the test would pass against the wrong response.

		FFlockHttpResponse Registration = FFlockFakeTransport::Ok(TEXT("{\"session_id\":\"srv-1\"}"));
		TMap<FString, FFlockHttpResponse> Closes;

		void ApplyRoutes()
		{
			for (const TPair<FString, FFlockHttpResponse>& Close : Closes)
			{
				Fake->On(FString::Printf(TEXT("analytics/sessions/%s"), *Close.Key), Close.Value);
			}
			Fake->On(TEXT("analytics/sessions"), Registration);
			Fake->On(TEXT("log_event"), FFlockFakeTransport::Ok(TEXT("{}")));
		}

		/** How `POST analytics/sessions` answers from now on. */
		void OnRegistration(const FFlockHttpResponse& Response)
		{
			Registration = Response;
			ApplyRoutes();
		}

		/** How `PATCH analytics/sessions/{ServerId}` answers from now on. */
		void OnClose(const FString& ServerId, const FFlockHttpResponse& Response)
		{
			Closes.Add(ServerId, Response);
			ApplyRoutes();
		}

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

		/** Every spooled log entry, oldest first, parsed. */
		TArray<FFlockLogEventRequest> AllSpooled() const
		{
			TArray<FString> Handles;
			TArray<FString> Payloads;
			Cache->PeekBatch(Cache->PendingCount(), Handles, Payloads);
			TArray<FFlockLogEventRequest> Out;
			for (const FString& Payload : Payloads)
			{
				FFlockLogEventRequest Event;
				if (FFlockAnalyticsJson::DeserializeEvent(Payload, Event))
				{
					Out.Add(MoveTemp(Event));
				}
			}
			return Out;
		}

		/** Every spooled gameplay event, oldest first, parsed. */
		TArray<FFlockSpooledAnalyticsEvent> SpooledEvents() const
		{
			TArray<FString> Handles;
			TArray<FString> Payloads;
			EventCache->PeekBatch(EventCache->PendingCount(), Handles, Payloads);
			TArray<FFlockSpooledAnalyticsEvent> Out;
			for (const FString& Payload : Payloads)
			{
				FFlockSpooledAnalyticsEvent Entry;
				if (FFlockAnalyticsJson::DeserializeSpooledAnalyticsEvent(Payload, Entry))
				{
					Out.Add(MoveTemp(Entry));
				}
			}
			return Out;
		}

		/** The events each `POST analytics/events` carried so far, one array per request, in order. */
		TArray<TArray<TSharedPtr<FJsonObject>>> SentEventBatches() const
		{
			TArray<TArray<TSharedPtr<FJsonObject>>> Out;
			for (const FFlockHttpRequest& Request : Fake->Requests)
			{
				if (!Request.Url.Contains(TEXT("analytics/events")))
				{
					continue;
				}
				TArray<TSharedPtr<FJsonObject>>& Batch = Out.AddDefaulted_GetRef();
				TSharedPtr<FJsonObject> Root;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Request.JsonBody);
				const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
				if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid() && Root->TryGetArrayField(TEXT("events"), Items))
				{
					for (const TSharedPtr<FJsonValue>& Item : *Items)
					{
						Batch.Add(Item->AsObject());
					}
				}
			}
			return Out;
		}

		/**
		 * Rebuilds the provider with the log sink on. The fixture leaves it off so other tests do not tap GLog; a test
		 * that needs automatic capture asks for it, and stops the sink with Shutdown.
		 */
		void EnableLogSink(const FFlockAnalyticsConfig& Config)
		{
			Deps.bEnableLogSink = true;
			Provider = MakeShared<FFlockAnalyticsProvider>(Client, NoRetryPolicy(), MakeShared<FFlockNullLogger>(),
				Session, Events, TEXT("http://x/v1"), Config, Deps, TEXT("gv-1"), TEXT("0.7.0"));
		}

		/** The oldest spooled session end, parsed. */
		bool FirstSpooledEnd(FFlockSessionSnapshot& OutSnapshot) const
		{
			TArray<FString> Handles;
			TArray<FString> Payloads;
			EndCache->PeekBatch(1, Handles, Payloads);
			return Payloads.Num() > 0 && FFlockAnalyticsJson::DeserializeSnapshot(Payloads[0], OutSnapshot);
		}

		/**
		 * Requests of one method whose URL contains Fragment. Both the registration POST and the close
		 * PATCH contain "analytics/sessions", so the method is the only thing that tells them apart.
		 */
		int32 CountMethod(const TCHAR* Method, const FString& Fragment) const
		{
			int32 Count = 0;
			for (const FFlockHttpRequest& Request : Fake->Requests)
			{
				if (Request.Method == Method && Request.Url.Contains(Fragment))
				{
					++Count;
				}
			}
			return Count;
		}
	};
}

namespace FlockAnalyticsProviderTestHelpers
{
	/** A string member, or "<absent>" so a missing member can never pass for an empty one. */
	inline FString JsonString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		FString Value;
		return Object.IsValid() && Object->TryGetStringField(Key, Value) ? Value : FString(TEXT("<absent>"));
	}

	/** An extra_data value, or "<absent>". */
	inline FString ExtraValue(const FFlockLogEventRequest& Event, const TCHAR* Key)
	{
		const FString* Value = Event.Data.ExtraData.Find(Key);
		return Value != nullptr ? *Value : FString(TEXT("<absent>"));
	}
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
	TestEqual(TEXT("one start call"), Fix.CountMethod(TEXT("POST"), TEXT("analytics/sessions")), 1);

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
	TestEqual(TEXT("ended against the server id"), Fix.CountMethod(TEXT("PATCH"), TEXT("analytics/sessions/srv-1")), 1);
	TestEqual(TEXT("delivered, so nothing left spooled"), Fix.EndCache->PendingCount(), 0);

	// Ending twice is harmless.
	bool bSecondEnd = false;
	Fix.Provider->EndSession(EFlockSessionEndReason::Manual, [&bSecondEnd](TFlockResult<FFlockAnalyticsAck> Result)
	{
		bSecondEnd = Result.bSuccess;
	});
	TestTrue(TEXT("second end is a no-op success"), bSecondEnd);
	TestEqual(TEXT("no extra call"), Fix.CountMethod(TEXT("PATCH"), TEXT("analytics/sessions/srv-1")), 1);
	return true;
}

/**
 * Starting while a session is open replaces it rather than ignoring the call. The old behavior handed
 * back the stale id and left the previous session running, so its metrics never reached anyone.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsSessionRestartTest, "Flock.Analytics.Provider.SessionRestart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsSessionRestartTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();

	UFlockEventTestListener* Listener = NewObject<UFlockEventTestListener>();
	Fix.Events->OnSessionEnded.AddDynamic(Listener, &UFlockEventTestListener::HandleSessionEnded);

	Fix.Provider->StartSession(TEXT("p-1"));
	const FString FirstLocalId = Fix.Provider->GetCurrentSnapshot().SessionId;

	Fix.Provider->StartSession(TEXT("p-1"));
	TestEqual(TEXT("the previous session was ended"), Listener->SessionEndedCount, 1);
	TestTrue(TEXT("reported as Restarted"),
		Listener->LastSessionEnded.Reason == EFlockSessionEndReason::Restarted);
	TestTrue(TEXT("a session is still active"), Fix.Provider->HasActiveSession());
	TestNotEqual(TEXT("and it is a new one"), Fix.Provider->GetCurrentSnapshot().SessionId, FirstLocalId);
	TestEqual(TEXT("two registrations"), Fix.CountMethod(TEXT("POST"), TEXT("analytics/sessions")), 2);
	TestEqual(TEXT("the old session was closed out"), Fix.CountMethod(TEXT("PATCH"), TEXT("analytics/sessions/")), 1);
	return true;
}

/**
 * The durability contract: an end is on disk before anything is sent, a failed send leaves it there,
 * and a later flush delivers it. This is what makes a quit, a crash, or an offline stretch cost
 * nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsSessionEndSpoolTest, "Flock.Analytics.Provider.SessionEndSpool",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsSessionEndSpoolTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();
	Fix.Provider->StartSession(TEXT("p-1"));
	Fix.Provider->RecordScreenView(TEXT("MainMenu"));

	// The close fails: offline, mid-quit, or signed out — the record must survive it.
	Fix.OnClose(TEXT("srv-1"), FFlockFakeTransport::Offline());

	bool bReportedFailure = false;
	Fix.Provider->EndSession(EFlockSessionEndReason::Quit, [&bReportedFailure](TFlockResult<FFlockAnalyticsAck> Result)
	{
		bReportedFailure = !Result.bSuccess;
	});

	TestTrue(TEXT("the caller is told it did not land"), bReportedFailure);
	TestFalse(TEXT("the session is closed locally regardless"), Fix.Provider->HasActiveSession());
	TestEqual(TEXT("still spooled after the failure"), Fix.EndCache->PendingCount(), 1);

	FFlockSessionSnapshot Spooled;
	TestTrue(TEXT("readable"), Fix.FirstSpooledEnd(Spooled));
	TestEqual(TEXT("carries the server id"), Spooled.ServerSessionId, TEXT("srv-1"));
	TestEqual(TEXT("carries the metrics"), Spooled.ScreensViewed, 1);
	TestFalse(TEXT("stored closed"), Spooled.IsActive);

	// Back online: the ordinary flush drains it, and the entry goes only once it is acknowledged.
	Fix.OnClose(TEXT("srv-1"), FFlockFakeTransport::Ok(TEXT("{}")));
	bool bDrained = false;
	Fix.Provider->Flush([&bDrained](TFlockResult<FFlockAnalyticsAck> Result) { bDrained = Result.bSuccess; });

	TestTrue(TEXT("the retry delivered it"), bDrained);
	TestEqual(TEXT("nothing left spooled"), Fix.EndCache->PendingCount(), 0);
	TestEqual(TEXT("two close attempts in total"), Fix.CountMethod(TEXT("PATCH"), TEXT("analytics/sessions/srv-1")), 2);
	return true;
}

/**
 * A session that never registered — offline at sign-in, or recovered from a run that died before the
 * POST landed — registers itself out of the spool and is then closed. The id is written back into the
 * spooled record, so a close that fails afterwards cannot open a second server session on retry.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsSessionEndRegistersTest, "Flock.Analytics.Provider.SessionEndRegisters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsSessionEndRegistersTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();

	// Registration is down when the session opens, so it runs with no server id.
	Fix.OnRegistration(FFlockFakeTransport::Offline());
	Fix.Provider->StartSession(TEXT("p-1"));
	TestTrue(TEXT("the session runs anyway"), Fix.Provider->HasActiveSession());
	TestEqual(TEXT("with no server id"), Fix.Provider->GetCurrentSessionId(), FString());

	// It ends while still unregistered, and the close cannot work without an id.
	Fix.Provider->EndSession(EFlockSessionEndReason::Quit);
	TestEqual(TEXT("spooled"), Fix.EndCache->PendingCount(), 1);

	// Registration comes back, but the close still fails. One POST must have happened, and the id it
	// returned must now be in the spooled record.
	Fix.OnRegistration(FFlockFakeTransport::Ok(TEXT("{\"session_id\":\"srv-late\"}")));
	Fix.OnClose(TEXT("srv-late"), FFlockFakeTransport::Offline());
	Fix.Provider->Flush();

	const int32 PostsAfterFirstDrain = Fix.CountMethod(TEXT("POST"), TEXT("analytics/sessions"));
	TestEqual(TEXT("still spooled"), Fix.EndCache->PendingCount(), 1);
	FFlockSessionSnapshot Spooled;
	TestTrue(TEXT("readable"), Fix.FirstSpooledEnd(Spooled));
	TestEqual(TEXT("the id was written back"), Spooled.ServerSessionId, TEXT("srv-late"));

	// The retry closes it — and must NOT register a second time.
	Fix.OnClose(TEXT("srv-late"), FFlockFakeTransport::Ok(TEXT("{}")));
	bool bDrained = false;
	Fix.Provider->Flush([&bDrained](TFlockResult<FFlockAnalyticsAck> Result) { bDrained = Result.bSuccess; });

	TestTrue(TEXT("delivered"), bDrained);
	TestEqual(TEXT("nothing left spooled"), Fix.EndCache->PendingCount(), 0);
	TestEqual(TEXT("registered exactly once"),
		Fix.CountMethod(TEXT("POST"), TEXT("analytics/sessions")), PostsAfterFirstDrain);
	TestEqual(TEXT("closed against the late id"),
		Fix.CountMethod(TEXT("PATCH"), TEXT("analytics/sessions/srv-late")), 2);
	return true;
}

/**
 * A 2xx carrying no session id is not a registration. Unguarded this recursed until the stack gave
 * out — the empty id is exactly what routes a record into the register branch, so adopting it and
 * carrying on fed the record straight back in. It cost a process crash, so it is pinned here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsSessionEndNoIdTest, "Flock.Analytics.Provider.SessionEndNoId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsSessionEndNoIdTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();

	FFlockSessionSnapshot Unregistered;
	Unregistered.SessionId = TEXT("local-1");
	Unregistered.PlayerId = TEXT("p-1");
	Unregistered.StartTimeUtc = TEXT("2026-07-22T08:00:00Z");
	Fix.EndCache->Enqueue(FFlockAnalyticsJson::SerializeSnapshot(Unregistered));

	// The backend answers 200 with nothing useful in it.
	Fix.OnRegistration(FFlockFakeTransport::Ok(TEXT("{}")));

	bool bFailed = false;
	Fix.Provider->Flush([&bFailed](TFlockResult<FFlockAnalyticsAck> Result) { bFailed = !Result.bSuccess; });

	TestTrue(TEXT("reported as a failure"), bFailed);
	TestEqual(TEXT("attempted once, did not spin"), Fix.CountMethod(TEXT("POST"), TEXT("analytics/sessions")), 1);
	TestEqual(TEXT("nothing was closed"), Fix.CountMethod(TEXT("PATCH"), TEXT("analytics/sessions/")), 0);
	// Kept, not dropped: the record is fine, it was the answer that was not.
	TestEqual(TEXT("the record survives for the next attempt"), Fix.EndCache->PendingCount(), 1);
	return true;
}

/**
 * Two records can describe one session — a quit spools its end, then a crash-recovery pass spools a
 * staler copy of the same session. Only the first is delivered.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsSessionEndSentOnceTest, "Flock.Analytics.Provider.SessionEndSentOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsSessionEndSentOnceTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();

	FFlockSessionSnapshot Snapshot;
	Snapshot.SessionId = TEXT("local-dupe");
	Snapshot.ServerSessionId = TEXT("srv-1");
	Snapshot.PlayerId = TEXT("p-1");
	Snapshot.DurationSeconds = 120.f;
	Fix.EndCache->Enqueue(FFlockAnalyticsJson::SerializeSnapshot(Snapshot));
	Snapshot.DurationSeconds = 90.f; // the staler copy
	Fix.EndCache->Enqueue(FFlockAnalyticsJson::SerializeSnapshot(Snapshot));

	// A permanently rejected record must not wedge the queue either, so one of those goes in behind.
	FFlockSessionSnapshot Rejected;
	Rejected.SessionId = TEXT("local-bad");
	Rejected.ServerSessionId = TEXT("srv-bad");
	Rejected.PlayerId = TEXT("p-1");
	Fix.EndCache->Enqueue(FFlockAnalyticsJson::SerializeSnapshot(Rejected));
	Fix.OnClose(TEXT("srv-bad"), FFlockFakeTransport::Status(400, TEXT("{}")));

	Fix.Provider->Flush();

	TestEqual(TEXT("the whole queue cleared"), Fix.EndCache->PendingCount(), 0);
	TestEqual(TEXT("the duplicate cost no request"),
		Fix.CountMethod(TEXT("PATCH"), TEXT("analytics/sessions/srv-1")), 1);
	TestEqual(TEXT("the rejected one was tried once, then dropped"),
		Fix.CountMethod(TEXT("PATCH"), TEXT("analytics/sessions/srv-bad")), 1);
	return true;
}

/**
 * A session the previous run left open is recovered at init, spooled, and delivered. Without this a
 * crashed run's session stays open on the backend forever.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsSessionRecoveryTest, "Flock.Analytics.Provider.SessionRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsSessionRecoveryTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();
	Fix.Provider->StartSession(TEXT("p-1"));
	Fix.Provider->TickForTesting(120.f);
	// The run dies here: no EndSession, no Shutdown.

	{
		// Same files, new process. A fresh directory would pass even if nothing were persisted at all.
		FFixture NextLaunch(FFlockAnalyticsConfig(), Fix.Dir);
		NextLaunch.Provider->Initialize();

		TestEqual(TEXT("the orphan was spooled"), NextLaunch.EndCache->PendingCount(), 1);
		FFlockSessionSnapshot Orphan;
		TestTrue(TEXT("readable"), NextLaunch.FirstSpooledEnd(Orphan));
		TestEqual(TEXT("with the id it had registered"), Orphan.ServerSessionId, TEXT("srv-1"));
		TestEqual(TEXT("and the duration it had reached"), Orphan.DurationSeconds, 120.f);
		TestFalse(TEXT("closed"), Orphan.IsActive);

		// Signing in drains it before the new session registers.
		NextLaunch.Provider->StartSession(TEXT("p-1"));
		TestEqual(TEXT("delivered on the next sign-in"), NextLaunch.EndCache->PendingCount(), 0);
		TestEqual(TEXT("closed against the recovered id"),
			NextLaunch.CountMethod(TEXT("PATCH"), TEXT("analytics/sessions/srv-1")), 1);

		// This run exits cleanly, so the launch after it has nothing to recover — the same orphan is
		// never reported twice.
		NextLaunch.Provider->EndSession(EFlockSessionEndReason::Quit);
		{
			FFixture ThirdLaunch(FFlockAnalyticsConfig(), Fix.Dir);
			ThirdLaunch.Provider->Initialize();
			TestEqual(TEXT("a clean exit leaves nothing to recover"), ThirdLaunch.EndCache->PendingCount(), 0);
		}
	}
	return true;
}

/**
 * A registration that failed at start is healed by the heartbeat. Without it the session can never be
 * closed on the server, and its end has to re-register itself out of the spool instead.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsRegistrationHealTest, "Flock.Analytics.Provider.RegistrationHeal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsRegistrationHealTest::RunTest(const FString& Parameters)
{
	FFlockAnalyticsConfig Config;
	Config.HeartbeatIntervalSeconds = 60.f;
	Config.EventBufferFlushIntervalSeconds = 0.f; // isolate the heartbeat
	Config.bTrackFps = false;
	FFixture Fix(Config);

	// Signed in, so the heal has a bearer to spend a request on.
	FString TokenError;
	Fix.Session->SetTokens(MakeTestJwt(TEXT("p-1")), TEXT("r-1"), TokenError);

	Fix.Provider->Initialize();
	Fix.Fake->On(TEXT("analytics/sessions"), FFlockFakeTransport::Offline());
	Fix.Provider->StartSession(TEXT("p-1"));
	TestEqual(TEXT("no server id yet"), Fix.Provider->GetCurrentSessionId(), FString());
	TestEqual(TEXT("one attempt so far"), Fix.CountMethod(TEXT("POST"), TEXT("analytics/sessions")), 1);

	// Below the interval nothing is retried.
	Fix.Provider->TickForTesting(30.f);
	TestEqual(TEXT("no retry mid-interval"), Fix.CountMethod(TEXT("POST"), TEXT("analytics/sessions")), 1);

	Fix.Fake->On(TEXT("analytics/sessions"), FFlockFakeTransport::Ok(TEXT("{\"session_id\":\"srv-healed\"}")));
	Fix.Provider->TickForTesting(31.f);
	TestEqual(TEXT("the heartbeat retried it"), Fix.CountMethod(TEXT("POST"), TEXT("analytics/sessions")), 2);
	TestEqual(TEXT("and the id was adopted"), Fix.Provider->GetCurrentSessionId(), TEXT("srv-healed"));

	// Once it has an id the heartbeat stops asking.
	Fix.Provider->TickForTesting(61.f);
	TestEqual(TEXT("no further registrations"), Fix.CountMethod(TEXT("POST"), TEXT("analytics/sessions")), 2);
	return true;
}

/**
 * Withdrawing consent discards the session instead of ending it: nothing is spooled, nothing is sent,
 * and no OnSessionEnded fires — a listener reacting to that would be reacting to data the player just
 * asked us to forget.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsConsentDiscardTest, "Flock.Analytics.Provider.ConsentDiscard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsConsentDiscardTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();
	Fix.Provider->StartSession(TEXT("p-1"));
	Fix.Provider->LogEvent(TEXT("something"));

	UFlockEventTestListener* Listener = NewObject<UFlockEventTestListener>();
	Fix.Events->OnSessionEnded.AddDynamic(Listener, &UFlockEventTestListener::HandleSessionEnded);

	const int32 CallsBefore = Fix.Fake->Requests.Num();
	Fix.Provider->SetConsent(false);

	TestFalse(TEXT("the session is closed"), Fix.Provider->HasActiveSession());
	TestEqual(TEXT("no end was spooled"), Fix.EndCache->PendingCount(), 0);
	TestEqual(TEXT("and nothing was sent"), Fix.Fake->Requests.Num(), CallsBefore);
	TestEqual(TEXT("no OnSessionEnded"), Listener->SessionEndedCount, 0);
	TestEqual(TEXT("the queued events went too"), Fix.Provider->GetPendingEventCount(), 0);
	return true;
}

/**
 * The logout case that used to lose every end: signed out, the close is refused with a 401, and auth
 * failures are never retried. The record has to stay spooled and go out after the next sign-in — so
 * this asserts that an Auth failure is not treated as a permanent rejection.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsSessionEndSurvivesAuthTest, "Flock.Analytics.Provider.SessionEndSurvivesAuth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsSessionEndSurvivesAuthTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();
	Fix.Provider->StartSession(TEXT("p-1"));

	Fix.OnClose(TEXT("srv-1"), FFlockFakeTransport::Status(401, TEXT("{}")));
	Fix.Provider->EndSession(EFlockSessionEndReason::Logout);

	// A 4xx, but not one to drop the record for: the token went bad, which is temporary.
	TestEqual(TEXT("kept for the next sign-in"), Fix.EndCache->PendingCount(), 1);

	// The 401 took the session down with it — the provider base tried a silent refresh, the refresh
	// failed, and a failed refresh clears the tokens. So the record waits for a real sign-in, not
	// merely for the route to start answering.
	TestFalse(TEXT("the 401 signed the session out"), Fix.Session->IsAuthenticated());

	Fix.OnClose(TEXT("srv-1"), FFlockFakeTransport::Ok(TEXT("{}")));
	Fix.Provider->Flush();
	TestEqual(TEXT("still waiting while signed out"), Fix.EndCache->PendingCount(), 1);

	FString TokenError;
	Fix.Session->SetTokens(MakeTestJwt(TEXT("p-1")), TEXT("r-1"), TokenError);
	Fix.Provider->Flush();
	TestEqual(TEXT("delivered on the next sign-in"), Fix.EndCache->PendingCount(), 0);
	return true;
}

/**
 * Signed out, the drain does not run at all. Every session route needs a bearer, so attempting one
 * is guaranteed-wasted traffic — and on the flush interval it produced a 401 error line every few
 * seconds for the whole run, which is what this pins.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsSessionEndWaitsForAuthTest, "Flock.Analytics.Provider.SessionEndWaitsForAuth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsSessionEndWaitsForAuthTest::RunTest(const FString& Parameters)
{
	FFlockAnalyticsConfig Config;
	Config.EventBufferFlushIntervalSeconds = 10.f;
	Config.bTrackFps = false;
	FFixture Fix(Config);
	Fix.Provider->Initialize();

	// A previous run left an end behind, and nobody has signed in yet this run.
	FFlockSessionSnapshot Orphan;
	Orphan.SessionId = TEXT("local-old");
	Orphan.ServerSessionId = TEXT("srv-old");
	Orphan.PlayerId = TEXT("p-1");
	Fix.EndCache->Enqueue(FFlockAnalyticsJson::SerializeSnapshot(Orphan));
	Fix.Session->ClearTokens();

	const int32 CallsBefore = Fix.Fake->Requests.Num();
	Fix.Provider->Flush();
	Fix.Provider->TickForTesting(11.f); // crosses the flush interval
	Fix.Provider->TickForTesting(11.f);

	TestEqual(TEXT("no request while signed out"), Fix.Fake->Requests.Num(), CallsBefore);
	TestEqual(TEXT("and the record is still waiting"), Fix.EndCache->PendingCount(), 1);

	// Signing in is the first moment it could have worked, and it goes then.
	FString TokenError;
	Fix.Session->SetTokens(MakeTestJwt(TEXT("p-1")), TEXT("r-1"), TokenError);
	Fix.Provider->Flush();

	TestEqual(TEXT("delivered on sign-in"), Fix.EndCache->PendingCount(), 0);
	TestEqual(TEXT("closed against its id"), Fix.CountMethod(TEXT("PATCH"), TEXT("analytics/sessions/srv-old")), 1);
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

// ── Gameplay events: POST analytics/events ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsTrackEventShapeTest, "Flock.Analytics.Provider.TrackEvent.Shape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsTrackEventShapeTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();
	Fix.Provider->StartSession(TEXT("p-fixture"));
	TestEqual(TEXT("precondition: the session registered"), Fix.Provider->GetCurrentSessionId(), TEXT("srv-1"));

	FFlockCommandData Properties;
	Properties.Set(TEXT("level"), 3).Set(TEXT("won"), true).Set(TEXT("playerLevel"), TEXT("gold"));
	TestTrue(TEXT("accepted"), Fix.Provider->TrackEvent(TEXT("level_complete"), Properties, TEXT("progression")));

	// Recording never touches the network.
	TestEqual(TEXT("spooled"), Fix.Provider->GetPendingAnalyticsEventCount(), 1);
	TestEqual(TEXT("not sent"), Fix.Fake->CountTo(TEXT("analytics/events")), 0);

	bool bFlushed = false;
	Fix.Provider->Flush([&bFlushed](TFlockResult<FFlockAnalyticsAck> Result) { bFlushed = Result.bSuccess; });
	TestTrue(TEXT("flush succeeded"), bFlushed);
	TestEqual(TEXT("delivered"), Fix.Provider->GetPendingAnalyticsEventCount(), 0);

	const TArray<TArray<TSharedPtr<FJsonObject>>> Batches = Fix.SentEventBatches();
	TestEqual(TEXT("one request"), Batches.Num(), 1);
	TestEqual(TEXT("one event in it"), FlockTestAt(Batches, 0).Num(), 1);
	const TSharedPtr<FJsonObject> Event = FlockTestAt(FlockTestAt(Batches, 0), 0);
	TestEqual(TEXT("player"), JsonString(Event, TEXT("player_id")), TEXT("p-fixture"));
	TestEqual(TEXT("name"), JsonString(Event, TEXT("event_name")), TEXT("level_complete"));
	TestEqual(TEXT("category"), JsonString(Event, TEXT("event_category")), TEXT("progression"));
	TestEqual(TEXT("the player's own session"), JsonString(Event, TEXT("session_id")), TEXT("srv-1"));
	TestNotEqual(TEXT("timestamped"), JsonString(Event, TEXT("timestamp")), FString(TEXT("<absent>")));

	const TSharedPtr<FJsonObject>* Props = nullptr;
	const bool bHasProps = Event.IsValid() && Event->TryGetObjectField(TEXT("properties"), Props) && Props->IsValid();
	TestTrue(TEXT("properties is an object"), bHasProps);
	if (bHasProps)
	{
		TestTrue(TEXT("an int stays a number"), (*Props)->HasTypedField<EJson::Number>(TEXT("level")));
		TestTrue(TEXT("a bool stays a bool"), (*Props)->HasTypedField<EJson::Boolean>(TEXT("won")));
		TestTrue(TEXT("keys kept verbatim"), (*Props)->HasField(TEXT("playerLevel")));
	}

	// Spool bookkeeping never reaches the wire.
	const FFlockHttpRequest* Sent = Fix.Fake->Requests.FindByPredicate(
		[](const FFlockHttpRequest& Request) { return Request.Url.Contains(TEXT("analytics/events")); });
	TestTrue(TEXT("no attempt counter on the wire"), Sent != nullptr && !Sent->JsonBody.Contains(TEXT("_flock_failed_sends")));
	TestTrue(TEXT("no local session id on the wire"), Sent != nullptr && !Sent->JsonBody.Contains(TEXT("local_session_id")));

	// Nothing optional given: the category is omitted, and properties is still an object.
	Fix.Provider->TrackEvent(TEXT("menu_open"));
	Fix.Provider->Flush();
	const TArray<TArray<TSharedPtr<FJsonObject>>> After = Fix.SentEventBatches();
	TestEqual(TEXT("second request"), After.Num(), 2);
	const TSharedPtr<FJsonObject> Bare = FlockTestAt(FlockTestAt(After, 1), 0);
	TestTrue(TEXT("precondition: the bare event was sent"), Bare.IsValid());
	TestFalse(TEXT("no category member"), Bare.IsValid() && Bare->HasField(TEXT("event_category")));
	TestTrue(TEXT("empty properties still an object"), Bare.IsValid() && Bare->HasTypedField<EJson::Object>(TEXT("properties")));

	// A player switch without the session ending: the previous player's session is never attached to the new one's event.
	FString TokenError;
	Fix.Session->SetTokens(MakeTestJwt(TEXT("p-other")), TEXT("r-o"), TokenError);
	TestTrue(TEXT("precondition: the previous session is still open"), Fix.Provider->HasActiveSession());
	Fix.Provider->TrackEvent(TEXT("after_switch"));
	const TArray<FFlockSpooledAnalyticsEvent> Switched = Fix.SpooledEvents();
	TestEqual(TEXT("recorded under the new player"), FlockTestAt(Switched, 0).Event.PlayerId, TEXT("p-other"));
	TestTrue(TEXT("without the previous player's session id"), FlockTestAt(Switched, 0).Event.SessionId.IsEmpty());
	TestTrue(TEXT("or its local session"), FlockTestAt(Switched, 0).LocalSessionId.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsTrackEventRefusalTest, "Flock.Analytics.Provider.TrackEvent.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsTrackEventRefusalTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();

	TestFalse(TEXT("an empty name is refused"), Fix.Provider->TrackEvent(FString()));
	TestFalse(TEXT("a blank name is refused"), Fix.Provider->TrackEvent(TEXT("  \t ")));
	// The server writes session_started when a session starts, and would count a client's copy as a second session.
	TestFalse(TEXT("session_started is refused"), Fix.Provider->TrackEvent(FFlockAnalyticsProvider::ReservedSessionStartedEvent));
	// The server cannot store a longer name or category, and fails the whole request for one.
	const FString LongestName = FString::ChrN(FFlockAnalyticsProvider::MaxEventNameLength, TEXT('n'));
	const FString LongestCategory = FString::ChrN(FFlockAnalyticsProvider::MaxEventCategoryLength, TEXT('c'));
	TestFalse(TEXT("a name one character over the limit is refused"), Fix.Provider->TrackEvent(LongestName + TEXT("n")));
	TestFalse(TEXT("a category one character over the limit is refused"),
		Fix.Provider->TrackEvent(TEXT("level_complete"), FFlockCommandData(), LongestCategory + TEXT("c")));
	TestEqual(TEXT("nothing spooled"), Fix.Provider->GetPendingAnalyticsEventCount(), 0);

	// Only that exact name is reserved.
	TestTrue(TEXT("a longer name is fine"), Fix.Provider->TrackEvent(TEXT("session_started_tutorial")));
	TestTrue(TEXT("a different case is a different name"), Fix.Provider->TrackEvent(TEXT("Session_Started")));
	TestTrue(TEXT("session_end is not reserved"), Fix.Provider->TrackEvent(TEXT("session_end")));
	TestTrue(TEXT("the longest name and category the server stores are fine"),
		Fix.Provider->TrackEvent(LongestName, FFlockCommandData(), LongestCategory));
	TestEqual(TEXT("all four spooled"), Fix.Provider->GetPendingAnalyticsEventCount(), 4);

	Fix.Provider->EraseLocalData();
	TestEqual(TEXT("erasing local data takes them"), Fix.Provider->GetPendingAnalyticsEventCount(), 0);

	// Consent is the same hard gate it is for everything else.
	{
		FFlockAnalyticsConfig Config;
		Config.bRequireExplicitConsent = true;
		FFixture Gated(Config);
		Gated.Provider->Initialize();
		TestFalse(TEXT("refused without consent"), Gated.Provider->TrackEvent(TEXT("level_complete")));
		TestEqual(TEXT("and not held either"), Gated.Provider->GetPendingAnalyticsEventCount(), 0);
		Gated.Provider->SetConsent(true);
		TestTrue(TEXT("accepted once granted"), Gated.Provider->TrackEvent(TEXT("level_complete")));
		Gated.Provider->SetConsent(false);
		TestEqual(TEXT("withdrawing drops spooled gameplay events"), Gated.Provider->GetPendingAnalyticsEventCount(), 0);
	}

	{
		FFlockAnalyticsConfig Config;
		Config.bEnabled = false;
		FFixture Off(Config);
		Off.Provider->Initialize();
		TestFalse(TEXT("refused when analytics is off"), Off.Provider->TrackEvent(TEXT("level_complete")));
		TestEqual(TEXT("nothing spooled when off"), Off.Provider->GetPendingAnalyticsEventCount(), 0);
	}
	return true;
}

/**
 * An event recorded while nobody is signed in is held — the server refuses one with no player — and belongs to whoever
 * signs in next. One recorded earlier keeps the player it was recorded under, and each player's events travel apart.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsTrackEventAttributionTest, "Flock.Analytics.Provider.TrackEvent.Attribution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsTrackEventAttributionTest::RunTest(const FString& Parameters)
{
	{
		FFixture Out;
		Out.Session->ClearTokens();
		Out.Provider->Initialize();
		TestTrue(TEXT("accepted while signed out"), Out.Provider->TrackEvent(TEXT("title_screen")));
		Out.Provider->Flush();
		TestEqual(TEXT("a held event costs no request"), Out.Fake->CountTo(TEXT("analytics/events")), 0);
		TestEqual(TEXT("and stays held"), Out.Provider->GetPendingAnalyticsEventCount(), 1);
		Out.Provider->HandleAuthenticated(FString());
		TestTrue(TEXT("an empty sign-in attributes nothing"), FlockTestAt(Out.SpooledEvents(), 0).Event.PlayerId.IsEmpty());
		TestEqual(TEXT("and sends nothing"), Out.Fake->CountTo(TEXT("analytics/events")), 0);
	}

	// An event that already has its player waits while nobody is signed in, and goes once someone is.
	{
		FFixture Waiting;
		Waiting.Provider->Initialize();
		Waiting.Provider->TrackEvent(TEXT("played_then_signed_out"));
		Waiting.Session->ClearTokens();
		Waiting.Provider->Flush();
		TestEqual(TEXT("nothing sent while nobody is signed in"), Waiting.Fake->CountTo(TEXT("analytics/events")), 0);
		FString TokenError;
		Waiting.Session->SetTokens(MakeTestJwt(TEXT("p-fixture")), TEXT("r-1"), TokenError);
		Waiting.Provider->Flush();
		TestEqual(TEXT("sent once someone is"), Waiting.Fake->CountTo(TEXT("analytics/events")), 1);
	}

	// A held event is never sent without a player, even after a sign-in that nobody told analytics about.
	{
		FFixture Unattributed;
		Unattributed.Provider->Initialize();
		Unattributed.Session->ClearTokens();
		Unattributed.Provider->TrackEvent(TEXT("held_through_a_quiet_sign_in"));
		FString TokenError;
		Unattributed.Session->SetTokens(MakeTestJwt(TEXT("p-fixture")), TEXT("r-1"), TokenError);
		Unattributed.Provider->Flush();
		TestEqual(TEXT("a held event is not sent without a player"), Unattributed.Fake->CountTo(TEXT("analytics/events")), 0);
		TestEqual(TEXT("and stays held"), Unattributed.Provider->GetPendingAnalyticsEventCount(), 1);
	}

	FFixture Fix;
	Fix.Provider->Initialize();

	// Played as p-fixture while the route was down, then signed out and kept playing.
	Fix.Fake->On(TEXT("analytics/events"), FFlockFakeTransport::Offline());
	Fix.Provider->TrackEvent(TEXT("played_as_fixture"));
	Fix.Session->ClearTokens();
	Fix.Provider->TrackEvent(TEXT("played_signed_out"));

	TArray<FFlockSpooledAnalyticsEvent> Spooled = Fix.SpooledEvents();
	TestEqual(TEXT("both spooled"), Spooled.Num(), 2);
	TestEqual(TEXT("recorded under the signed-in player"), FlockTestAt(Spooled, 0).Event.PlayerId, TEXT("p-fixture"));
	TestTrue(TEXT("held without a player"), FlockTestAt(Spooled, 1).Event.PlayerId.IsEmpty());
	TestTrue(TEXT("a held event carries no session"), FlockTestAt(Spooled, 1).LocalSessionId.IsEmpty());

	FString TokenError;
	Fix.Session->SetTokens(MakeTestJwt(TEXT("p-2")), TEXT("r-2"), TokenError);
	Fix.Provider->HandleAuthenticated(TEXT("p-2"));

	Spooled = Fix.SpooledEvents();
	TestEqual(TEXT("already attributed: keeps its player"), FlockTestAt(Spooled, 0).Event.PlayerId, TEXT("p-fixture"));
	TestEqual(TEXT("held: now the new player's"), FlockTestAt(Spooled, 1).Event.PlayerId, TEXT("p-2"));

	// Back online: one request per player, never a mixed batch.
	Fix.Fake->On(TEXT("analytics/events"), FFlockFakeTransport::Ok(TEXT("{\"ok\":true}")));
	const int32 Before = Fix.Fake->CountTo(TEXT("analytics/events"));
	bool bFlushed = false;
	Fix.Provider->Flush([&bFlushed](TFlockResult<FFlockAnalyticsAck> Result) { bFlushed = Result.bSuccess; });
	TestTrue(TEXT("delivered"), bFlushed);
	TestEqual(TEXT("nothing left"), Fix.Provider->GetPendingAnalyticsEventCount(), 0);
	TestEqual(TEXT("one request per player"), Fix.Fake->CountTo(TEXT("analytics/events")) - Before, 2);

	const TArray<TArray<TSharedPtr<FJsonObject>>> Batches = Fix.SentEventBatches();
	const TArray<TSharedPtr<FJsonObject>> ForFixture = FlockTestAt(Batches, Batches.Num() - 2);
	const TArray<TSharedPtr<FJsonObject>> ForSecond = FlockTestAt(Batches, Batches.Num() - 1);
	TestEqual(TEXT("first batch holds one event"), ForFixture.Num(), 1);
	TestEqual(TEXT("first batch is p-fixture's"), JsonString(FlockTestAt(ForFixture, 0), TEXT("player_id")), TEXT("p-fixture"));
	TestEqual(TEXT("second batch holds one event"), ForSecond.Num(), 1);
	TestEqual(TEXT("second batch is p-2's"), JsonString(FlockTestAt(ForSecond, 0), TEXT("player_id")), TEXT("p-2"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsTrackEventRefusedPlayerTest, "Flock.Analytics.Provider.TrackEvent.RefusedPlayer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsTrackEventRefusedPlayerTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();
	Fix.Provider->TrackEvent(TEXT("from_deleted_player"));
	FString TokenError;
	Fix.Session->SetTokens(MakeTestJwt(TEXT("p-b")), TEXT("r-b"), TokenError);
	Fix.Provider->TrackEvent(TEXT("from_live_player"));

	// The route refuses a whole batch for one player it does not know. That must cost only that player's events.
	Fix.Fake->OnSequence(TEXT("analytics/events"),
		{ FFlockFakeTransport::Coded(404, TEXT("analytics.player_not_found")), FFlockFakeTransport::Ok(TEXT("{\"ok\":true}")) });
	bool bFlushed = true;
	FFlockError Reported;
	Fix.Provider->Flush([&bFlushed, &Reported](TFlockResult<FFlockAnalyticsAck> Result)
	{
		bFlushed = Result.bSuccess;
		Reported = Result.Error;
	});
	TestFalse(TEXT("the refusal is reported: those events were lost"), bFlushed);
	TestEqual(TEXT("by the server's code"), Reported.Code, TEXT("analytics.player_not_found"));
	TestEqual(TEXT("refused events dropped, not retried forever"), Fix.Provider->GetPendingAnalyticsEventCount(), 0);
	const TArray<TArray<TSharedPtr<FJsonObject>>> Batches = Fix.SentEventBatches();
	TestEqual(TEXT("two requests"), Batches.Num(), 2);
	TestEqual(TEXT("the refused batch was p-fixture's"),
		JsonString(FlockTestAt(FlockTestAt(Batches, 0), 0), TEXT("player_id")), TEXT("p-fixture"));
	TestEqual(TEXT("the other player's events still went"),
		JsonString(FlockTestAt(FlockTestAt(Batches, 1), 0), TEXT("player_id")), TEXT("p-b"));

	// A failure the server did not decide stops the pass and keeps everything.
	{
		FFixture Busy;
		Busy.Provider->Initialize();
		Busy.Provider->TrackEvent(TEXT("from_a"));
		FString Error;
		Busy.Session->SetTokens(MakeTestJwt(TEXT("p-b")), TEXT("r-b"), Error);
		Busy.Provider->TrackEvent(TEXT("from_b"));
		Busy.Fake->On(TEXT("analytics/events"), FFlockFakeTransport::Status(500, TEXT("{}")));
		bool bFailed = false;
		Busy.Provider->Flush([&bFailed](TFlockResult<FFlockAnalyticsAck> Result) { bFailed = !Result.bSuccess; });
		TestTrue(TEXT("the failure is reported"), bFailed);
		TestEqual(TEXT("both kept"), Busy.Provider->GetPendingAnalyticsEventCount(), 2);
		TestEqual(TEXT("the pass stopped at the first player"), Busy.Fake->CountTo(TEXT("analytics/events")), 1);
		const TArray<FFlockSpooledAnalyticsEvent> Spooled = Busy.SpooledEvents();
		TestEqual(TEXT("the sent event counts a failed send"), FlockTestAt(Spooled, 0).FailedSends, 1);
		TestEqual(TEXT("the unsent one is not"), FlockTestAt(Spooled, 1).FailedSends, 0);
	}
	return true;
}

/**
 * A build that did not check lengths may have queued an event the server cannot store. It is dropped when found, never
 * sent: the server fails the whole request for it, so every event beside it would be held back until its attempts ran out.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsTrackEventTooLongToStoreTest, "Flock.Analytics.Provider.TrackEvent.TooLongToStoreIsNotSent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsTrackEventTooLongToStoreTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();
	Fix.Provider->TrackEventAsPlayerForTesting(TEXT("p-fixture"), FString::ChrN(FFlockAnalyticsProvider::MaxEventNameLength + 1, TEXT('n')));
	Fix.Provider->TrackEventAsPlayerForTesting(TEXT("p-fixture"), FString::ChrN(FFlockAnalyticsProvider::MaxEventNameLength, TEXT('n')));
	TestTrue(TEXT("an ordinary event is accepted"), Fix.Provider->TrackEvent(TEXT("level_complete")));
	TestEqual(TEXT("precondition: all three queued"), Fix.Provider->GetPendingAnalyticsEventCount(), 3);

	Fix.Fake->On(TEXT("analytics/events"), FFlockFakeTransport::Ok(TEXT("{\"ok\":true}")));
	bool bFlushed = false;
	Fix.Provider->Flush([&bFlushed](TFlockResult<FFlockAnalyticsAck> Result) { bFlushed = Result.bSuccess; });
	TestTrue(TEXT("the flush succeeded"), bFlushed);
	TestEqual(TEXT("nothing is left queued"), Fix.Provider->GetPendingAnalyticsEventCount(), 0);

	const TArray<TArray<TSharedPtr<FJsonObject>>> Batches = Fix.SentEventBatches();
	TestEqual(TEXT("one request"), Batches.Num(), 1);
	TestEqual(TEXT("carrying the two events the server can store"), FlockTestAt(Batches, 0).Num(), 2);
	for (const TSharedPtr<FJsonObject>& Event : FlockTestAt(Batches, 0))
	{
		TestTrue(TEXT("no event name longer than the server stores"),
			JsonString(Event, TEXT("event_name")).Len() <= FFlockAnalyticsProvider::MaxEventNameLength);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsTrackEventSessionConflictTest, "Flock.Analytics.Provider.TrackEvent.SessionConflict",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsTrackEventSessionConflictTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();
	Fix.Provider->StartSession(TEXT("p-fixture"));
	Fix.Provider->TrackEvent(TEXT("in_session"));

	// A session id the server does not know answers 409. The event itself is fine, so it goes once more without one.
	Fix.Fake->OnSequence(TEXT("analytics/events"),
		{ FFlockFakeTransport::Coded(409, TEXT("request.conflict")), FFlockFakeTransport::Ok(TEXT("{\"ok\":true}")) });
	bool bFlushed = false;
	Fix.Provider->Flush([&bFlushed](TFlockResult<FFlockAnalyticsAck> Result) { bFlushed = Result.bSuccess; });
	TestTrue(TEXT("delivered"), bFlushed);
	TestEqual(TEXT("nothing left"), Fix.Provider->GetPendingAnalyticsEventCount(), 0);
	const TArray<TArray<TSharedPtr<FJsonObject>>> Batches = Fix.SentEventBatches();
	TestEqual(TEXT("sent twice"), Batches.Num(), 2);
	TestEqual(TEXT("first with the session"), JsonString(FlockTestAt(FlockTestAt(Batches, 0), 0), TEXT("session_id")), TEXT("srv-1"));
	TestEqual(TEXT("then without it"), JsonString(FlockTestAt(FlockTestAt(Batches, 1), 0), TEXT("session_id")), TEXT("<absent>"));
	TestEqual(TEXT("the same event"), JsonString(FlockTestAt(FlockTestAt(Batches, 1), 0), TEXT("event_name")), TEXT("in_session"));

	// The live session keeps that id for its own end, but no later event carries it into another refusal.
	Fix.Provider->TrackEvent(TEXT("later_in_session"));
	const TArray<FFlockSpooledAnalyticsEvent> Later = Fix.SpooledEvents();
	TestTrue(TEXT("recorded without the refused session id"), FlockTestAt(Later, 0).Event.SessionId.IsEmpty());
	TestTrue(TEXT("or its local session"), FlockTestAt(Later, 0).LocalSessionId.IsEmpty());
	Fix.Provider->Flush();
	TestEqual(TEXT("sent once, with no second refusal"), Fix.Fake->CountTo(TEXT("analytics/events")), 3);

	// A second 409 is a refusal, not a loop.
	{
		FFixture Twice;
		Twice.Provider->Initialize();
		Twice.Provider->StartSession(TEXT("p-fixture"));
		Twice.Provider->TrackEvent(TEXT("in_session"));
		Twice.Fake->On(TEXT("analytics/events"), FFlockFakeTransport::Coded(409, TEXT("request.conflict")));
		Twice.Provider->Flush();
		TestEqual(TEXT("retried once, not again"), Twice.Fake->CountTo(TEXT("analytics/events")), 2);
		TestEqual(TEXT("then dropped"), Twice.Provider->GetPendingAnalyticsEventCount(), 0);
	}

	// Without a session id there is nothing to strip, so a 409 is a refusal at once.
	{
		FFixture Sessionless;
		Sessionless.Provider->Initialize();
		Sessionless.Provider->TrackEvent(TEXT("no_session"));
		Sessionless.Fake->On(TEXT("analytics/events"), FFlockFakeTransport::Coded(409, TEXT("request.conflict")));
		Sessionless.Provider->Flush();
		TestEqual(TEXT("not retried"), Sessionless.Fake->CountTo(TEXT("analytics/events")), 1);
		TestEqual(TEXT("dropped"), Sessionless.Provider->GetPendingAnalyticsEventCount(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsTrackEventSessionIdAtDrainTest, "Flock.Analytics.Provider.TrackEvent.SessionIdAtDrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsTrackEventSessionIdAtDrainTest::RunTest(const FString& Parameters)
{
	FFlockAnalyticsConfig Config;
	Config.HeartbeatIntervalSeconds = 1.f;
	Config.EventBufferFlushIntervalSeconds = 0.f;
	Config.bTrackFps = false;
	FFixture Fix(Config);
	Fix.OnRegistration(FFlockFakeTransport::Offline());
	Fix.Provider->Initialize();
	Fix.Provider->StartSession(TEXT("p-fixture"));
	TestTrue(TEXT("precondition: the session is open locally"), Fix.Provider->HasActiveSession());
	TestTrue(TEXT("precondition: the server has not named it"), Fix.Provider->GetCurrentSessionId().IsEmpty());

	Fix.Provider->TrackEvent(TEXT("before_registration"));
	const TArray<FFlockSpooledAnalyticsEvent> Spooled = Fix.SpooledEvents();
	TestTrue(TEXT("no server id to record yet"), FlockTestAt(Spooled, 0).Event.SessionId.IsEmpty());
	TestFalse(TEXT("the local session is remembered"), FlockTestAt(Spooled, 0).LocalSessionId.IsEmpty());

	// The heartbeat registers the session later.
	Fix.OnRegistration(FFlockFakeTransport::Ok(TEXT("{\"session_id\":\"srv-late\"}")));
	Fix.Provider->TickForTesting(1.f);
	TestEqual(TEXT("precondition: registered"), Fix.Provider->GetCurrentSessionId(), TEXT("srv-late"));

	Fix.Provider->Flush();
	TestEqual(TEXT("sent with the id the server knows the session by"),
		JsonString(FlockTestAt(FlockTestAt(Fix.SentEventBatches(), 0), 0), TEXT("session_id")), TEXT("srv-late"));

	// An event recorded outside any session carries none.
	Fix.Provider->EndSession();
	Fix.Provider->TrackEvent(TEXT("after_session"));
	Fix.Provider->Flush();
	const TArray<TArray<TSharedPtr<FJsonObject>>> Batches = Fix.SentEventBatches();
	TestEqual(TEXT("precondition: sent"), JsonString(FlockTestAt(FlockTestAt(Batches, Batches.Num() - 1), 0), TEXT("event_name")),
		TEXT("after_session"));
	TestEqual(TEXT("no session once it has ended"),
		JsonString(FlockTestAt(FlockTestAt(Batches, Batches.Num() - 1), 0), TEXT("session_id")), TEXT("<absent>"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsTrackEventIndependentDeliveryTest, "Flock.Analytics.Provider.TrackEvent.IndependentDelivery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsTrackEventIndependentDeliveryTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Session->ClearTokens();
	Fix.Provider->Initialize();
	Fix.Provider->LogEvent(TEXT("a diagnostic"));
	Fix.Provider->TrackEvent(TEXT("held_gameplay"));

	Fix.Fake->bDeferred = true;
	Fix.Provider->Flush();
	TestEqual(TEXT("precondition: the log batch is away"), Fix.Fake->CountTo(TEXT("log_event")), 1);
	TestEqual(TEXT("precondition: no gameplay request yet"), Fix.Fake->CountTo(TEXT("analytics/events")), 0);

	// Signing in mid-flush delivers the held events at once: a log batch in flight must not hold them back.
	FString TokenError;
	Fix.Session->SetTokens(MakeTestJwt(TEXT("p-2")), TEXT("r-2"), TokenError);
	Fix.Provider->HandleAuthenticated(TEXT("p-2"));
	TestEqual(TEXT("sent while the log batch is still away"), Fix.Fake->CountTo(TEXT("analytics/events")), 1);

	// When the log batch lands the flush reaches the events queue, which is already being delivered.
	Fix.Fake->FlushPending();
	Fix.Fake->FlushPending();
	TestEqual(TEXT("sent once, not twice"), Fix.Fake->CountTo(TEXT("analytics/events")), 1);
	TestEqual(TEXT("log spool drained"), Fix.Provider->GetPendingEventCount(), 0);
	TestEqual(TEXT("events spool drained"), Fix.Provider->GetPendingAnalyticsEventCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsTrackEventAttemptBudgetTest, "Flock.Analytics.Provider.TrackEvent.AttemptBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsTrackEventAttemptBudgetTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();
	Fix.Provider->TrackEvent(TEXT("stuck"));

	// Unanswered sends are free: a flush fires every interval, and hours offline must not cost the event.
	Fix.Fake->On(TEXT("analytics/events"), FFlockFakeTransport::Offline());
	for (int32 Index = 0; Index < FFlockAnalyticsProvider::MaxFailedSends + 5; ++Index)
	{
		Fix.Provider->Flush();
	}
	Fix.Fake->On(TEXT("analytics/events"), FFlockFakeTransport::Timeout());
	Fix.Provider->Flush();
	TestEqual(TEXT("kept through any number of unanswered sends"), Fix.Provider->GetPendingAnalyticsEventCount(), 1);
	TestEqual(TEXT("and counted nothing"), FlockTestAt(Fix.SpooledEvents(), 0).FailedSends, 0);

	// Answered failures are counted, and the budget ends it.
	Fix.Fake->On(TEXT("analytics/events"), FFlockFakeTransport::Status(500, TEXT("{}")));
	for (int32 Index = 1; Index < FFlockAnalyticsProvider::MaxFailedSends; ++Index)
	{
		Fix.Provider->Flush();
	}
	TestEqual(TEXT("kept below the budget"), Fix.Provider->GetPendingAnalyticsEventCount(), 1);
	TestEqual(TEXT("each answered failure counted"), FlockTestAt(Fix.SpooledEvents(), 0).FailedSends,
		FFlockAnalyticsProvider::MaxFailedSends - 1);
	Fix.Provider->Flush();
	TestEqual(TEXT("dropped when the budget is spent"), Fix.Provider->GetPendingAnalyticsEventCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsLogSpoolRefusalTest, "Flock.Analytics.Provider.LogSpoolRefusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsLogSpoolRefusalTest::RunTest(const FString& Parameters)
{
	FFlockAnalyticsConfig Config;
	Config.CacheFlushBatchSize = 1;
	FFixture Fix(Config);
	Fix.Provider->Initialize();
	Fix.Provider->LogEvent(TEXT("refused"));
	Fix.Provider->LogEvent(TEXT("accepted"));

	// A body the server refuses is refused on every flush, forever, with every later entry queued behind it.
	Fix.Fake->OnSequence(TEXT("log_event"),
		{ FFlockFakeTransport::Coded(422, TEXT("request.validation_failed")), FFlockFakeTransport::Ok(TEXT("{}")) });
	bool bFlushed = false;
	FFlockError Reported;
	Fix.Provider->Flush([&bFlushed, &Reported](TFlockResult<FFlockAnalyticsAck> Result)
	{
		bFlushed = Result.bSuccess;
		Reported = Result.Error;
	});
	TestFalse(TEXT("the refusal is reported: an entry was lost"), bFlushed);
	TestEqual(TEXT("by the server's code"), Reported.Code, TEXT("request.validation_failed"));
	TestEqual(TEXT("the refused entry went"), Fix.Provider->GetPendingEventCount(), 0);
	TestEqual(TEXT("and the next one was delivered"), Fix.Fake->CountTo(TEXT("log_event")), 2);

	// A failure the server did not decide keeps the entry.
	Fix.Provider->LogEvent(TEXT("kept"));
	const auto FailedSendsOnDisk = [&Fix]()
	{
		TArray<FString> Handles;
		TArray<FString> Payloads;
		Fix.Cache->PeekBatch(1, Handles, Payloads);
		return Payloads.Num() > 0 ? FFlockAnalyticsJson::ReadFailedSendCount(Payloads[0]) : -1;
	};

	Fix.Fake->On(TEXT("log_event"), FFlockFakeTransport::Offline());
	Fix.Provider->Flush();
	TestEqual(TEXT("kept while unreachable"), Fix.Provider->GetPendingEventCount(), 1);
	TestEqual(TEXT("an unanswered send is free"), FailedSendsOnDisk(), 0);

	Fix.Fake->On(TEXT("log_event"), FFlockFakeTransport::Status(500, TEXT("{}")));
	Fix.Provider->Flush();
	TestEqual(TEXT("kept after a server error"), Fix.Provider->GetPendingEventCount(), 1);
	TestEqual(TEXT("an answered failure is counted"), FailedSendsOnDisk(), 1);

	// A bare 403 is a proxy between the game and the server, not the server refusing the entry.
	Fix.Fake->On(TEXT("log_event"), FFlockFakeTransport::Status(403, TEXT("<html>Forbidden</html>")));
	Fix.Provider->Flush();
	TestEqual(TEXT("kept after a bare 403"), Fix.Provider->GetPendingEventCount(), 1);

	// The counter is the spool's, never the server's.
	Fix.Fake->On(TEXT("log_event"), FFlockFakeTransport::Status(500, TEXT("{}")));
	Fix.Provider->Flush();
	TestTrue(TEXT("precondition: the entry sent had already been counted"), FailedSendsOnDisk() >= 3);
	TestFalse(TEXT("the attempt counter never reaches the wire"),
		Fix.Fake->Requests.Last().JsonBody.Contains(TEXT("_flock_failed_sends")));

	// The answered budget still ends it.
	for (int32 Guard = 0; Guard < FFlockAnalyticsProvider::MaxFailedSends
		&& FailedSendsOnDisk() >= 0 && FailedSendsOnDisk() < FFlockAnalyticsProvider::MaxFailedSends - 1; ++Guard)
	{
		Fix.Provider->Flush();
	}
	TestEqual(TEXT("kept below the budget"), Fix.Provider->GetPendingEventCount(), 1);
	TestEqual(TEXT("counted up to the limit"), FailedSendsOnDisk(), FFlockAnalyticsProvider::MaxFailedSends - 1);
	Fix.Provider->Flush();
	TestEqual(TEXT("dropped when the budget is spent"), Fix.Provider->GetPendingEventCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsCachingOffTest, "Flock.Analytics.Provider.CachingOff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsCachingOffTest::RunTest(const FString& Parameters)
{
	FFlockAnalyticsConfig Config;
	Config.MaxCachedEvents = 0; // what switching caching off builds: a spool that stores nothing
	FFixture Fix(Config);
	Fix.Provider->Initialize();

	Fix.Provider->LogEvent(TEXT("unspoolable diagnostic"));
	TestEqual(TEXT("a log entry the spool cannot take is sent at once"), Fix.Fake->CountTo(TEXT("log_event")), 1);
	TestEqual(TEXT("and is not pending"), Fix.Provider->GetPendingEventCount(), 0);

	TestTrue(TEXT("a gameplay event is accepted"), Fix.Provider->TrackEvent(TEXT("unspoolable_event")));
	TestEqual(TEXT("and sent at once"), Fix.Fake->CountTo(TEXT("analytics/events")), 1);
	TestEqual(TEXT("with its player"),
		JsonString(FlockTestAt(FlockTestAt(Fix.SentEventBatches(), 0), 0), TEXT("player_id")), TEXT("p-fixture"));

	// Nobody signed in and nowhere to hold it: refused, rather than sent to a certain refusal.
	Fix.Session->ClearTokens();
	TestFalse(TEXT("refused"), Fix.Provider->TrackEvent(TEXT("orphan")));
	TestEqual(TEXT("nothing sent"), Fix.Fake->CountTo(TEXT("analytics/events")), 1);
	return true;
}

// ── Exception capture ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsRepeatedExceptionsTest, "Flock.Analytics.Provider.RepeatedExceptions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsRepeatedExceptionsTest::RunTest(const FString& Parameters)
{
	FFlockAnalyticsConfig Config;
	Config.EventBufferFlushIntervalSeconds = 0.f; // hold the spool for inspection
	Config.HeartbeatIntervalSeconds = 0.f;
	Config.bTrackFps = false;
	FFixture Fix(Config);
	Fix.EnableLogSink(Config);
	double Now = 1000.0;
	Fix.Provider->SetClockForTesting([&Now]() { return Now; });

	// Where the engine's Blueprint debugger is loaded it already listens, and the sink writes nothing back.
	const FString ScriptStack = TEXT("Script call stack:\n\tBP_Flock_RepeatProbe_C.ReceiveTick");
	if (!FBlueprintCoreDelegates::OnScriptException.IsBound())
	{
		AddExpectedMessagePlain(TEXT("BP_Flock_RepeatProbe_C.ReceiveTick"), ELogVerbosity::Warning,
			EAutomationExpectedMessageFlags::Contains, 0);
	}

	Fix.Provider->Initialize();
	FFlockLogSink* Sink = Fix.Provider->GetLogSinkForTesting();
	TestNotNull(TEXT("sink is live"), Sink);
	if (Sink == nullptr)
	{
		return false;
	}
	// A session is open, so the next launch's termination report is counting exceptions.
	Fix.Provider->StartSession(TEXT("p-fixture"));

	const auto Matching =[&Fix](const FString& Needle, bool bSummaries)
	{
		TArray<FFlockLogEventRequest> Out;
		for (const FFlockLogEventRequest& Event : Fix.AllSpooled())
		{
			if (Event.Message.Contains(Needle) && Event.Data.ExtraData.Contains(TEXT("repeat_count")) == bSummaries)
			{
				Out.Add(Event);
			}
		}
		return Out;
	};

	// Four occurrences of one fault inside a minute. The node number in the message differs; the fault does not.
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Sink->SimulateScriptExceptionForTesting(EBlueprintExceptionType::AccessViolation,
			FString::Printf(TEXT("Accessed None reading Weapon (node %d)"), 10 + Index), ScriptStack);
		Now += 5.0;
	}
	Fix.Provider->TickForTesting(0.1f);

	const TArray<FFlockLogEventRequest> Reports = Matching(TEXT("Accessed None reading Weapon"), false);
	TestEqual(TEXT("reported once"), Reports.Num(), 1);
	const FFlockLogEventRequest First = FlockTestAt(Reports, 0);
	TestTrue(TEXT("as an exception"), First.Data.Type == EFlockLogEventType::Exception);
	TestEqual(TEXT("from Blueprint"), ExtraValue(First, TEXT("exception_source")), TEXT("blueprint"));
	TestEqual(TEXT("with its kind"), ExtraValue(First, TEXT("blueprint_exception_type")), TEXT("access_violation"));
	TestEqual(TEXT("under the engine's script category"), ExtraValue(First, TEXT("category")), TEXT("LogScript"));
	TestEqual(TEXT("carrying the Blueprint call stack"), First.Data.ErrorTraceback, ScriptStack);
	TestEqual(TEXT("no summary while the window is open"), Matching(TEXT("Accessed None reading Weapon"), true).Num(), 0);
	TestEqual(TEXT("every occurrence still counts toward the termination report"),
		Fix.Deps.TerminationTracker->GetPendingExceptionCount(), 4);

	// A different fault in the same minute is its own report.
	Sink->SimulateScriptExceptionForTesting(EBlueprintExceptionType::InfiniteLoop, TEXT("Runaway loop in BP_Flock_RepeatProbe"), ScriptStack);
	Fix.Provider->TickForTesting(0.1f);
	TestEqual(TEXT("a different fault is reported"), Matching(TEXT("Runaway loop"), false).Num(), 1);

	// The window closes: the repeats are reported with their count.
	Now += 60.0;
	Fix.Provider->TickForTesting(0.1f);
	TArray<FFlockLogEventRequest> Summaries = Matching(TEXT("Accessed None reading Weapon"), true);
	TestEqual(TEXT("one summary"), Summaries.Num(), 1);
	TestEqual(TEXT("counting the three held-back repeats"), ExtraValue(FlockTestAt(Summaries, 0), TEXT("repeat_count")), TEXT("3"));
	TestEqual(TEXT("and the window"), ExtraValue(FlockTestAt(Summaries, 0), TEXT("repeat_window_seconds")),
		FString::SanitizeFloat(Config.ExceptionRepeatWindowSeconds));
	// Counted where the first report was: same source, kind and category.
	TestEqual(TEXT("the repeat report keeps the Blueprint source"), ExtraValue(FlockTestAt(Summaries, 0), TEXT("exception_source")), TEXT("blueprint"));
	TestEqual(TEXT("and the kind"), ExtraValue(FlockTestAt(Summaries, 0), TEXT("blueprint_exception_type")), TEXT("access_violation"));
	TestEqual(TEXT("and the category"), ExtraValue(FlockTestAt(Summaries, 0), TEXT("category")), TEXT("LogScript"));
	TestEqual(TEXT("a fault that never repeated owes no summary"), Matching(TEXT("Runaway loop"), true).Num(), 0);
	// Four occurrences plus the loop: the summary itself is not counted a second time.
	TestEqual(TEXT("a summary is not counted again"), Fix.Deps.TerminationTracker->GetPendingExceptionCount(), 5);

	// After its window the same fault is news again.
	Sink->SimulateScriptExceptionForTesting(EBlueprintExceptionType::AccessViolation, TEXT("Accessed None reading Weapon (node 99)"), ScriptStack);
	Fix.Provider->TickForTesting(0.1f);
	TestEqual(TEXT("reported again in a new window"), Matching(TEXT("Accessed None reading Weapon"), false).Num(), 2);

	// A fault reported by hand is never folded into another.
	Fix.Provider->LogException(TEXT("reported by hand"), TEXT("at Foo()"));
	Fix.Provider->LogException(TEXT("reported by hand"), TEXT("at Foo()"));
	TestEqual(TEXT("both manual reports kept"), Matching(TEXT("reported by hand"), false).Num(), 2);

	// Repeats still inside an open window at shutdown are not lost.
	Sink->SimulateScriptExceptionForTesting(EBlueprintExceptionType::AccessViolation, TEXT("Accessed None reading Weapon (node 100)"), ScriptStack);
	Fix.Provider->Shutdown();
	Summaries = Matching(TEXT("Accessed None reading Weapon"), true);
	TestEqual(TEXT("the open window is summarised at shutdown"), Summaries.Num(), 2);
	TestEqual(TEXT("with its one repeat"), ExtraValue(FlockTestAt(Summaries, 1), TEXT("repeat_count")), TEXT("1"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsExceptionCaptureConfigTest, "Flock.Analytics.Provider.ExceptionCaptureConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsExceptionCaptureConfigTest::RunTest(const FString& Parameters)
{
	{
		const FFlockAnalyticsConfig Defaults;
		TestTrue(TEXT("capture on by default"), Defaults.bCaptureExceptions);
		TestTrue(TEXT("automation controller noise excluded by default"),
			Defaults.ExcludedExceptionCategories.Contains(TEXT("LogAutomationController")));
		TestTrue(TEXT("automation command line noise excluded by default"),
			Defaults.ExcludedExceptionCategories.Contains(TEXT("LogAutomationCommandLine")));
		TestEqual(TEXT("a one-minute window by default"), Defaults.ExceptionRepeatWindowSeconds, 60.f);
	}

	// A project's own exclusions reach the sink, and the SDK's own category stays excluded whatever the list says.
	{
		FFlockAnalyticsConfig Config;
		Config.ExcludedExceptionCategories = { TEXT("LogNoisyPlugin"), FString() };
		FFixture Fix(Config);
		Fix.EnableLogSink(Config);
		Fix.Provider->Initialize();
		const FFlockLogSink* Sink = Fix.Provider->GetLogSinkForTesting();
		TestNotNull(TEXT("sink is live"), Sink);
		if (Sink != nullptr)
		{
			TestTrue(TEXT("configured category excluded"), Sink->IsExcluded(FName(TEXT("LogNoisyPlugin"))));
			TestTrue(TEXT("the SDK's own category always excluded"), Sink->IsExcluded(FName(TEXT("LogFlock"))));
			TestFalse(TEXT("an unlisted category is captured"), Sink->IsExcluded(FName(TEXT("LogGame"))));
			TestFalse(TEXT("a blank entry excludes nothing"), Sink->IsExcluded(NAME_None));
		}
		TestTrue(TEXT("coverage reports capture on"), Fix.Provider->GetExceptionCaptureCoverage().bEnabled);
		Fix.Provider->Shutdown();
		TestFalse(TEXT("and off once shut down"), Fix.Provider->GetExceptionCaptureCoverage().bEnabled);
	}

	// Switched off by the project: nothing taps the log and nothing listens for Blueprint faults.
	{
		FFlockAnalyticsConfig Config;
		Config.bCaptureExceptions = false;
		FFixture Fix(Config);
		Fix.EnableLogSink(Config);
		Fix.Provider->Initialize();
		TestNull(TEXT("no sink when the project switched capture off"), Fix.Provider->GetLogSinkForTesting());
		TestFalse(TEXT("coverage reports capture off"), Fix.Provider->GetExceptionCaptureCoverage().bEnabled);
	}

	// The process switch alone keeps it off too: the automation runner's own errors are not a game's.
	{
		FFixture Fix;
		Fix.Provider->Initialize();
		TestNull(TEXT("no sink without the process switch"), Fix.Provider->GetLogSinkForTesting());
		TestFalse(TEXT("and coverage says so"), Fix.Provider->GetExceptionCaptureCoverage().bEnabled);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsExceptionCaptureCoverageTest, "Flock.Analytics.Provider.ExceptionCaptureCoverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsExceptionCaptureCoverageTest::RunTest(const FString& Parameters)
{
	const FFlockExceptionCaptureCoverage Full = FFlockAnalyticsProvider::ComputeCoverage(true, true, true, true, TEXT("Development"));
	TestTrue(TEXT("a Development build sees everything"), Full.IsComplete());

	const FFlockExceptionCaptureCoverage Shipping = FFlockAnalyticsProvider::ComputeCoverage(true, false, false, false, TEXT("Shipping"));
	TestFalse(TEXT("Shipping: no error log lines"), Shipping.bLogErrors);
	TestFalse(TEXT("Shipping: no ensures"), Shipping.bEnsures);
	TestTrue(TEXT("Shipping: Blueprint exceptions are still broadcast"), Shipping.bBlueprintExceptions);
	TestFalse(TEXT("Shipping: no infinite-loop detection"), Shipping.bBlueprintInfiniteLoops);
	TestTrue(TEXT("Shipping: crashes"), Shipping.bCrashes);
	TestFalse(TEXT("Shipping is not complete"), Shipping.IsComplete());
	TestEqual(TEXT("names its configuration"), Shipping.BuildConfiguration, TEXT("Shipping"));

	// An ensure arrives as an Error line, so it needs logging as well as ensures.
	TestFalse(TEXT("ensures without logging are invisible"),
		FFlockAnalyticsProvider::ComputeCoverage(true, false, true, true, TEXT("Test")).bEnsures);
	const FFlockExceptionCaptureCoverage NoEnsures = FFlockAnalyticsProvider::ComputeCoverage(true, true, false, true, TEXT("Test"));
	TestFalse(TEXT("logging without ensures has none"), NoEnsures.bEnsures);
	TestTrue(TEXT("and still has error lines"), NoEnsures.bLogErrors);

	const FFlockExceptionCaptureCoverage Off = FFlockAnalyticsProvider::ComputeCoverage(false, true, true, true, TEXT("Development"));
	TestFalse(TEXT("capture off sees nothing"),
		Off.bEnabled || Off.bLogErrors || Off.bEnsures || Off.bBlueprintExceptions || Off.bBlueprintInfiniteLoops || Off.bCrashes);

	const auto Notices = [](const FFixture& Fix)
	{
		TArray<FFlockLogEventRequest> Out;
		for (const FFlockLogEventRequest& Event : Fix.AllSpooled())
		{
			if (Event.Message == TEXT("exception_capture_limited"))
			{
				Out.Add(Event);
			}
		}
		return Out;
	};

	// A build that cannot see everything says so once, on the diagnostics dashboard.
	{
		FFixture Fix;
		Fix.Provider->SetCoverageForTesting(Shipping);
		Fix.Provider->Initialize();
		Fix.Provider->Initialize();
		const TArray<FFlockLogEventRequest> Found = Notices(Fix);
		TestEqual(TEXT("one notice"), Found.Num(), 1);
		const FFlockLogEventRequest Notice = FlockTestAt(Found, 0);
		TestTrue(TEXT("a diagnostic entry, not an exception"), Notice.Data.Type == EFlockLogEventType::Debug);
		TestEqual(TEXT("names the configuration"), ExtraValue(Notice, TEXT("build_configuration")), TEXT("Shipping"));
		TestEqual(TEXT("log errors"), ExtraValue(Notice, TEXT("log_errors")), TEXT("false"));
		TestEqual(TEXT("ensures"), ExtraValue(Notice, TEXT("ensures")), TEXT("false"));
		TestEqual(TEXT("blueprint exceptions"), ExtraValue(Notice, TEXT("blueprint_exceptions")), TEXT("true"));
		TestEqual(TEXT("blueprint infinite loops"), ExtraValue(Notice, TEXT("blueprint_infinite_loops")), TEXT("false"));
		TestEqual(TEXT("crashes"), ExtraValue(Notice, TEXT("crashes")), TEXT("true"));
	}

	// Once per build: the next launch of the same build, over the same disk, says nothing more.
	{
		FFixture First;
		First.Provider->SetCoverageForTesting(Shipping);
		First.Provider->Initialize();
		TestEqual(TEXT("sent on the first launch"), Notices(First).Num(), 1);
		const FString RecordPath = FPaths::Combine(First.Dir, TEXT("coverage_notice.txt"));
		{
			FFixture NextLaunch(FFlockAnalyticsConfig(), First.Dir);
			NextLaunch.Provider->SetCoverageForTesting(Shipping);
			NextLaunch.Provider->Initialize();
			TestEqual(TEXT("not again on the next launch of the same build"), Notices(NextLaunch).Num(), 0);
		}
		// That fixture removed the folder on its way out; a record from a different build stands in for it.
		FFileHelper::SaveStringToFile(TEXT("Development|gv-0|0.6.0"), *RecordPath);
		{
			FFixture NewBuild(FFlockAnalyticsConfig(), First.Dir);
			NewBuild.Provider->SetCoverageForTesting(Shipping);
			NewBuild.Provider->Initialize();
			TestEqual(TEXT("sent again by a different build"), Notices(NewBuild).Num(), 1);
		}
	}

	// Nothing is recorded as sent while nothing is collected, so a consent gate cannot swallow the notice for good.
	{
		FFlockAnalyticsConfig Gated;
		Gated.bRequireExplicitConsent = true;
		FFixture Fix(Gated);
		Fix.Provider->SetCoverageForTesting(Shipping);
		Fix.Provider->Initialize();
		TestEqual(TEXT("no notice without consent"), Notices(Fix).Num(), 0);
		TestFalse(TEXT("and no record that one was sent"), FPaths::FileExists(FPaths::Combine(Fix.Dir, TEXT("coverage_notice.txt"))));
	}
	{
		FFixture Fix;
		Fix.Provider->SetCoverageForTesting(Full);
		Fix.Provider->Initialize();
		TestEqual(TEXT("silent when nothing is out of reach"), Notices(Fix).Num(), 0);
	}
	{
		FFixture Fix;
		Fix.Provider->SetCoverageForTesting(Off);
		Fix.Provider->Initialize();
		TestEqual(TEXT("silent when the project switched capture off"), Notices(Fix).Num(), 0);
	}

	// This build's own answer.
	{
		FFixture Fix;
		Fix.EnableLogSink(FFlockAnalyticsConfig());
		Fix.Provider->Initialize();
		const FFlockExceptionCaptureCoverage Coverage = Fix.Provider->GetExceptionCaptureCoverage();
		TestTrue(TEXT("capturing"), Coverage.bEnabled);
		TestTrue(TEXT("Blueprint exceptions in every configuration"), Coverage.bBlueprintExceptions);
		TestEqual(TEXT("names this build"), Coverage.BuildConfiguration, FString(LexToString(FApp::GetBuildConfiguration())));
		Fix.Provider->Shutdown();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsJsonSpooledEventTest, "Flock.Analytics.Json.SpooledAnalyticsEvent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsJsonSpooledEventTest::RunTest(const FString& Parameters)
{
	FFlockSpooledAnalyticsEvent Entry;
	Entry.Event.PlayerId = TEXT("p-1");
	Entry.Event.EventName = TEXT("level_complete");
	Entry.Event.EventCategory = TEXT("progression");
	Entry.Event.SessionId = TEXT("srv-1");
	Entry.Event.Timestamp = TEXT("2026-09-14T10:00:00.000Z");
	Entry.Event.Properties.Set(TEXT("level"), 3).Set(TEXT("playerLevel"), TEXT("gold"));
	Entry.LocalSessionId = TEXT("local-1");
	Entry.FailedSends = 7;

	FFlockSpooledAnalyticsEvent Back;
	TestTrue(TEXT("round trips"), FFlockAnalyticsJson::DeserializeSpooledAnalyticsEvent(
		FFlockAnalyticsJson::SerializeSpooledAnalyticsEvent(Entry), Back));
	TestEqual(TEXT("player"), Back.Event.PlayerId, Entry.Event.PlayerId);
	TestEqual(TEXT("name"), Back.Event.EventName, Entry.Event.EventName);
	TestEqual(TEXT("category"), Back.Event.EventCategory, Entry.Event.EventCategory);
	TestEqual(TEXT("session"), Back.Event.SessionId, Entry.Event.SessionId);
	TestEqual(TEXT("timestamp"), Back.Event.Timestamp, Entry.Event.Timestamp);
	TestEqual(TEXT("local session"), Back.LocalSessionId, Entry.LocalSessionId);
	TestEqual(TEXT("attempts"), Back.FailedSends, 7);
	TestTrue(TEXT("properties keep their keys"), Back.Event.Properties.ToJsonObject()->HasField(TEXT("playerLevel")));
	TestTrue(TEXT("and their types"), Back.Event.Properties.ToJsonObject()->HasTypedField<EJson::Number>(TEXT("level")));

	FFlockSpooledAnalyticsEvent Held;
	Held.Event.EventName = TEXT("title_screen");
	TestTrue(TEXT("a held event round trips"), FFlockAnalyticsJson::DeserializeSpooledAnalyticsEvent(
		FFlockAnalyticsJson::SerializeSpooledAnalyticsEvent(Held), Back));
	TestTrue(TEXT("still unattributed"), Back.Event.PlayerId.IsEmpty());

	// Anything that is not an event is refused rather than sent as one.
	TestFalse(TEXT("no name"), FFlockAnalyticsJson::DeserializeSpooledAnalyticsEvent(TEXT("{\"event\":{\"player_id\":\"p-1\"}}"), Back));
	TestFalse(TEXT("no event"), FFlockAnalyticsJson::DeserializeSpooledAnalyticsEvent(TEXT("{}"), Back));
	TestFalse(TEXT("not JSON"), FFlockAnalyticsJson::DeserializeSpooledAnalyticsEvent(TEXT("not json"), Back));

	// The attempt counter rides a log entry without disturbing it.
	FFlockLogEventRequest Log;
	Log.Message = TEXT("spooled diagnostic");
	const FString Spooled = FFlockAnalyticsJson::SerializeEvent(Log);
	TestEqual(TEXT("an uncounted entry reads 0"), FFlockAnalyticsJson::ReadFailedSendCount(Spooled), 0);
	const FString Counted = FFlockAnalyticsJson::WithFailedSendCount(Spooled, 3);
	TestEqual(TEXT("the count reads back"), FFlockAnalyticsJson::ReadFailedSendCount(Counted), 3);
	FFlockLogEventRequest LogBack;
	TestTrue(TEXT("a counted entry is still a log entry"), FFlockAnalyticsJson::DeserializeEvent(Counted, LogBack));
	TestEqual(TEXT("unchanged"), LogBack.Message, Log.Message);
	TestFalse(TEXT("reserialised without the counter"), FFlockAnalyticsJson::SerializeEvent(LogBack).Contains(TEXT("_flock_failed_sends")));
	TestEqual(TEXT("an unreadable entry is left alone"), FFlockAnalyticsJson::WithFailedSendCount(TEXT("not json"), 3), FString(TEXT("not json")));
	TestEqual(TEXT("and reads 0"), FFlockAnalyticsJson::ReadFailedSendCount(TEXT("not json")), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsFlushWaitAfterFailuresTest, "Flock.Analytics.Provider.FlushWaitAfterFailures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsFlushWaitAfterFailuresTest::RunTest(const FString& Parameters)
{
	FFlockAnalyticsConfig Config;
	Config.EventBufferFlushIntervalSeconds = 10.f;
	Config.HeartbeatIntervalSeconds = 0.f;
	Config.bTrackFps = false;
	FFixture Fix(Config);
	Fix.Provider->Initialize();
	Fix.Provider->LogEvent(TEXT("kept through an outage"));
	Fix.Fake->On(TEXT("log_event"), FFlockFakeTransport::Status(500, TEXT("{}")));

	// Each failure the server answered doubles the wait before the next interval flush, so an outage does not use up
	// every entry's failed-send limit in minutes.
	Fix.Provider->TickForTesting(10.f);
	TestEqual(TEXT("first interval flush"), Fix.Fake->CountTo(TEXT("log_event")), 1);
	TestEqual(TEXT("then waits twice as long"), Fix.Provider->GetFlushWaitSecondsForTesting(), 20.f);
	Fix.Provider->TickForTesting(10.f);
	TestEqual(TEXT("not at the plain interval"), Fix.Fake->CountTo(TEXT("log_event")), 1);
	Fix.Provider->TickForTesting(10.f);
	TestEqual(TEXT("at the longer one"), Fix.Fake->CountTo(TEXT("log_event")), 2);
	TestEqual(TEXT("and doubles again"), Fix.Provider->GetFlushWaitSecondsForTesting(), 40.f);

	// The wait has a limit, so a long outage still retries.
	for (int32 Index = 0; Index < 12; ++Index)
	{
		Fix.Provider->TickForTesting(FFlockAnalyticsProvider::MaxFlushWaitSeconds);
	}
	TestEqual(TEXT("never waits longer than the limit"), Fix.Provider->GetFlushWaitSecondsForTesting(),
		FFlockAnalyticsProvider::MaxFlushWaitSeconds);
	TestEqual(TEXT("fourteen sends in hours, not minutes"), Fix.Fake->CountTo(TEXT("log_event")), 14);
	TestEqual(TEXT("and the entry is still kept"), Fix.Provider->GetPendingEventCount(), 1);

	// A flush that delivers resets it.
	Fix.Fake->On(TEXT("log_event"), FFlockFakeTransport::Ok(TEXT("{}")));
	Fix.Provider->TickForTesting(FFlockAnalyticsProvider::MaxFlushWaitSeconds);
	TestEqual(TEXT("delivered"), Fix.Provider->GetPendingEventCount(), 0);
	TestEqual(TEXT("and the wait is back to the interval"), Fix.Provider->GetFlushWaitSecondsForTesting(), 0.f);

	// A send that never reached the server counts nothing, so it stretches nothing.
	Fix.Provider->LogEvent(TEXT("kept while offline"));
	Fix.Fake->On(TEXT("log_event"), FFlockFakeTransport::Offline());
	const int32 Before = Fix.Fake->CountTo(TEXT("log_event"));
	Fix.Provider->TickForTesting(10.f);
	Fix.Provider->TickForTesting(10.f);
	TestEqual(TEXT("offline keeps the plain interval"), Fix.Fake->CountTo(TEXT("log_event")) - Before, 2);
	TestEqual(TEXT("with no longer wait"), Fix.Provider->GetFlushWaitSecondsForTesting(), 0.f);
	return true;
}

/**
 * One entry can make the server refuse a whole batch for what is in it. Sending the batch apart loses only that entry;
 * a refusal about who is asking cannot come from one entry, so it drops the batch without the extra requests.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsRefusedBatchSentApartTest, "Flock.Analytics.Provider.RefusedBatchSentApart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsRefusedBatchSentApartTest::RunTest(const FString& Parameters)
{
	const FFlockHttpResponse RefusedContent = FFlockFakeTransport::Coded(422, TEXT("request.validation_failed"));
	const FFlockHttpResponse Accepted = FFlockFakeTransport::Ok(TEXT("{\"ok\":true}"));

	{
		FFlockAnalyticsConfig Config;
		Config.CacheFlushBatchSize = 3;
		FFixture Fix(Config);
		Fix.Provider->Initialize();
		Fix.Provider->LogEvent(TEXT("first good entry"));
		Fix.Provider->LogEvent(TEXT("the refused entry"));
		Fix.Provider->LogEvent(TEXT("second good entry"));
		Fix.Fake->OnSequence(TEXT("log_event"), { RefusedContent, Accepted, RefusedContent, Accepted });

		bool bFlushed = true;
		FFlockError Reported;
		Fix.Provider->Flush([&bFlushed, &Reported](TFlockResult<FFlockAnalyticsAck> Result)
		{
			bFlushed = Result.bSuccess;
			Reported = Result.Error;
		});
		TestEqual(TEXT("the batch, then each entry on its own"), Fix.Fake->CountTo(TEXT("log_event")), 4);
		TestEqual(TEXT("nothing left"), Fix.Provider->GetPendingEventCount(), 0);
		TestFalse(TEXT("the one refusal is reported"), bFlushed);
		TestEqual(TEXT("by its code"), Reported.Code, TEXT("request.validation_failed"));

		TArray<FString> LogBodies;
		for (const FFlockHttpRequest& Request : Fix.Fake->Requests)
		{
			if (Request.Url.Contains(TEXT("log_event")))
			{
				LogBodies.Add(Request.JsonBody);
			}
		}
		TestTrue(TEXT("the first good entry went on its own"), FlockTestAt(LogBodies, 1).Contains(TEXT("first good entry")));
		TestTrue(TEXT("the refused one alone"), FlockTestAt(LogBodies, 2).Contains(TEXT("the refused entry")));
		TestTrue(TEXT("and the second good entry after it"), FlockTestAt(LogBodies, 3).Contains(TEXT("second good entry")));
	}

	// A refusal about who is asking is not taken apart: it would only be repeated entry by entry.
	{
		FFlockAnalyticsConfig Config;
		Config.CacheFlushBatchSize = 3;
		FFixture Fix(Config);
		Fix.Provider->Initialize();
		Fix.Provider->LogEvent(TEXT("a"));
		Fix.Provider->LogEvent(TEXT("b"));
		Fix.Provider->LogEvent(TEXT("c"));
		Fix.Fake->On(TEXT("log_event"), FFlockFakeTransport::Coded(403, TEXT("request.forbidden")));
		Fix.Provider->Flush();
		TestEqual(TEXT("one request, not one per entry"), Fix.Fake->CountTo(TEXT("log_event")), 1);
		TestEqual(TEXT("dropped together"), Fix.Provider->GetPendingEventCount(), 0);
	}

	// Gameplay events: the same, inside one player's batch.
	{
		FFlockAnalyticsConfig Config;
		Config.CacheFlushBatchSize = 3;
		FFixture Fix(Config);
		Fix.Provider->Initialize();
		Fix.Provider->TrackEvent(TEXT("first_good_event"));
		Fix.Provider->TrackEvent(TEXT("the_refused_event"));
		Fix.Provider->TrackEvent(TEXT("second_good_event"));
		Fix.Fake->OnSequence(TEXT("analytics/events"), { RefusedContent, Accepted, RefusedContent, Accepted });
		Fix.Provider->Flush();

		const TArray<TArray<TSharedPtr<FJsonObject>>> Batches = Fix.SentEventBatches();
		TestEqual(TEXT("the batch, then each event on its own"), Batches.Num(), 4);
		TestEqual(TEXT("nothing left"), Fix.Provider->GetPendingAnalyticsEventCount(), 0);
		TestEqual(TEXT("the first good event alone"), JsonString(FlockTestAt(FlockTestAt(Batches, 1), 0), TEXT("event_name")),
			TEXT("first_good_event"));
		TestEqual(TEXT("the refused one alone"), JsonString(FlockTestAt(FlockTestAt(Batches, 2), 0), TEXT("event_name")),
			TEXT("the_refused_event"));
		TestEqual(TEXT("the second good event alone"), JsonString(FlockTestAt(FlockTestAt(Batches, 3), 0), TEXT("event_name")),
			TEXT("second_good_event"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsFatalCaptureDetailsTest, "Flock.Analytics.Provider.FatalCaptureDetails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsFatalCaptureDetailsTest::RunTest(const FString& Parameters)
{
	FFlockAnalyticsConfig Config;
	Config.EventBufferFlushIntervalSeconds = 0.f;
	FFixture Fix(Config);
	Fix.EnableLogSink(Config);
	Fix.Provider->Initialize();
	FFlockLogSink* Sink = Fix.Provider->GetLogSinkForTesting();
	TestNotNull(TEXT("sink is live"), Sink);
	if (Sink == nullptr)
	{
		return false;
	}

	// A crash has no tick after it, so it takes the path that writes straight to disk — and still says where it came from.
	Sink->SimulateSystemErrorForTesting();
	bool bFound = false;
	for (const FFlockLogEventRequest& Event : Fix.AllSpooled())
	{
		if (Event.Message != TEXT("Unhandled system error"))
		{
			continue;
		}
		bFound = true;
		TestEqual(TEXT("category"), ExtraValue(Event, TEXT("category")), TEXT("SystemError"));
		TestEqual(TEXT("source"), ExtraValue(Event, TEXT("exception_source")), TEXT("crash"));
	}
	TestTrue(TEXT("the crash was spooled"), bFound);
	Fix.Provider->Shutdown();
	return true;
}

/**
 * OnSessionRegistered hands listeners the id a session's records are filed under, once per session, from that
 * session's own registration. A heartbeat on a session that already has its id raises nothing more.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsSessionRegisteredOncePerSessionTest,
	"Flock.Analytics.Provider.SessionRegistered.OncePerSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsSessionRegisteredOncePerSessionTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();
	UFlockEventTestListener* Listener = NewObject<UFlockEventTestListener>();
	Fix.Events->OnSessionRegistered.AddDynamic(Listener, &UFlockEventTestListener::HandleSessionRegistered);

	Fix.Provider->StartSession(TEXT("p-1"));
	const FString FirstLocalId = Fix.Provider->GetCurrentSnapshot().SessionId;
	TestEqual(TEXT("Raised once the server answered"), Listener->SessionRegisteredCount, 1);
	TestEqual(TEXT("With the local id OnSessionStarted carried"), Listener->LastRegisteredSessionId, FirstLocalId);
	TestEqual(TEXT("And the server's id"), Listener->LastRegisteredServerSessionId, FString(TEXT("srv-1")));

	Fix.Provider->TickForTesting(120.f);
	TestEqual(TEXT("A heartbeat on a registered session raises nothing more"), Listener->SessionRegisteredCount, 1);

	Fix.OnRegistration(FFlockFakeTransport::Ok(TEXT("{\"session_id\":\"srv-2\"}")));
	Fix.Provider->StartSession(TEXT("p-1"));
	TestEqual(TEXT("The next session raises for itself"), Listener->SessionRegisteredCount, 2);
	TestEqual(TEXT("With its own server id"), Listener->LastRegisteredServerSessionId, FString(TEXT("srv-2")));
	TestNotEqual(TEXT("And its own local id"), Listener->LastRegisteredSessionId, FirstLocalId);
	return true;
}

/** Nothing is raised until the server has handed over an id; the heartbeat retry that gets one raises it. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsSessionRegisteredOnlyWithAServerIdTest,
	"Flock.Analytics.Provider.SessionRegistered.OnlyWithAServerId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsSessionRegisteredOnlyWithAServerIdTest::RunTest(const FString& Parameters)
{
	FFlockAnalyticsConfig Config;
	Config.HeartbeatIntervalSeconds = 60.f;
	Config.EventBufferFlushIntervalSeconds = 0.f; // isolate the heartbeat
	Config.bTrackFps = false;
	FFixture Fix(Config);
	Fix.Provider->Initialize();
	UFlockEventTestListener* Listener = NewObject<UFlockEventTestListener>();
	Fix.Events->OnSessionRegistered.AddDynamic(Listener, &UFlockEventTestListener::HandleSessionRegistered);

	Fix.OnRegistration(FFlockFakeTransport::Offline());
	Fix.Provider->StartSession(TEXT("p-1"));
	TestEqual(TEXT("A registration that never reached the server raises nothing"), Listener->SessionRegisteredCount, 0);

	Fix.OnRegistration(FFlockFakeTransport::Ok(TEXT("{}")));
	Fix.Provider->TickForTesting(61.f);
	TestEqual(TEXT("The heartbeat retried"), Fix.CountMethod(TEXT("POST"), TEXT("analytics/sessions")), 2);
	TestEqual(TEXT("A 2xx with no id raises nothing"), Listener->SessionRegisteredCount, 0);

	Fix.OnRegistration(FFlockFakeTransport::Ok(TEXT("{\"session_id\":\"srv-healed\"}")));
	Fix.Provider->TickForTesting(61.f);
	TestEqual(TEXT("The retry that got an id raises it"), Listener->SessionRegisteredCount, 1);
	TestEqual(TEXT("With that id"), Listener->LastRegisteredServerSessionId, FString(TEXT("srv-healed")));
	return true;
}

/**
 * A session that ends before its registration reply lands is closed from the spool, and its id reaches the spooled
 * record, not the listeners: the session they would be told about is already over.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsSessionRegisteredNotForAnEndedSessionTest,
	"Flock.Analytics.Provider.SessionRegistered.NotForASessionThatEndedFirst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsSessionRegisteredNotForAnEndedSessionTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();
	UFlockEventTestListener* Listener = NewObject<UFlockEventTestListener>();
	Fix.Events->OnSessionRegistered.AddDynamic(Listener, &UFlockEventTestListener::HandleSessionRegistered);

	Fix.Fake->bDeferred = true;
	Fix.Provider->StartSession(TEXT("p-1"));
	Fix.Provider->EndSession(EFlockSessionEndReason::Logout);
	// The registration reply for the ended session, then whatever the spool drain sends after it.
	for (int32 Round = 0; Round < 4; ++Round)
	{
		Fix.Fake->FlushPending();
	}

	TestTrue(TEXT("The session did register"), Fix.CountMethod(TEXT("POST"), TEXT("analytics/sessions")) >= 1);
	TestEqual(TEXT("A reply for a session that already ended raises nothing"), Listener->SessionRegisteredCount, 0);
	return true;
}

/** An end spooled by an earlier run registers itself on the next flush, and that is not this run's session. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsSessionRegisteredNotForASpooledEndTest,
	"Flock.Analytics.Provider.SessionRegistered.NotForASpooledEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsSessionRegisteredNotForASpooledEndTest::RunTest(const FString& Parameters)
{
	FFixture Fix;
	Fix.Provider->Initialize();
	UFlockEventTestListener* Listener = NewObject<UFlockEventTestListener>();
	Fix.Events->OnSessionRegistered.AddDynamic(Listener, &UFlockEventTestListener::HandleSessionRegistered);

	FFlockSessionSnapshot Unregistered;
	Unregistered.SessionId = TEXT("local-earlier-run");
	Unregistered.PlayerId = TEXT("p-1");
	Unregistered.StartTimeUtc = TEXT("2026-07-22T08:00:00Z");
	Fix.EndCache->Enqueue(FFlockAnalyticsJson::SerializeSnapshot(Unregistered));
	Fix.OnRegistration(FFlockFakeTransport::Ok(TEXT("{\"session_id\":\"srv-earlier-run\"}")));
	Fix.OnClose(TEXT("srv-earlier-run"), FFlockFakeTransport::Ok(TEXT("{}")));

	Fix.Provider->Flush();

	TestEqual(TEXT("The spooled end registered itself"), Fix.CountMethod(TEXT("POST"), TEXT("analytics/sessions")), 1);
	TestEqual(TEXT("An earlier run's session raises nothing"), Listener->SessionRegisteredCount, 0);
	return true;
}

namespace FlockAnalyticsProviderTestHelpers
{
	/** The platform the latest session registration sent, or "<absent>". */
	inline FString RegisteredPlatform(const FFixture& Fix)
	{
		for (int32 Index = Fix.Fake->Requests.Num() - 1; Index >= 0; --Index)
		{
			const FFlockHttpRequest& Request = Fix.Fake->Requests[Index];
			if (Request.Method == TEXT("POST") && Request.Url.EndsWith(TEXT("analytics/sessions")))
			{
				TSharedPtr<FJsonObject> Body;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Request.JsonBody);
				return FJsonSerializer::Deserialize(Reader, Body) ? JsonString(Body, TEXT("platform")) : FString(TEXT("<unreadable>"));
			}
		}
		return TEXT("<absent>");
	}
}

/** Session Platform replaces the engine's platform name on a session start, and a stray space is refused with a warning. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsSessionPlatformIsSentTest, "Flock.Analytics.Provider.SessionPlatformIsSent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsSessionPlatformIsSentTest::RunTest(const FString& Parameters)
{
	const FString EnginePlatformName = FPlatformProperties::IniPlatformName();
	{
		FFixture Fix;
		Fix.Provider->Initialize();
		Fix.Provider->StartSession(TEXT("p-1"));
		TestEqual(TEXT("Unset sends the engine's platform name"), RegisteredPlatform(Fix), EnginePlatformName);
	}

	struct FCase
	{
		const TCHAR* Setting;
		FString Expected;
		bool bWarns;
	};
	const FCase Cases[] = {
		{ TEXT("steam"), TEXT("steam"), false },
		{ TEXT("steam "), EnginePlatformName, true },
	};
	for (const FCase& Case : Cases)
	{
		FFlockAnalyticsConfig Config;
		Config.SessionPlatform = Case.Setting;
		FFixture Fix(Config);
		const TSharedRef<FFlockRecordingLogger> Logger = MakeShared<FFlockRecordingLogger>();
		Fix.Provider = MakeShared<FFlockAnalyticsProvider>(Fix.Client, NoRetryPolicy(), Logger, Fix.Session, Fix.Events,
			TEXT("http://x/v1"), Config, Fix.Deps, TEXT("gv-1"), TEXT("0.7.0"));
		Fix.Provider->Initialize();
		Fix.Provider->StartSession(TEXT("p-1"));

		TestEqual(FString::Printf(TEXT("'%s' sends the expected platform"), Case.Setting), RegisteredPlatform(Fix), Case.Expected);
		TestEqual(FString::Printf(TEXT("'%s' warns only when it is not used"), Case.Setting),
			FFlockRecordingLogger::AnyContains(Logger->Warnings, FString::Printf(TEXT("'%s'"), Case.Setting)), Case.bWarns);
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
