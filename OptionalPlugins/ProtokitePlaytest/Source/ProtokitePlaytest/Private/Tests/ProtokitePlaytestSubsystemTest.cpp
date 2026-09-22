// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Config/FlockConfig.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "FlockEvents.h"
#include "FlockInitConfig.h"
#include "ProtokitePlaytestSettings.h"
#include "ProtokitePlaytestSubsystem.h"
#include "FlockSubsystem.h"
#include "HAL/CriticalSection.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeLock.h"
#include "Tests/ProtokitePlaytestFakeTransport.h"
#include "Tests/ProtokitePlaytestSubsystemTestSupport.h"
#include "Tests/ProtokitePlaytestTestSupport.h"
#include "UObject/Package.h"

using namespace ProtokitePlaytestSubsystemTesting;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSubsystemFollowsFlockLifecycleTest,
	"Protokite.Playtest.Subsystem.FollowsFlockLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSubsystemFollowsFlockLifecycleTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;

	Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
	ExpectPlaytestStatus(*this, TEXT("Before the Flock SDK initializes"), Fixture.Playtest->GetStatus(),
		EProtokitePlaytestStatus::WaitingForFlock);

	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
	ExpectPlaytestStatus(*this, TEXT("Once the Flock SDK initializes and the config is fetched"),
		Fixture.Playtest->GetStatus(), EProtokitePlaytestStatus::Ready);

	Fixture.Flock->ShutdownSdk();
	ExpectPlaytestStatus(*this, TEXT("After the Flock SDK shuts down"), Fixture.Playtest->GetStatus(),
		EProtokitePlaytestStatus::WaitingForFlock);
	TestTrue(TEXT("The config is forgotten with the Flock initialization it belonged to"),
		Fixture.Playtest->GetPlaytestConfig().TestId.IsEmpty());

	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
	ExpectPlaytestStatus(*this, TEXT("After the Flock SDK initializes a second time"), Fixture.Playtest->GetStatus(),
		EProtokitePlaytestStatus::Ready);
	TestEqual(TEXT("Each Flock initialization fetches its own config"), Fixture.ConfigRequests(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSubsystemReadyWhenFlockInitializedFirstTest,
	"Protokite.Playtest.Subsystem.ReadyWhenFlockInitializedFirst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSubsystemReadyWhenFlockInitializedFirstTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;

	// The production order under Auto-Initialize On Load: the Flock SDK's initialized event has already
	// fired by the time the playtest subsystem starts following it.
	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
	Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
	ExpectPlaytestStatus(*this, TEXT("Following a Flock SDK that is already initialized"),
		Fixture.Playtest->GetStatus(), EProtokitePlaytestStatus::Ready);
	TestEqual(TEXT("One config request"), Fixture.ConfigRequests(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSubsystemTurnedOffStaysOffWhenFlockInitializesTest,
	"Protokite.Playtest.Subsystem.TurnedOffStaysOffWhenFlockInitializes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSubsystemTurnedOffStaysOffWhenFlockInitializesTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(false, UsableUrl);
	FPlaytestFixture Fixture;

	Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
	ExpectPlaytestStatus(*this, TEXT("Settings off, Flock SDK initialized"), Fixture.Playtest->GetStatus(),
		EProtokitePlaytestStatus::TurnedOff);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSubsystemStopsFollowingFlockWhenDeinitializedTest,
	"Protokite.Playtest.Subsystem.StopsFollowingFlockWhenDeinitialized",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSubsystemStopsFollowingFlockWhenDeinitializedTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;

	Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
	ExpectPlaytestStatus(*this, TEXT("Before the playtest subsystem is torn down"), Fixture.Playtest->GetStatus(),
		EProtokitePlaytestStatus::Ready);
	TestTrue(TEXT("It listens for Flock sessions starting while it follows the Flock SDK"),
		Fixture.Flock->GetEvents()->OnSessionStarted.Contains(Fixture.Playtest, TEXT("HandleFlockSessionStarted")));
	TestTrue(TEXT("It listens for Flock sessions reaching the server while it follows the Flock SDK"),
		Fixture.Flock->GetEvents()->OnSessionRegistered.Contains(Fixture.Playtest, TEXT("HandleFlockSessionRegistered")));

	// Ready must not survive teardown: nothing may start playtest work on a subsystem that has shut down.
	Fixture.Playtest->Deinitialize();
	ExpectPlaytestStatus(*this, TEXT("Once the playtest subsystem is torn down"), Fixture.Playtest->GetStatus(),
		EProtokitePlaytestStatus::Stopped);
	TestFalse(TEXT("It stops listening for Flock sessions starting once torn down"),
		Fixture.Flock->GetEvents()->OnSessionStarted.Contains(Fixture.Playtest, TEXT("HandleFlockSessionStarted")));
	TestFalse(TEXT("It stops listening for Flock sessions reaching the server once torn down"),
		Fixture.Flock->GetEvents()->OnSessionRegistered.Contains(Fixture.Playtest, TEXT("HandleFlockSessionRegistered")));
	TestFalse(TEXT("No feature is on once stopped"),
		Fixture.Playtest->IsPlaytestFeatureEnabled(ProtokitePlaytestFeatures::VideoRecording));

	// Had the shut-down event still reached the torn-down subsystem, it would have decided again and moved to
	// WaitingForFlock.
	Fixture.Flock->ShutdownSdk();
	ExpectPlaytestStatus(*this, TEXT("Flock SDK shut down after the playtest subsystem was torn down"),
		Fixture.Playtest->GetStatus(), EProtokitePlaytestStatus::Stopped);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSubsystemLogsEachChangeOnceTest,
	"Protokite.Playtest.Subsystem.LogsEachChangeOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSubsystemLogsEachChangeOnceTest::RunTest(const FString& Parameters)
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
		// With playtesting turned on, waiting for the Flock SDK, fetching, being ready and then waiting for a Flock session
		// are each logged once, at Log.
		FScopedPlaytestSettings Settings(true, UsableUrl);
		FPlaytestFixture Fixture;
		Fixture.Transport->bHoldReplies = true;
		FPlaytestLogCapture Capture;

		Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
		Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
		Fixture.Transport->ReleaseAllHeldReplies();

		const TArray<FPlaytestLogCapture::FLine> Lines = Capture.LinesAtLogOrLouder();
		if (TestEqual(TEXT("Waiting, fetching, ready and waiting for a Flock session are logged once each"), Lines.Num(), 4))
		{
			const EProtokitePlaytestStatus Expected[] = {
				EProtokitePlaytestStatus::WaitingForFlock, EProtokitePlaytestStatus::FetchingPlaytestConfig, EProtokitePlaytestStatus::Ready };
			for (int32 Index = 0; Index < 3; ++Index)
			{
				TestEqual(FString::Printf(TEXT("Line %d is logged at Log"), Index), static_cast<int32>(Lines[Index].Verbosity),
					static_cast<int32>(ELogVerbosity::Log));
				TestTrue(FString::Printf(TEXT("Line %d describes the expected status"), Index),
					Lines[Index].Message.Contains(DescribePlaytestStatus(Expected[Index])));
			}
			TestEqual(TEXT("Line 3 is logged at Log"), static_cast<int32>(Lines[3].Verbosity), static_cast<int32>(ELogVerbosity::Log));
			TestTrue(TEXT("Line 3 says the Protokite session waits for a Flock session"),
				Lines[3].Message.Contains(TEXT("starts once a Flock session reaches the server")));
		}
	}
	{
		// Turned off is the chosen state of every build that is not a playtest build, so it says nothing at Log.
		FScopedPlaytestSettings Settings(false, UsableUrl);
		FPlaytestFixture Fixture;
		FPlaytestLogCapture Capture;

		Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
		Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
		TestEqual(TEXT("Turned off logs nothing at Log or louder"), Capture.LinesAtLogOrLouder().Num(), 0);
	}
	{
		// A build with no linked playtest is a warning naming the version it sent.
		FScopedPlaytestSettings Settings(true, UsableUrl);
		FPlaytestFixture Fixture;
		Fixture.AnswerConfig(FProtokitePlaytestFakeTransport::Status(404, ProtokitePlaytestFixtures::NotLinkedBody));
		FPlaytestLogCapture Capture;

		Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
		Fixture.Flock->InitializeWithConfig(MakeFlockConfig());

		const TArray<FPlaytestLogCapture::FLine> Lines = Capture.LinesAtLogOrLouder();
		if (TestEqual(TEXT("Waiting, then not linked"), Lines.Num(), 2))
		{
			TestEqual(TEXT("Not linked is a warning"), static_cast<int32>(Lines[1].Verbosity),
				static_cast<int32>(ELogVerbosity::Warning));
			TestTrue(TEXT("It names the version this build sent"), Lines[1].Message.Contains(ProtokitePlaytestFixtures::GameVersionId));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSubsystemSendsFlocksOwnHeadersTest,
	"Protokite.Playtest.Subsystem.SendsFlocksOwnHeaders",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSubsystemSendsFlocksOwnHeadersTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, TEXT("http://localhost:8020/"));
	FPlaytestFixture Fixture;

	// The config the Flock SDK is initialized with here is not the project's settings, which is exactly the case
	// where reading the settings instead would send the wrong key.
	Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());

	if (TestEqual(TEXT("One config request"), Fixture.ConfigRequests(), 1))
	{
		const FFlockHttpRequest& Request = Fixture.Transport->Requests[0];
		TestEqual(TEXT("To the playtest-config route of the Protokite API URL"), Request.Url,
			FString(TEXT("http://localhost:8020/game/sdk/playtest-config")));
		TestEqual(TEXT("With the API key the Flock SDK initialized with"), Request.Headers.FindRef(TEXT("X-Flock-API-Key")),
			FString(TEXT("secret")));
		TestEqual(TEXT("With the version id the Flock SDK initialized with"), Request.Headers.FindRef(TEXT("X-Game-Version-ID")),
			FString(ProtokitePlaytestFixtures::GameVersionId));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSubsystemRefusalsBecomeStatusesTest,
	"Protokite.Playtest.Subsystem.RefusalsBecomeStatuses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSubsystemRefusalsBecomeStatusesTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		FFlockHttpResponse Response;
		EProtokitePlaytestStatus Expected;
		const TCHAR* What;
	};
	const FCase Cases[] = {
		{ FProtokitePlaytestFakeTransport::Status(404, ProtokitePlaytestFixtures::NotLinkedBody), EProtokitePlaytestStatus::PlaytestNotLinked, TEXT("404") },
		{ FProtokitePlaytestFakeTransport::Status(401, ProtokitePlaytestFixtures::InvalidApiKeyBody), EProtokitePlaytestStatus::ProtokiteRefusedApiKey, TEXT("401") },
		{ FProtokitePlaytestFakeTransport::Status(422, ProtokitePlaytestFixtures::MissingApiKeyBody), EProtokitePlaytestStatus::ProtokiteRefusedApiKey, TEXT("422") },
		{ FProtokitePlaytestFakeTransport::Status(403, TEXT("<html>Forbidden</html>")), EProtokitePlaytestStatus::PlaytestConfigUnavailable, TEXT("A 403 from something on the way") },
		{ FProtokitePlaytestFakeTransport::Status(503, ProtokitePlaytestFixtures::FlockUnreachableBody), EProtokitePlaytestStatus::PlaytestConfigUnavailable, TEXT("503") },
		{ FProtokitePlaytestFakeTransport::ConnectionFailure(), EProtokitePlaytestStatus::PlaytestConfigUnavailable, TEXT("No connection") },
		{ FProtokitePlaytestFakeTransport::Status(200, TEXT("<html>captive portal</html>")), EProtokitePlaytestStatus::PlaytestConfigUnavailable, TEXT("An unreadable 200") },
	};
	for (const FCase& Case : Cases)
	{
		FScopedPlaytestSettings Settings(true, UsableUrl);
		FPlaytestFixture Fixture;
		Fixture.AnswerConfig(Case.Response);

		Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
		Fixture.Flock->InitializeWithConfig(MakeFlockConfig());

		ExpectPlaytestStatus(*this, Case.What, Fixture.Playtest->GetStatus(), Case.Expected);
		TestTrue(FString::Printf(TEXT("%s leaves no config"), Case.What), Fixture.Playtest->GetPlaytestConfig().TestId.IsEmpty());
		TestFalse(FString::Printf(TEXT("%s leaves every feature off"), Case.What),
			Fixture.Playtest->IsPlaytestFeatureEnabled(ProtokitePlaytestFeatures::VideoRecording));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSubsystemRetriesStopWhenTheConfigIsForgottenTest,
	"Protokite.Playtest.Subsystem.RetriesStopWhenTheConfigIsForgotten",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSubsystemRetriesStopWhenTheConfigIsForgottenTest::RunTest(const FString& Parameters)
{
	enum class EWhatEnds : uint8
	{
		Nothing,
		FlockSdk,
		PlaytestSubsystem,
	};
	struct FCase
	{
		EWhatEnds WhatEnds;
		int32 ExpectedRequests;
		const TCHAR* What;
	};
	// Protokite keeps failing and three retries are allowed, so a fetch nobody stops asks four times.
	const FCase Cases[] = {
		{ EWhatEnds::Nothing, 4, TEXT("Nothing ends, so every retry is sent") },
		{ EWhatEnds::FlockSdk, 1, TEXT("The Flock SDK shuts down, so no retry is sent with its key") },
		{ EWhatEnds::PlaytestSubsystem, 1, TEXT("The playtest subsystem is torn down, so no retry is sent") },
	};
	for (const FCase& Case : Cases)
	{
		FScopedPlaytestSettings Settings(true, UsableUrl);
		FPlaytestFixture Fixture;
		Fixture.AnswerConfig(FProtokitePlaytestFakeTransport::Status(503, ProtokitePlaytestFixtures::FlockUnreachableBody));
		FFlockRetryPolicy ThreeQuickRetries;
		ThreeQuickRetries.MaxRetries = 3;
		ThreeQuickRetries.InitialDelaySeconds = 0.f;
		ThreeQuickRetries.bUseJitter = false;
		Fixture.Playtest->SetRetryPolicyForTesting(ThreeQuickRetries);

		Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
		Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
		if (Case.WhatEnds == EWhatEnds::FlockSdk)
		{
			Fixture.Flock->ShutdownSdk();
		}
		else if (Case.WhatEnds == EWhatEnds::PlaytestSubsystem)
		{
			Fixture.Playtest->Deinitialize();
		}
		RunPendingPlaytestRetries();

		TestEqual(Case.What, Fixture.ConfigRequests(), Case.ExpectedRequests);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSubsystemUsesTheFlockSdkRetrySettingsTest,
	"Protokite.Playtest.Subsystem.UsesTheFlockSdkRetrySettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSubsystemUsesTheFlockSdkRetrySettingsTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockRetrySettings RetrySettings(1, false);
	// No retry policy is handed in, so the subsystem has to read the Flock SDK's own settings, as a game does.
	FPlaytestFixture Fixture(/*bTurnRetriesOff*/ false);
	Fixture.AnswerConfig(FProtokitePlaytestFakeTransport::Status(503, ProtokitePlaytestFixtures::FlockUnreachableBody));

	Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
	RunPendingPlaytestRetries();

	TestEqual(TEXT("The first attempt and the one retry the Flock SDK's settings allow"), Fixture.ConfigRequests(), 2);
	ExpectPlaytestStatus(*this, TEXT("Once the retries are spent"), Fixture.Playtest->GetStatus(),
		EProtokitePlaytestStatus::PlaytestConfigUnavailable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSubsystemConfigForAnotherVersionIsRefusedTest,
	"Protokite.Playtest.Subsystem.ConfigForAnotherVersionIsRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSubsystemConfigForAnotherVersionIsRefusedTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	Fixture.AnswerConfig(FProtokitePlaytestFakeTransport::Status(200, ProtokitePlaytestFixtures::ConfigBody(TEXT("pt-other-version"))));

	Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());

	ExpectPlaytestStatus(*this, TEXT("A config for another version"), Fixture.Playtest->GetStatus(),
		EProtokitePlaytestStatus::PlaytestConfigForAnotherVersion);
	TestFalse(TEXT("Its features stay off"), Fixture.Playtest->IsPlaytestFeatureEnabled(ProtokitePlaytestFeatures::VideoRecording));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSubsystemNoRequestUntilAllowedTest,
	"Protokite.Playtest.Subsystem.NoRequestUntilAllowed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSubsystemNoRequestUntilAllowedTest::RunTest(const FString& Parameters)
{
	{
		// Installed but turned off: a whole Flock lifecycle sends nothing to Protokite.
		FScopedPlaytestSettings Settings(false, UsableUrl);
		FPlaytestFixture Fixture;
		Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
		Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
		Fixture.Flock->ShutdownSdk();
		Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
		TestEqual(TEXT("Turned off: no request of any kind"), Fixture.Transport->Requests.Num(), 0);
	}
	{
		FScopedPlaytestSettings Settings(true, TEXT("localhost:8020"));
		FPlaytestFixture Fixture;
		Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
		Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
		TestEqual(TEXT("Unusable URL: no request of any kind"), Fixture.Transport->Requests.Num(), 0);
	}
	{
		FScopedPlaytestSettings Settings(true, UsableUrl);
		FPlaytestFixture Fixture;
		Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
		TestEqual(TEXT("Flock SDK not initialized: no request of any kind"), Fixture.Transport->Requests.Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSubsystemOneFetchPerFlockInitializationTest,
	"Protokite.Playtest.Subsystem.OneFetchPerFlockInitialization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSubsystemOneFetchPerFlockInitializationTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	Fixture.Transport->bHoldReplies = true;

	Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
	TestEqual(TEXT("The fetch is out"), Fixture.ConfigRequests(), 1);

	// Another initialized event while the first fetch is still out must not send a second request.
	Fixture.Flock->GetEvents()->InvokeInitialized();
	TestEqual(TEXT("Still one request"), Fixture.ConfigRequests(), 1);

	Fixture.Transport->ReleaseAllHeldReplies();
	ExpectPlaytestStatus(*this, TEXT("The one reply loads the config"), Fixture.Playtest->GetStatus(), EProtokitePlaytestStatus::Ready);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSubsystemReplyAfterFlockShutdownIsIgnoredTest,
	"Protokite.Playtest.Subsystem.ReplyAfterFlockShutdownIsIgnored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSubsystemReplyAfterFlockShutdownIsIgnoredTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	Fixture.Transport->bHoldReplies = true;

	Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
	Fixture.Flock->ShutdownSdk();
	Fixture.Transport->ReleaseAllHeldReplies();

	ExpectPlaytestStatus(*this, TEXT("A reply for a Flock initialization that has ended"), Fixture.Playtest->GetStatus(),
		EProtokitePlaytestStatus::WaitingForFlock);
	TestTrue(TEXT("It leaves no config"), Fixture.Playtest->GetPlaytestConfig().TestId.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSubsystemReplyFromAnEarlierInitializationIsIgnoredTest,
	"Protokite.Playtest.Subsystem.ReplyFromAnEarlierInitializationIsIgnored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSubsystemReplyFromAnEarlierInitializationIsIgnoredTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	const FString SecondVersion = TEXT("pt-test-version-2");
	Fixture.Transport->AnswerInOrder(ProtokitePlaytestFixtures::PlaytestConfigRoute, {
		FProtokitePlaytestFakeTransport::Status(200, ProtokitePlaytestFixtures::ConfigBody()),
		FProtokitePlaytestFakeTransport::Status(200, ProtokitePlaytestFixtures::ConfigBody(SecondVersion)) });
	Fixture.Transport->bHoldReplies = true;

	Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
	Fixture.Flock->ShutdownSdk();
	Fixture.Flock->InitializeWithConfig(MakeFlockConfig(SecondVersion));
	TestEqual(TEXT("One fetch per Flock initialization"), Fixture.ConfigRequests(), 2);

	// The first initialization's reply lands after the second fetch went out.
	Fixture.Transport->ReleaseOldestHeldReply();
	ExpectPlaytestStatus(*this, TEXT("The earlier reply is ignored"), Fixture.Playtest->GetStatus(),
		EProtokitePlaytestStatus::FetchingPlaytestConfig);

	Fixture.Transport->ReleaseOldestHeldReply();
	ExpectPlaytestStatus(*this, TEXT("The current reply loads"), Fixture.Playtest->GetStatus(), EProtokitePlaytestStatus::Ready);
	TestEqual(TEXT("With the current initialization's version"), Fixture.Playtest->GetPlaytestConfig().FlockGameVersionId,
		SecondVersion);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSubsystemReplyAfterTeardownIsIgnoredTest,
	"Protokite.Playtest.Subsystem.ReplyAfterTeardownIsIgnored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSubsystemReplyAfterTeardownIsIgnoredTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	Fixture.Transport->bHoldReplies = true;

	Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
	Fixture.Playtest->Deinitialize();
	Fixture.Transport->ReleaseAllHeldReplies();

	ExpectPlaytestStatus(*this, TEXT("A reply after teardown"), Fixture.Playtest->GetStatus(), EProtokitePlaytestStatus::Stopped);
	TestFalse(TEXT("No feature is on"), Fixture.Playtest->IsPlaytestFeatureEnabled(ProtokitePlaytestFeatures::VideoRecording));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSubsystemFeaturesAreOnOnlyWhileReadyTest,
	"Protokite.Playtest.Subsystem.FeaturesAreOnOnlyWhileReady",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSubsystemFeaturesAreOnOnlyWhileReadyTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	// The config turns video recording on, so the switch followed here is one the config really turns on. No game viewport
	// is ever found, so nothing is recorded.
	Fixture.AnswerConfig(FProtokitePlaytestFakeTransport::Status(200,
		ProtokitePlaytestFixtures::ConfigBody(ProtokitePlaytestFixtures::GameVersionId, /*bHeavyAnalytics*/ false, /*bVideoRecording*/ true)));
	Fixture.Playtest->SetVideoFrameSourceFactoryForTesting([](FIntPoint, FString&) -> TSharedPtr<IProtokitePlaytestVideoFrameSource> { return nullptr; });
	Fixture.Transport->bHoldReplies = true;

	Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
	ExpectPlaytestStatus(*this, TEXT("Fetching"), Fixture.Playtest->GetStatus(), EProtokitePlaytestStatus::FetchingPlaytestConfig);
	TestFalse(TEXT("Video recording is off while fetching"),
		Fixture.Playtest->IsPlaytestFeatureEnabled(ProtokitePlaytestFeatures::VideoRecording));

	Fixture.Transport->ReleaseAllHeldReplies();
	TestTrue(TEXT("Video recording is on once ready"), Fixture.Playtest->IsPlaytestFeatureEnabled(ProtokitePlaytestFeatures::VideoRecording));
	TestTrue(TEXT("Exception capturing is on once ready"),
		Fixture.Playtest->IsPlaytestFeatureEnabled(ProtokitePlaytestFeatures::ExceptionCapturing));
	TestFalse(TEXT("Heavy analytics stays off: the config says false"),
		Fixture.Playtest->IsPlaytestFeatureEnabled(ProtokitePlaytestFeatures::HeavyAnalytics));

	// Turning playtesting off keeps the config that was loaded, and the next decision moves the status away from
	// Ready. The features follow the status, not the config still held.
	Settings.Settings->bPlaytestingEnabled = false;
	Fixture.Flock->GetEvents()->InvokeInitialized();
	ExpectPlaytestStatus(*this, TEXT("Turned off after it was ready"), Fixture.Playtest->GetStatus(),
		EProtokitePlaytestStatus::TurnedOff);
	TestFalse(TEXT("Video recording is off once playtesting is turned off"),
		Fixture.Playtest->IsPlaytestFeatureEnabled(ProtokitePlaytestFeatures::VideoRecording));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSubsystemAsksAgainWhenTheNextSessionStartsTest,
	"Protokite.Playtest.Subsystem.AsksAgainWhenTheNextSessionStarts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSubsystemAsksAgainWhenTheNextSessionStartsTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	// Protokite cannot be reached at start-up, and is back by the time a session starts.
	Fixture.Transport->AnswerInOrder(ProtokitePlaytestFixtures::PlaytestConfigRoute, {
		FProtokitePlaytestFakeTransport::ConnectionFailure(),
		FProtokitePlaytestFakeTransport::Status(200, ProtokitePlaytestFixtures::ConfigBody()) });

	Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
	Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
	ExpectPlaytestStatus(*this, TEXT("Protokite could not be reached at start-up"), Fixture.Playtest->GetStatus(),
		EProtokitePlaytestStatus::PlaytestConfigUnavailable);

	Fixture.Flock->GetEvents()->InvokeSessionStarted(TEXT("session-1"));
	TestEqual(TEXT("A session starting asks again"), Fixture.ConfigRequests(), 2);
	ExpectPlaytestStatus(*this, TEXT("The second answer loads the playtest"), Fixture.Playtest->GetStatus(),
		EProtokitePlaytestStatus::Ready);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSubsystemSessionStartAsksAgainOnlyWhenProtokiteWasUnreachableTest,
	"Protokite.Playtest.Subsystem.SessionStartAsksAgainOnlyWhenProtokiteWasUnreachable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSubsystemSessionStartAsksAgainOnlyWhenProtokiteWasUnreachableTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		FFlockHttpResponse Response;
		bool bHoldReply;
		EProtokitePlaytestStatus Expected;
		const TCHAR* What;
	};
	// A refusal would get the same answer twice, and a playtest loaded or on its way needs nothing more.
	const FCase Cases[] = {
		{ FProtokitePlaytestFakeTransport::Status(200, ProtokitePlaytestFixtures::ConfigBody()), false, EProtokitePlaytestStatus::Ready, TEXT("A loaded playtest") },
		{ FProtokitePlaytestFakeTransport::Status(404, ProtokitePlaytestFixtures::NotLinkedBody), false, EProtokitePlaytestStatus::PlaytestNotLinked, TEXT("No linked playtest") },
		{ FProtokitePlaytestFakeTransport::Status(401, ProtokitePlaytestFixtures::InvalidApiKeyBody), false, EProtokitePlaytestStatus::ProtokiteRefusedApiKey, TEXT("A refused key") },
		{ FProtokitePlaytestFakeTransport::Status(200, ProtokitePlaytestFixtures::ConfigBody(TEXT("pt-other-version"))), false, EProtokitePlaytestStatus::PlaytestConfigForAnotherVersion, TEXT("A playtest for another version") },
		{ FProtokitePlaytestFakeTransport::Status(200, ProtokitePlaytestFixtures::ConfigBody()), true, EProtokitePlaytestStatus::FetchingPlaytestConfig, TEXT("A fetch still on its way") },
	};
	for (const FCase& Case : Cases)
	{
		FScopedPlaytestSettings Settings(true, UsableUrl);
		FPlaytestFixture Fixture;
		Fixture.AnswerConfig(Case.Response);
		Fixture.Transport->bHoldReplies = Case.bHoldReply;

		Fixture.Playtest->FollowFlockLifecycleForTesting(Fixture.Flock);
		Fixture.Flock->InitializeWithConfig(MakeFlockConfig());
		Fixture.Flock->GetEvents()->InvokeSessionStarted(TEXT("session-1"));

		TestEqual(FString::Printf(TEXT("%s: a session starting sends nothing more"), Case.What), Fixture.ConfigRequests(), 1);
		ExpectPlaytestStatus(*this, Case.What, Fixture.Playtest->GetStatus(), Case.Expected);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestSubsystemGameInstanceFollowsItsFlockTest,
	"Protokite.Playtest.Subsystem.GameInstanceFollowsItsFlock",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestSubsystemGameInstanceFollowsItsFlockTest::RunTest(const FString& Parameters)
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

		UProtokitePlaytestSubsystem* Playtest = GameInstance->GetSubsystem<UProtokitePlaytestSubsystem>();
		UFlockSubsystem* Flock = GameInstance->GetSubsystem<UFlockSubsystem>();
		if (TestNotNull(TEXT("The game instance has a playtest subsystem"), Playtest)
			&& TestNotNull(TEXT("The game instance has a Flock subsystem"), Flock))
		{
			TestTrue(TEXT("The playtest subsystem follows its own game instance's Flock subsystem"),
				Playtest->GetFollowedFlockForTesting() == Flock);
			TestTrue(TEXT("It listens for its Flock sessions reaching the server, which start the Protokite session"),
				Flock->GetEvents()->OnSessionRegistered.Contains(Playtest, TEXT("HandleFlockSessionRegistered")));
		}
	}
	TestTrue(TEXT("A running game instance was found to check"), GameInstancesChecked > 0);
	return true;
}

#endif
