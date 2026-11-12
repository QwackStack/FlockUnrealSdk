// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

#include "Analytics/FlockAnalyticsJson.h"
#include "Analytics/FlockAnalyticsLaunches.h"
#include "Analytics/FlockLogSink.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Auth/FlockAuthSession.h"
#include "FlockEvents.h"
#include "FlockLogger.h"
#include "HAL/FileManager.h"
#include "Http/FlockHttpClient.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/FlockLaunchFolder.h"
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
#include "Tests/Support/FlockTestSpelling.h"
#include "HAL/PlatformProperties.h"

namespace FlockAnalyticsProviderTestHelpers
{
	inline FFlockRetryPolicy NoRetryPolicy()
	{
		FFlockRetryPolicy Policy;
		Policy.MaxRetries = 0;
		return Policy;
	}

	/** The game every fixture's launches are kept for. */
	const TCHAR* const AnalyticsTestGame = TEXT("0123456789abcdef");

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
		/** Only the fixture that made Dir deletes it, so a second launch over the same files never wipes the first's. */
		bool bOwnsDir = false;
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

		/**
		 * Pass ExistingDir to reuse another fixture's files — that is what "the next launch" means. The SDK's own events are
		 * off unless a test asks for them, so the other tests count only their own events.
		 */
		explicit FFixture(FFlockAnalyticsConfig Config = FFlockAnalyticsConfig(),
			const FString& ExistingDir = FString(), bool bSendDefaultEvents = false)
			: Dir(ExistingDir.IsEmpty() ? TempDir() : ExistingDir)
			, bOwnsDir(ExistingDir.IsEmpty())
			, Client(MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>()))
			, Session(MakeShared<FFlockAuthSession>(Client, Store, MakeShared<FFlockNullLogger>(),
				TEXT("http://x/v1"), TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") } }))
		{
			Events = NewObject<UFlockEvents>();
			bDefaultEvents = bSendDefaultEvents;

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
			// Each fixture is one launch of the game, with an analytics folder of its own under Dir.
			Deps.Launches = FFlockAnalyticsLaunches::Start(Dir, AnalyticsTestGame);
			Deps.Session = MakeShared<FFlockSession>(Config, Deps.Launches->GetSessionStatePath());
			Deps.TerminationTracker = MakeShared<FFlockTerminationTracker>(true, Deps.Launches->GetTerminationMarkerPath());
			Deps.ConsentStore = MakeShared<FFlockConsentStore>(FPaths::Combine(Dir, TEXT("consent.json")));
			Deps.Pump = MakeShared<FFlockLifecyclePump>();
			Deps.bEnableLogSink = false; // a GLog tap inside the runner captures the runner's own errors
			Deps.CoverageNoticeMarkerPath = FPaths::Combine(Dir, TEXT("coverage_notice.txt"));
			Deps.DefaultEventsRecordPath = FPaths::Combine(Dir, TEXT("default_events.json"));

			ApplyRoutes();

			Provider = MakeShared<FFlockAnalyticsProvider>(Client, NoRetryPolicy(), MakeShared<FFlockNullLogger>(),
				Session, Events, TEXT("http://x/v1"), Same(Config), Deps, TEXT("gv-1"), TEXT("0.7.0"));
		}

		/** Whether this fixture's provider records the SDK's own events. */
		bool bDefaultEvents = false;

		/** A config for a provider this fixture builds: the SDK's own events as the fixture has them. */
		FFlockAnalyticsConfig Same(FFlockAnalyticsConfig Config) const
		{
			Config.bSendDefaultEvents = bDefaultEvents;
			return Config;
		}

		/** Lets go of this launch's lock while its files stay, the way a game that crashed looks to the next launch. */
		void EndLaunchWithoutShuttingDown() const
		{
			Deps.Launches->StopHoldingItsFolderForTesting();
		}

		/** Saves Contents as FileName in a launch folder under ParentDir whose launch has already ended. */
		static void SaveInAnEndedLaunch(const FString& ParentDir, const FString& FileName, const FString& Contents)
		{
			const TSharedPtr<FFlockLaunchFolder> Ended =
				FFlockLaunchFolder::Create(FFlockAnalyticsLaunches::LaunchesFolderOf(ParentDir, AnalyticsTestGame));
			if (Ended.IsValid())
			{
				FFileHelper::SaveStringToFile(Contents, *FPaths::Combine(Ended->GetPath(), FileName));
			}
		}

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
			Deps.Launches.Reset();
			if (bOwnsDir)
			{
				IFileManager::Get().DeleteDirectory(*Dir, false, true);
			}
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
				Session, Events, TEXT("http://x/v1"), Same(Config), Deps, TEXT("gv-1"), TEXT("0.7.0"));
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
		// Found only under exactly this spelling: the object's own lookup ignores letter case, and the server does not.
		return FlockTestSpelling::HasMemberSpelled(Object, Key) && Object->TryGetStringField(Key, Value) ? Value : FString(TEXT("<absent>"));
	}

	/** An extra_data value, or "<absent>". */
	inline FString ExtraValue(const FFlockLogEventRequest& Event, const TCHAR* Key)
	{
		const FString* Value = Event.Data.ExtraData.Find(Key);
		return Value != nullptr ? *Value : FString(TEXT("<absent>"));
	}
}

#endif // WITH_AUTOMATION_TESTS
