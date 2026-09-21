// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockTokenStore.h"
#include "Config/FlockConfig.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FlockEvents.h"
#include "FlockInitConfig.h"
#include "FlockPlaytestConsent.h"
#include "FlockPlaytestPerformanceTimeline.h"
#include "FlockPlaytestSettings.h"
#include "FlockPlaytestSubsystem.h"
#include "FlockSubsystem.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "Misc/Base64.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Providers/FlockAnalyticsProvider.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/FlockPlaytestFakeTransport.h"
#include "Tests/FlockPlaytestTestSupport.h"
#include "UObject/Package.h"

/** What the playtest subsystem's tests share: settings scopes, the two subsystems under one game instance, and a log capture. */
namespace FlockPlaytestSubsystemTesting
{
	inline constexpr const TCHAR* UsableUrl = TEXT("http://localhost:8020");

	/** Sets the project's playtest settings for one test and puts the previous values back when it ends. */
	struct FScopedPlaytestSettings
	{
		UFlockPlaytestSettings* Settings;
		bool bSavedPlaytestingEnabled;
		FString SavedProtokiteApiUrl;
		bool bSavedAskThePlayerForPlaytestConsent;

		FScopedPlaytestSettings(bool bPlaytestingEnabled, const FString& ProtokiteApiUrl,
			bool bAskThePlayerForPlaytestConsent = true)
			: Settings(GetMutableDefault<UFlockPlaytestSettings>())
			, bSavedPlaytestingEnabled(Settings->bPlaytestingEnabled)
			, SavedProtokiteApiUrl(Settings->ProtokiteApiUrl)
			, bSavedAskThePlayerForPlaytestConsent(Settings->bAskThePlayerForPlaytestConsent)
		{
			Settings->bPlaytestingEnabled = bPlaytestingEnabled;
			Settings->ProtokiteApiUrl = ProtokiteApiUrl;
			Settings->bAskThePlayerForPlaytestConsent = bAskThePlayerForPlaytestConsent;
		}

		~FScopedPlaytestSettings()
		{
			Settings->bPlaytestingEnabled = bSavedPlaytestingEnabled;
			Settings->ProtokiteApiUrl = SavedProtokiteApiUrl;
			Settings->bAskThePlayerForPlaytestConsent = bSavedAskThePlayerForPlaytestConsent;
		}
	};

	/** Sets the Flock SDK's retry settings for one test and puts the previous values back when it ends. */
	struct FScopedFlockRetrySettings
	{
		UFlockConfig* FlockSettings = GetMutableDefault<UFlockConfig>();
		int32 SavedRetryMaxRetries = FlockSettings->RetryMaxRetries;
		bool bSavedRetryUseJitter = FlockSettings->bRetryUseJitter;

		FScopedFlockRetrySettings(int32 RetryMaxRetries, bool bRetryUseJitter)
		{
			FlockSettings->RetryMaxRetries = RetryMaxRetries;
			FlockSettings->bRetryUseJitter = bRetryUseJitter;
		}

		~FScopedFlockRetrySettings()
		{
			FlockSettings->RetryMaxRetries = SavedRetryMaxRetries;
			FlockSettings->bRetryUseJitter = bSavedRetryUseJitter;
		}
	};

	/**
	 * Sets the Flock SDK's analytics settings the playtest tests rely on for one test: analytics on or off, a session
	 * started on sign-in, no consent needed, and exception capture on unless a test turns it off. Events are sent as they are recorded rather than queued on
	 * disk, so each test sees its own events arrive as it records them. A test that has to watch the queue keeps it
	 * (bKeepEventsUntilSent); the queue is in the fixture's own folder. Puts the previous values back.
	 */
	struct FScopedFlockAnalyticsSettings
	{
		UFlockConfig* FlockSettings = GetMutableDefault<UFlockConfig>();
		bool bSavedAnalyticsEnabled = FlockSettings->bAnalyticsEnabled;
		bool bSavedAnalyticsAutoStartSession = FlockSettings->bAnalyticsAutoStartSession;
		bool bSavedAnalyticsRequireExplicitConsent = FlockSettings->bAnalyticsRequireExplicitConsent;
		bool bSavedAnalyticsCacheFailedEvents = FlockSettings->bAnalyticsCacheFailedEvents;
		bool bSavedAnalyticsCaptureExceptions = FlockSettings->bAnalyticsCaptureExceptions;

		explicit FScopedFlockAnalyticsSettings(bool bAnalyticsEnabled, bool bCaptureExceptions = true, bool bKeepEventsUntilSent = false)
		{
			FlockSettings->bAnalyticsEnabled = bAnalyticsEnabled;
			FlockSettings->bAnalyticsAutoStartSession = true;
			FlockSettings->bAnalyticsRequireExplicitConsent = false;
			FlockSettings->bAnalyticsCacheFailedEvents = bKeepEventsUntilSent;
			FlockSettings->bAnalyticsCaptureExceptions = bCaptureExceptions;
		}

		~FScopedFlockAnalyticsSettings()
		{
			FlockSettings->bAnalyticsEnabled = bSavedAnalyticsEnabled;
			FlockSettings->bAnalyticsAutoStartSession = bSavedAnalyticsAutoStartSession;
			FlockSettings->bAnalyticsRequireExplicitConsent = bSavedAnalyticsRequireExplicitConsent;
			FlockSettings->bAnalyticsCacheFailedEvents = bSavedAnalyticsCacheFailedEvents;
			FlockSettings->bAnalyticsCaptureExceptions = bSavedAnalyticsCaptureExceptions;
		}
	};

	/** Holds a saved Flock sign-in in memory, the way a player who signed in on an earlier launch comes back. */
	class FPlaytestTokenStore : public IFlockTokenStore
	{
	public:
		FFlockStoredTokens Stored;
		bool bHasTokens = false;

		virtual void Save(const FFlockStoredTokens& Tokens) override
		{
			Stored = Tokens;
			bHasTokens = true;
		}

		virtual bool Load(FFlockStoredTokens& OutTokens) override
		{
			OutTokens = Stored;
			return bHasTokens;
		}

		virtual void Clear() override
		{
			Stored = FFlockStoredTokens();
			bHasTokens = false;
		}
	};

	/** An access token for PlayerId that stays valid for an hour. Only its player and expiry are read. */
	inline FString MakeAccessToken(const FString& PlayerId)
	{
		const FString Payload = FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId,
			FDateTime::UtcNow().ToUnixTimestamp() + 3600);
		FString Encoded = FBase64::Encode(Payload);
		Encoded.ReplaceInline(TEXT("+"), TEXT("-"));
		Encoded.ReplaceInline(TEXT("/"), TEXT("_"));
		Encoded.ReplaceInline(TEXT("="), TEXT(""));
		return FString::Printf(TEXT("h.%s.s"), *Encoded);
	}

	/**
	 * A game world whose map is called MapName, belonging to GameInstance, the way the engine hands a loaded world to its
	 * map-load event. Not announced to the engine, and destroyed when this goes. Each lives in a package of its own, so
	 * two tests can use the same map name.
	 */
	struct FScopedTestWorld
	{
		UWorld* World = nullptr;

		FScopedTestWorld(const FString& MapName, UGameInstance* GameInstance)
		{
			UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/FlockPlaytestTests/%s/%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits), *MapName));
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(*MapName), Package);
			World->SetGameInstance(GameInstance);
		}

		~FScopedTestWorld()
		{
			World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			World->RemoveFromRoot();
		}
	};

	inline FFlockInitConfig MakeFlockConfig(const FString& GameVersionId = FlockPlaytestFixtures::GameVersionId)
	{
		FFlockInitConfig Config;
		// Nothing in these tests signs in, so nothing is sent to Flock; the address is unreachable all the same.
		Config.ApiUrl = TEXT("http://127.0.0.1:9");
		Config.ApiKey = TEXT("secret");
		Config.GameId = TEXT("flock-playtest-test");
		Config.GameVersion = TEXT("1.2.3");
		Config.GameVersionId = GameVersionId;
		return Config;
	}

	/**
	 * Both subsystems under one game instance, built directly rather than through the game instance's subsystem
	 * collection, so each test drives the Flock SDK's lifecycle itself. Protokite is the fake transport, which
	 * answers the playtest config with a loaded config, a session start with a session and a session end with 204,
	 * unless a test says otherwise. Nothing is retried unless a test keeps the Flock SDK's retry settings. The device
	 * id lives in this fixture's own folder, and the Steam account is the one a test puts in SteamAccount (none by
	 * default), unless a test asks for the real sources.
	 */
	struct FPlaytestFixture
	{
		UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
		UFlockSubsystem* Flock = NewObject<UFlockSubsystem>(GameInstance);
		UFlockPlaytestSubsystem* Playtest = NewObject<UFlockPlaytestSubsystem>(GameInstance);
		TSharedRef<FFlockPlaytestFakeTransport> Transport = MakeShared<FFlockPlaytestFakeTransport>();

		/** This fixture's own folder, removed when it ends, so no test reads another test's device id. */
		FString Folder = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockPlaytestTests"),
			FGuid::NewGuid().ToString(EGuidFormats::Digits));
		FString DeviceIdFilePath = FPaths::Combine(Folder, TEXT("device_id.txt"));

		/** The player's consent answer, in this fixture's folder so no test reads or writes the project's own. */
		FString ConsentFilePath = FPaths::Combine(Folder, TEXT("playtest_consent.json"));

		/** The account the Steam reader answers with, and how many times it was asked. */
		TSharedRef<FFlockRunningSteamAccount> SteamAccount = MakeShared<FFlockRunningSteamAccount>();
		TSharedRef<int32> SteamReads = MakeShared<int32>(0);

		/** The engine frame number the playtest subsystem reads. Tests move it on the way the engine does, once per frame. */
		TSharedRef<uint64> EngineFrameNumber = MakeShared<uint64>(0);

		explicit FPlaytestFixture(bool bTurnRetriesOff = true, bool bUseTestIdentitySources = true,
			EFlockPlaytestConsentChoice PlayersConsent = EFlockPlaytestConsentChoice::VideoAndPlayData)
		{
			// The fixture stands in for a player who has already said what this playtest may collect, so a test that is
			// not about that question sees the playtest run. A test about it hands in another answer, NotAnswered
			// included, which is a player who has not been asked yet. Written straight to the file rather than through
			// the subsystem, which would decide and log a status before the test had begun.
			Playtest->SetConsentFilePathForTesting(ConsentFilePath);
			FFlockPlaytestConsentFile(ConsentFilePath).Save(PlayersConsent);

			AnswerConfig(FFlockPlaytestFakeTransport::Status(200, FlockPlaytestFixtures::ConfigBody()));
			Transport->Answer(FlockPlaytestFixtures::PlaytestSessionStartRoute,
				FFlockPlaytestFakeTransport::Status(200, FlockPlaytestFixtures::SessionStartBody()));
			Transport->Answer(FlockPlaytestFixtures::PlaytestSessionEndRoute, FFlockPlaytestFakeTransport::NoContent());
			Playtest->SetHttpAdapterForTesting(Transport);
			// Every file the Flock SDK saves is kept in this fixture's folder. Otherwise the fixture is one more launch of the
			// game: a real launch takes over the analytics it left and sends them (a killed run sent this fixture's signed-in
			// player to the real server), and the fixture takes over, sends or deletes what real launches left.
			Flock->SetSavedFilesFolderForTesting(FPaths::Combine(Folder, TEXT("Flock")));
			// Recordings are kept in this fixture's folder too, so what a launch finds there is only what the test put there.
			Playtest->SetVideoRecordingFolderForTesting(FPaths::Combine(Folder, TEXT("Recordings")));
			// And feedback forms that could not be sent, which a later launch sends wherever it finds them.
			Playtest->SetFormSpoolFolderForTesting(FPaths::Combine(Folder, TEXT("FeedbackForms")));
			const TSharedRef<uint64> Frame = EngineFrameNumber;
			Playtest->SetEngineFrameNumberReaderForTesting([Frame]() { return *Frame; });
			if (bTurnRetriesOff)
			{
				FFlockRetryPolicy NoRetries;
				NoRetries.MaxRetries = 0;
				Playtest->SetRetryPolicyForTesting(NoRetries);
			}
			if (bUseTestIdentitySources)
			{
				Playtest->SetDeviceIdFilePathForTesting(DeviceIdFilePath);
				const TSharedRef<FFlockRunningSteamAccount> Account = SteamAccount;
				const TSharedRef<int32> Reads = SteamReads;
				Playtest->SetSteamAccountReaderForTesting([Account, Reads]()
				{
					++*Reads;
					return *Account;
				});
			}
		}

		~FPlaytestFixture()
		{
			// Shut down the way the game instance does, so nothing a test started (a recording, a ticker) outlives it.
			Playtest->Deinitialize();
			Flock->ShutdownSdk();
			IFileManager::Get().DeleteDirectory(*Folder, /*bRequireExists*/ false, /*bTree*/ true);
		}

		/** Follows the Flock SDK and initializes it, which reaches Ready with the default answers. */
		void StartFlock()
		{
			Playtest->FollowFlockLifecycleForTesting(Flock);
			// What earlier launches left is gone through on a worker thread; a test starts recording once that is done.
			Playtest->WaitUntilRecordingsFolderFinishedForTesting();
			Flock->InitializeWithConfig(MakeFlockConfig());
		}

		/** Announces a Flock session reaching the server, the way the Flock SDK does. */
		void RegisterFlockSession(const FString& ServerSessionId)
		{
			Flock->GetEvents()->InvokeSessionRegistered(TEXT("local-") + ServerSessionId, ServerSessionId);
		}

		void AnswerConfig(const FFlockHttpResponse& Response)
		{
			Transport->Answer(FlockPlaytestFixtures::PlaytestConfigRoute, Response);
		}

		int32 ConfigRequests() const
		{
			return Transport->CountRequestsEndingWith(FlockPlaytestFixtures::PlaytestConfigRoute);
		}

		int32 SessionStarts() const
		{
			return Transport->CountRequestsEndingWith(FlockPlaytestFixtures::PlaytestSessionStartRoute);
		}

		int32 SessionEnds() const
		{
			return Transport->CountRequestsEndingWith(TEXT("/end"));
		}

		/** The latest session start's body, parsed; null when no start was sent. */
		TSharedPtr<FJsonObject> LastSessionStartBody() const
		{
			TSharedPtr<FJsonObject> Body;
			if (const FFlockHttpRequest* Request = Transport->FindLastRequestEndingWith(FlockPlaytestFixtures::PlaytestSessionStartRoute))
			{
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Request->JsonBody);
				FJsonSerializer::Deserialize(Reader, Body);
			}
			return Body;
		}

		TSharedRef<FPlaytestTokenStore> TokenStore = MakeShared<FPlaytestTokenStore>();

		/**
		 * Brings a signed-in player back when the Flock SDK initializes, and answers the Flock SDK's requests on the same
		 * fake transport, so the analytics events it is handed can be delivered and read. Call before StartFlock.
		 */
		void SignInToFlockOnStart(const FString& PlayerId = FlockPlaytestFixtures::FlockPlayerId)
		{
			TokenStore->Stored.AccessToken = MakeAccessToken(PlayerId);
			TokenStore->Stored.RefreshToken = TEXT("refresh-token");
			TokenStore->Stored.AuthMethod = EFlockAuthMethod::Device;
			TokenStore->bHasTokens = true;
			Flock->SetTokenStoreForTesting(TokenStore);
			Flock->SetHttpAdapterForTesting(Transport);

			// Shaped like the local Flock API's answers (2026-09-15).
			Transport->Answer(TEXT("/analytics/sessions"), FFlockPlaytestFakeTransport::Status(200, FString::Printf(
				TEXT("{\"session_id\":\"%s\",\"event_name\":\"session_started\"}"), FlockPlaytestFixtures::FirstFlockSessionId)));
			Transport->Answer(TEXT("/analytics/events"), FFlockPlaytestFakeTransport::Status(200, TEXT("{\"ok\":true,\"count\":1}")));
			Transport->Answer(TEXT("/log_event"), FFlockPlaytestFakeTransport::Status(200, TEXT("{\"ok\":true,\"count\":1}")));
		}

		/**
		 * Every event in the playtest category the Flock SDK has sent so far, in order. Only the playtest category, because
		 * the Flock SDK sends its own events on the same transport.
		 */
		TArray<TSharedPtr<FJsonObject>> SentPlaytestEvents() const
		{
			TArray<TSharedPtr<FJsonObject>> Events;
			for (const FFlockHttpRequest& Request : Transport->Requests)
			{
				if (!Request.Url.EndsWith(TEXT("/analytics/events")))
				{
					continue;
				}
				TSharedPtr<FJsonObject> Body;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Request.JsonBody);
				const TArray<TSharedPtr<FJsonValue>>* Sent = nullptr;
				if (!FJsonSerializer::Deserialize(Reader, Body) || !Body.IsValid() || !Body->TryGetArrayField(TEXT("events"), Sent))
				{
					continue;
				}
				for (const TSharedPtr<FJsonValue>& Value : *Sent)
				{
					const TSharedPtr<FJsonObject>* Event = nullptr;
					if (Value.IsValid() && Value->TryGetObject(Event)
						&& StringMember(*Event, TEXT("event_category")).Equals(FlockPlaytestEvents::Category, ESearchCase::CaseSensitive))
					{
						Events.Add(*Event);
					}
				}
			}
			return Events;
		}

		/** How many requests the Flock SDK has sent to its analytics events route so far. */
		int32 AnalyticsEventRequests() const
		{
			return Transport->CountRequestsEndingWith(TEXT("/analytics/events"));
		}
	};

	/** Collects what the playtest plugin logs while it exists. */
	class FPlaytestLogCapture : public FOutputDevice
	{
	public:
		struct FLine
		{
			ELogVerbosity::Type Verbosity;
			FString Message;
		};

		FPlaytestLogCapture()
		{
			GLog->AddOutputDevice(this);
		}

		virtual ~FPlaytestLogCapture() override
		{
			GLog->RemoveOutputDevice(this);
		}

		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Category == PlaytestCategory)
			{
				FScopeLock Lock(&LinesLock);
				Lines.Add({ static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask), Message });
			}
		}

		// Handed each line as it is logged rather than later from a buffer, so the lines are here when read.
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		/** The lines logged at Log or louder, in order. */
		TArray<FLine> LinesAtLogOrLouder()
		{
			GLog->Flush();
			FScopeLock Lock(&LinesLock);
			return Lines.FilterByPredicate([](const FLine& Line) { return Line.Verbosity <= ELogVerbosity::Log; });
		}

		/** The lines logged at Log or louder that contain Needle, in order. */
		TArray<FLine> LinesContaining(const FString& Needle)
		{
			return LinesAtLogOrLouder().FilterByPredicate([&Needle](const FLine& Line) { return Line.Message.Contains(Needle); });
		}

	private:
		const FName PlaytestCategory = TEXT("LogFlockPlaytest");
		FCriticalSection LinesLock;
		TArray<FLine> Lines;
	};
}

#endif
