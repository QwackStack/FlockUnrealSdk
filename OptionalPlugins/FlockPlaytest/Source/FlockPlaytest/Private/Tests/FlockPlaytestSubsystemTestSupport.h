// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

#include "Config/FlockConfig.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "FlockEvents.h"
#include "FlockInitConfig.h"
#include "FlockPlaytestSettings.h"
#include "FlockPlaytestSubsystem.h"
#include "FlockSubsystem.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/FlockPlaytestFakeTransport.h"
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

		FScopedPlaytestSettings(bool bPlaytestingEnabled, const FString& ProtokiteApiUrl)
			: Settings(GetMutableDefault<UFlockPlaytestSettings>())
			, bSavedPlaytestingEnabled(Settings->bPlaytestingEnabled)
			, SavedProtokiteApiUrl(Settings->ProtokiteApiUrl)
		{
			Settings->bPlaytestingEnabled = bPlaytestingEnabled;
			Settings->ProtokiteApiUrl = ProtokiteApiUrl;
		}

		~FScopedPlaytestSettings()
		{
			Settings->bPlaytestingEnabled = bSavedPlaytestingEnabled;
			Settings->ProtokiteApiUrl = SavedProtokiteApiUrl;
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

		/** The account the Steam reader answers with, and how many times it was asked. */
		TSharedRef<FFlockRunningSteamAccount> SteamAccount = MakeShared<FFlockRunningSteamAccount>();
		TSharedRef<int32> SteamReads = MakeShared<int32>(0);

		explicit FPlaytestFixture(bool bTurnRetriesOff = true, bool bUseTestIdentitySources = true)
		{
			AnswerConfig(FFlockPlaytestFakeTransport::Status(200, FlockPlaytestFixtures::ConfigBody()));
			Transport->Answer(FlockPlaytestFixtures::PlaytestSessionStartRoute,
				FFlockPlaytestFakeTransport::Status(200, FlockPlaytestFixtures::SessionStartBody()));
			Transport->Answer(FlockPlaytestFixtures::PlaytestSessionEndRoute, FFlockPlaytestFakeTransport::NoContent());
			Playtest->SetHttpAdapterForTesting(Transport);
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
			Flock->ShutdownSdk();
			IFileManager::Get().DeleteDirectory(*Folder, /*bRequireExists*/ false, /*bTree*/ true);
		}

		/** Follows the Flock SDK and initializes it, which reaches Ready with the default answers. */
		void StartFlock()
		{
			Playtest->FollowFlockLifecycleForTesting(Flock);
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
