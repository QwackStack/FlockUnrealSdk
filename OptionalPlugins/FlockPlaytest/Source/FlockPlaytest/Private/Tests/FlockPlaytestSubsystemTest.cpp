// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "FlockInitConfig.h"
#include "FlockPlaytestSettings.h"
#include "FlockPlaytestSubsystem.h"
#include "FlockSubsystem.h"
#include "HAL/CriticalSection.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeLock.h"
#include "Tests/FlockPlaytestTestSupport.h"
#include "UObject/Package.h"

namespace
{
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

	FFlockInitConfig MakeFlockConfig()
	{
		FFlockInitConfig Config;
		// Nothing in these tests signs in, so nothing is sent; the address is unreachable all the same.
		Config.ApiUrl = TEXT("http://127.0.0.1:9");
		Config.ApiKey = TEXT("secret");
		Config.GameId = TEXT("flock-playtest-test");
		Config.GameVersion = TEXT("1.2.3");
		Config.GameVersionId = TEXT("pt-test-version");
		return Config;
	}

	/**
	 * Both subsystems under one game instance, built directly rather than through the game instance's
	 * subsystem collection, so each test drives the Flock SDK's lifecycle itself.
	 */
	struct FPlaytestFixture
	{
		UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
		UFlockSubsystem* Flock = NewObject<UFlockSubsystem>(GameInstance);
		UFlockPlaytestSubsystem* Playtest = NewObject<UFlockPlaytestSubsystem>(GameInstance);

		~FPlaytestFixture()
		{
			Flock->ShutdownSdk();
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

	private:
		const FName PlaytestCategory = TEXT("LogFlockPlaytest");
		FCriticalSection LinesLock;
		TArray<FLine> Lines;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSubsystemFollowsFlockLifecycleTest,
	"Flock.Playtest.Subsystem.FollowsFlockLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSubsystemFollowsFlockLifecycleTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, TEXT("http://localhost:8020"));
	FPlaytestFixture Fixture;

	Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
	ExpectPlaytestStatus(*this, TEXT("Before the Flock SDK initializes"), Fixture.Playtest->GetStatus(),
		EFlockPlaytestStatus::WaitingForFlock);

	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
	ExpectPlaytestStatus(*this, TEXT("Once the Flock SDK initializes"), Fixture.Playtest->GetStatus(),
		EFlockPlaytestStatus::Ready);

	Fixture.Flock->ShutdownSdk();
	ExpectPlaytestStatus(*this, TEXT("After the Flock SDK shuts down"), Fixture.Playtest->GetStatus(),
		EFlockPlaytestStatus::WaitingForFlock);

	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
	ExpectPlaytestStatus(*this, TEXT("After the Flock SDK initializes a second time"), Fixture.Playtest->GetStatus(),
		EFlockPlaytestStatus::Ready);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSubsystemReadyWhenFlockInitializedFirstTest,
	"Flock.Playtest.Subsystem.ReadyWhenFlockInitializedFirst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSubsystemReadyWhenFlockInitializedFirstTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, TEXT("http://localhost:8020"));
	FPlaytestFixture Fixture;

	// The production order under Auto-Initialize On Load: the Flock SDK's initialized event has already
	// fired by the time the playtest subsystem starts following it.
	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
	Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
	ExpectPlaytestStatus(*this, TEXT("Following a Flock SDK that is already initialized"),
		Fixture.Playtest->GetStatus(), EFlockPlaytestStatus::Ready);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSubsystemTurnedOffStaysOffWhenFlockInitializesTest,
	"Flock.Playtest.Subsystem.TurnedOffStaysOffWhenFlockInitializes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSubsystemTurnedOffStaysOffWhenFlockInitializesTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(false, TEXT("http://localhost:8020"));
	FPlaytestFixture Fixture;

	Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
	ExpectPlaytestStatus(*this, TEXT("Settings off, Flock SDK initialized"), Fixture.Playtest->GetStatus(),
		EFlockPlaytestStatus::TurnedOff);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSubsystemStopsFollowingFlockWhenDeinitializedTest,
	"Flock.Playtest.Subsystem.StopsFollowingFlockWhenDeinitialized",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSubsystemStopsFollowingFlockWhenDeinitializedTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, TEXT("http://localhost:8020"));
	FPlaytestFixture Fixture;

	Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
	ExpectPlaytestStatus(*this, TEXT("Before the playtest subsystem is torn down"), Fixture.Playtest->GetStatus(),
		EFlockPlaytestStatus::Ready);

	// Ready must not survive teardown: nothing may start playtest work on a subsystem that has shut down.
	Fixture.Playtest->Deinitialize();
	ExpectPlaytestStatus(*this, TEXT("Once the playtest subsystem is torn down"), Fixture.Playtest->GetStatus(),
		EFlockPlaytestStatus::Stopped);

	// Had the shut-down event still reached the torn-down subsystem, it would have decided again and moved to
	// WaitingForFlock.
	Fixture.Flock->ShutdownSdk();
	ExpectPlaytestStatus(*this, TEXT("Flock SDK shut down after the playtest subsystem was torn down"),
		Fixture.Playtest->GetStatus(), EFlockPlaytestStatus::Stopped);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSubsystemLogsEachChangeOnceTest,
	"Flock.Playtest.Subsystem.LogsEachChangeOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSubsystemLogsEachChangeOnceTest::RunTest(const FString& Parameters)
{
	{
		// A setting that stops playtesting is a warning, logged once however often the Flock SDK changes state.
		FScopedPlaytestSettings Settings(true, TEXT("localhost:8020"));
		FPlaytestFixture Fixture;
		FPlaytestLogCapture Capture;

		Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
		Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
		Fixture.Flock->ShutdownSdk();

		const TArray<FPlaytestLogCapture::FLine> Lines = Capture.LinesAtLogOrLouder();
		if (TestEqual(TEXT("An unusable URL is logged once"), Lines.Num(), 1))
		{
			TestEqual(TEXT("It is logged as a warning"), static_cast<int32>(Lines[0].Verbosity),
				static_cast<int32>(ELogVerbosity::Warning));
			TestTrue(TEXT("It quotes the value, so a stray space would show"),
				Lines[0].Message.Contains(TEXT("'localhost:8020'")));
		}
	}
	{
		// With playtesting turned on, waiting for the Flock SDK and being ready are each logged once, at Log.
		FScopedPlaytestSettings Settings(true, TEXT("http://localhost:8020"));
		FPlaytestFixture Fixture;
		FPlaytestLogCapture Capture;

		Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
		Fixture.Flock->InitializeWithConfig(MakeFlockConfig());

		const TArray<FPlaytestLogCapture::FLine> Lines = Capture.LinesAtLogOrLouder();
		if (TestEqual(TEXT("Waiting and ready are logged once each"), Lines.Num(), 2))
		{
			TestEqual(TEXT("Waiting is logged at Log"), static_cast<int32>(Lines[0].Verbosity),
				static_cast<int32>(ELogVerbosity::Log));
			TestTrue(TEXT("The first line is the wait"),
				Lines[0].Message.Contains(DescribePlaytestStatus(EFlockPlaytestStatus::WaitingForFlock)));
			TestEqual(TEXT("Ready is logged at Log"), static_cast<int32>(Lines[1].Verbosity),
				static_cast<int32>(ELogVerbosity::Log));
			TestTrue(TEXT("The second line is ready"),
				Lines[1].Message.Contains(DescribePlaytestStatus(EFlockPlaytestStatus::Ready)));
		}
	}
	{
		// Turned off is the chosen state of every build that is not a playtest build, so it says nothing at Log.
		FScopedPlaytestSettings Settings(false, TEXT("http://localhost:8020"));
		FPlaytestFixture Fixture;
		FPlaytestLogCapture Capture;

		Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
		Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
		TestEqual(TEXT("Turned off logs nothing at Log or louder"), Capture.LinesAtLogOrLouder().Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSubsystemGameInstanceFollowsItsFlockTest,
	"Flock.Playtest.Subsystem.GameInstanceFollowsItsFlock",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSubsystemGameInstanceFollowsItsFlockTest::RunTest(const FString& Parameters)
{
	// Runs only outside the editor, where the running game's game instance had its subsystems created by the
	// engine: the one place the real start-up path, not the testing entry point, is what did the following.
	int32 GameInstancesChecked = 0;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UGameInstance* GameInstance = Context.OwningGameInstance;
		if (!GameInstance)
		{
			continue;
		}
		++GameInstancesChecked;

		UFlockPlaytestSubsystem* Playtest = GameInstance->GetSubsystem<UFlockPlaytestSubsystem>();
		UFlockSubsystem* Flock = GameInstance->GetSubsystem<UFlockSubsystem>();
		if (TestNotNull(TEXT("The game instance has a playtest subsystem"), Playtest)
			&& TestNotNull(TEXT("The game instance has a Flock subsystem"), Flock))
		{
			TestTrue(TEXT("The playtest subsystem follows its own game instance's Flock subsystem"),
				Playtest->GetFollowedFlockForTesting() == Flock);
		}
	}
	TestTrue(TEXT("A running game instance was found to check"), GameInstancesChecked > 0);
	return true;
}

#endif
