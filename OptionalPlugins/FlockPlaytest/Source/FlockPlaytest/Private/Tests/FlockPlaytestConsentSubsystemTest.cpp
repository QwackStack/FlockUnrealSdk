// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockPlaytestConsent.h"
#include "FlockPlaytestRecordingsFolder.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/FlockPlaytestSubsystemTestSupport.h"
#include "Tests/FlockPlaytestTestSupport.h"

using namespace FlockPlaytestSubsystemTesting;
using namespace FlockPlaytestFixtures;

namespace
{
	/** The Flock session this file's tests name, so a Protokite session can start when one is allowed to. */
	const FString ConsentTestFlockSessionId = TEXT("01M2N94CCCCCCCCCCCCCCCCCCC");

	/** The extra_debug object of the last session start; null when none was sent. */
	TSharedPtr<FJsonObject> LastSessionExtraDebug(const FPlaytestFixture& Fixture)
	{
		const TSharedPtr<FJsonObject> Body = Fixture.LastSessionStartBody();
		const TSharedPtr<FJsonObject>* Debug = nullptr;
		return Body.IsValid() && HasMemberSpelled(Body, TEXT("extra_debug")) && Body->TryGetObjectField(TEXT("extra_debug"), Debug)
			&& Debug != nullptr
			? *Debug
			: nullptr;
	}

	/** A finished recording an earlier launch could not send, with the session it belongs to saved beside it. */
	bool PlantARecordingAnEarlierLaunchLeft(FAutomationTestBase& Test, const FString& RecordingsFolder)
	{
		FString Error;
		const TSharedPtr<FFlockPlaytestRecordingRun> Run = FFlockPlaytestRecordingRun::CreateNamedForTesting(
			RecordingsFolder, EFlockPlaytestRecordingKind::Playtest, TEXT("2026-09-21-000000-aaaaaaaa"), /*ReservedBytes*/ 0, Error);
		if (!Test.TestTrue(TEXT("The earlier launch's recording was made"), Run.IsValid()))
		{
			return false;
		}
		FFileHelper::SaveStringToFile(TEXT("not really a video"), *Run->GetVideoFilePath());
		FFlockPlaytestRecordingSession Session;
		Session.ProtokiteSessionId = PlaytestSessionId;
		Session.ProtokiteApiUrl = UsableUrl;
		Session.FlockGameVersionId = GameVersionId;
		Run->SaveSession(Session, Error);
		// Let go of, so this launch finds it the way it finds one an ended launch left.
		return true;
	}

	/** Where a link for that recording would be asked for. */
	FString UploadLinkRouteForTheKeptRecording()
	{
		return FString(TEXT("/game/sdk/playtest-session/")) + PlaytestSessionId + TEXT("/recording-upload");
	}

	/** A playtest that asks for everything, so what is collected is decided by the player's answer alone. */
	void AnswerConfigWithEveryFeature(FPlaytestFixture& Fixture)
	{
		Fixture.AnswerConfig(FFlockPlaytestFakeTransport::Status(200,
			ConfigBody(GameVersionId, /*bHeavyAnalytics*/ true, /*bVideoRecording*/ true)));
	}
}

/**
 * Nothing is collected until the player has answered. Without this the question would be decoration: a playtest that
 * records while it asks has already done the thing it is asking about.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestConsentWaitsForTheAnswerTest,
	"Flock.Playtest.Consent.NothingIsCollectedUntilThePlayerAnswers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestConsentWaitsForTheAnswerTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings Analytics(/*bAnalyticsEnabled*/ true);
	// A player who has not been asked yet.
	FPlaytestFixture Fixture(/*bTurnRetriesOff*/ true, /*bUseTestIdentitySources*/ true, EFlockPlaytestConsentChoice::NotAnswered);
	AnswerConfigWithEveryFeature(Fixture);
	Fixture.StartFlock();

	ExpectPlaytestStatus(*this, TEXT("It waits for the player"), Fixture.Playtest->GetStatus(),
		EFlockPlaytestStatus::WaitingForPlayerConsent);
	TestEqual(TEXT("The playtest config was still fetched, since there is nothing to ask about without it"),
		Fixture.ConfigRequests(), 1);

	TestFalse(TEXT("Video recording is off"),
		Fixture.Playtest->IsPlaytestFeatureEnabled(FlockPlaytestFeatures::VideoRecording));
	TestFalse(TEXT("Heavy analytics is off"),
		Fixture.Playtest->IsPlaytestFeatureEnabled(FlockPlaytestFeatures::HeavyAnalytics));
	TestFalse(TEXT("Nothing is being measured"), Fixture.Playtest->IsMeasuringPerformance());
	TestFalse(TEXT("Nothing is being recorded"), Fixture.Playtest->IsRecordingVideo());
	TestFalse(TEXT("And the game's own playtest events are refused"),
		Fixture.Playtest->RecordPlaytestEvent(TEXT("waiting_for_an_answer")));

	// The question itself needs a viewport, which a headless test has none of; what it can see is that the playtest
	// asked for it. Without this, deleting the call from RefreshStatus leaves every test green and no player is ever
	// asked anything.
	TestTrue(TEXT("The player is being asked, as soon as there is somewhere to draw the question"),
		Fixture.Playtest->IsWaitingToAskForConsentForTesting());

	// Even with a Flock session on the server, which is everything else a session start waits for.
	Fixture.RegisterFlockSession(ConsentTestFlockSessionId);
	TestEqual(TEXT("No Protokite session is started, so this launch is not on the playtest at all"),
		Fixture.SessionStarts(), 0);
	return true;
}

/**
 * Answering starts the playtest, which is the counter-case for the test above: without it, a build that collected
 * nothing whatever the answer would pass that one perfectly.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestConsentAnsweringStartsItTest,
	"Flock.Playtest.Consent.AnsweringStartsThePlaytest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestConsentAnsweringStartsItTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings Analytics(/*bAnalyticsEnabled*/ true);
	FPlaytestFixture Fixture(/*bTurnRetriesOff*/ true, /*bUseTestIdentitySources*/ true, EFlockPlaytestConsentChoice::NotAnswered);
	AnswerConfigWithEveryFeature(Fixture);
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(ConsentTestFlockSessionId);
	TestEqual(TEXT("Precondition: nothing has been sent"), Fixture.SessionStarts(), 0);

	TestTrue(TEXT("The player answers"),
		Fixture.Playtest->SetPlaytestConsent(EFlockPlaytestConsentChoice::VideoAndPlayData));

	ExpectPlaytestStatus(*this, TEXT("Playtesting is ready"), Fixture.Playtest->GetStatus(), EFlockPlaytestStatus::Ready);
	TestFalse(TEXT("And nothing is waiting to ask them any more"), Fixture.Playtest->IsWaitingToAskForConsentForTesting());
	TestTrue(TEXT("Video recording is on"), Fixture.Playtest->IsPlaytestFeatureEnabled(FlockPlaytestFeatures::VideoRecording));
	TestTrue(TEXT("Heavy analytics is on"), Fixture.Playtest->IsPlaytestFeatureEnabled(FlockPlaytestFeatures::HeavyAnalytics));
	TestEqual(TEXT("And the launch's Protokite session starts, without waiting for another Flock session"),
		Fixture.SessionStarts(), 1);

	// The answer is on this machine for every later launch, not only in this one's memory.
	const FFlockPlaytestConsentFile Saved(Fixture.ConsentFilePath);
	TestEqual(TEXT("The answer is kept for the next launch"), static_cast<int32>(Saved.Read()),
		static_cast<int32>(EFlockPlaytestConsentChoice::VideoAndPlayData));
	return true;
}

/** Collecting nothing is the same build behaviour as Enable Playtesting being off, which is what the player is told. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestConsentNothingTest,
	"Flock.Playtest.Consent.NothingReadsLikePlaytestingTurnedOff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestConsentNothingTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings Analytics(/*bAnalyticsEnabled*/ true);
	FPlaytestFixture Fixture(/*bTurnRetriesOff*/ true, /*bUseTestIdentitySources*/ true, EFlockPlaytestConsentChoice::Nothing);
	AnswerConfigWithEveryFeature(Fixture);
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(ConsentTestFlockSessionId);

	ExpectPlaytestStatus(*this, TEXT("The player refused"), Fixture.Playtest->GetStatus(),
		EFlockPlaytestStatus::PlayerRefusedPlaytest);
	TestFalse(TEXT("Video recording is off"), Fixture.Playtest->IsPlaytestFeatureEnabled(FlockPlaytestFeatures::VideoRecording));
	TestFalse(TEXT("Heavy analytics is off"), Fixture.Playtest->IsPlaytestFeatureEnabled(FlockPlaytestFeatures::HeavyAnalytics));
	TestFalse(TEXT("Nothing is being measured"), Fixture.Playtest->IsMeasuringPerformance());
	TestEqual(TEXT("No Protokite session is started"), Fixture.SessionStarts(), 0);
	TestFalse(TEXT("And there is no feedback form to open, since nothing of this launch is collected"),
		Fixture.Playtest->CanOpenFeedbackForm());
	return true;
}

/** Each half of the answer silences the other, everywhere at once. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestConsentVideoOnlyTest,
	"Flock.Playtest.Consent.VideoOnlyLeavesPlayDataOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestConsentVideoOnlyTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings Analytics(/*bAnalyticsEnabled*/ true);
	FPlaytestFixture Fixture(/*bTurnRetriesOff*/ true, /*bUseTestIdentitySources*/ true, EFlockPlaytestConsentChoice::VideoOnly);
	AnswerConfigWithEveryFeature(Fixture);
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(ConsentTestFlockSessionId);

	ExpectPlaytestStatus(*this, TEXT("Playtesting is ready"), Fixture.Playtest->GetStatus(), EFlockPlaytestStatus::Ready);
	TestTrue(TEXT("The playtest may record the screen"),
		Fixture.Playtest->IsPlaytestFeatureEnabled(FlockPlaytestFeatures::VideoRecording));
	TestFalse(TEXT("And collects no play data, although the playtest asks for it"),
		Fixture.Playtest->IsPlaytestFeatureEnabled(FlockPlaytestFeatures::HeavyAnalytics));
	TestFalse(TEXT("So nothing is measured"), Fixture.Playtest->IsMeasuringPerformance());
	TestFalse(TEXT("And the game's own playtest events are refused"),
		Fixture.Playtest->RecordPlaytestEvent(TEXT("not_this_time")));
	TestEqual(TEXT("The launch still gets its session, which is where the recording goes"), Fixture.SessionStarts(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestConsentPlayDataOnlyTest,
	"Flock.Playtest.Consent.PlayDataOnlyLeavesTheScreenOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestConsentPlayDataOnlyTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings Analytics(/*bAnalyticsEnabled*/ true);
	FPlaytestFixture Fixture(/*bTurnRetriesOff*/ true, /*bUseTestIdentitySources*/ true, EFlockPlaytestConsentChoice::PlayDataOnly);
	AnswerConfigWithEveryFeature(Fixture);
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(ConsentTestFlockSessionId);

	ExpectPlaytestStatus(*this, TEXT("Playtesting is ready"), Fixture.Playtest->GetStatus(), EFlockPlaytestStatus::Ready);
	TestFalse(TEXT("The screen is not recorded, although the playtest asks for it"),
		Fixture.Playtest->IsPlaytestFeatureEnabled(FlockPlaytestFeatures::VideoRecording));
	TestFalse(TEXT("And nothing is capturing"), Fixture.Playtest->IsRecordingVideo());
	TestTrue(TEXT("Play data is collected"), Fixture.Playtest->IsPlaytestFeatureEnabled(FlockPlaytestFeatures::HeavyAnalytics));
	TestTrue(TEXT("So performance is being measured"), Fixture.Playtest->IsMeasuringPerformance());
	return true;
}

/**
 * The session says what it was allowed to collect. Protokite otherwise shows a session with no recording as a build
 * that went wrong, when it is a player who asked for none.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestConsentSessionFieldsTest,
	"Flock.Playtest.Consent.TheSessionSaysWhatItMayCollect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestConsentSessionFieldsTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture(/*bTurnRetriesOff*/ true, /*bUseTestIdentitySources*/ true, EFlockPlaytestConsentChoice::VideoOnly);
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(ConsentTestFlockSessionId);
	if (!TestEqual(TEXT("Precondition: the session started"), Fixture.SessionStarts(), 1))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Debug = LastSessionExtraDebug(Fixture);
	if (TestTrue(TEXT("The start carries extra_debug"), Debug.IsValid()))
	{
		// Letter for letter: Unreal's ordinary string checks ignore case, and these are values read off a dashboard.
		TestEqualSensitive(TEXT("It names the answer"), *StringMember(Debug, TEXT("playtest_consent")), TEXT("video_only"));
		TestEqualSensitive(TEXT("And says the player was asked"), *StringMember(Debug, TEXT("playtest_consent_asked")), TEXT("true"));
		// The facts that were already there are still there: these ride along with them, they do not replace them.
		TestFalse(TEXT("Beside the rest of the debug facts"), StringMember(Debug, TEXT("sdk_version")) == TEXT("<absent>"));
	}
	return true;
}

/**
 * A build that asks nobody collects everything, and its sessions say so. Without that field, a build that never asked
 * and a player who allowed everything look identical on the dashboard.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestConsentNotAskedTest,
	"Flock.Playtest.Consent.ABuildThatDoesNotAskCollectsEverythingAndSaysSo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestConsentNotAskedTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl, /*bAskThePlayerForPlaytestConsent*/ false);
	FPlaytestFixture Fixture(/*bTurnRetriesOff*/ true, /*bUseTestIdentitySources*/ true, EFlockPlaytestConsentChoice::NotAnswered);
	AnswerConfigWithEveryFeature(Fixture);
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(ConsentTestFlockSessionId);

	ExpectPlaytestStatus(*this, TEXT("It does not wait for an answer"), Fixture.Playtest->GetStatus(),
		EFlockPlaytestStatus::Ready);
	TestFalse(TEXT("Nobody answered"), FlockPlaytestConsent::IsAnswered(Fixture.Playtest->GetPlayersConsentAnswer()));
	TestTrue(TEXT("And the playtest collects what it asks for"),
		Fixture.Playtest->IsPlaytestFeatureEnabled(FlockPlaytestFeatures::VideoRecording));

	const TSharedPtr<FJsonObject> Debug = LastSessionExtraDebug(Fixture);
	if (TestTrue(TEXT("The session started"), Debug.IsValid()))
	{
		TestEqualSensitive(TEXT("It says everything was collected"), *StringMember(Debug, TEXT("playtest_consent")),
			TEXT("video_and_play_data"));
		TestEqualSensitive(TEXT("And that the player was never asked"), *StringMember(Debug, TEXT("playtest_consent_asked")),
			TEXT("false"));
	}
	return true;
}

/**
 * An answer a player gave holds in a build that has since stopped asking. The setting decides whether the question is
 * put, never whether an answer already given counts.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestConsentOutlivesTheSettingTest,
	"Flock.Playtest.Consent.AnAnswerOutlivesABuildThatStopsAsking",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestConsentOutlivesTheSettingTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl, /*bAskThePlayerForPlaytestConsent*/ false);
	FPlaytestFixture Fixture(/*bTurnRetriesOff*/ true, /*bUseTestIdentitySources*/ true, EFlockPlaytestConsentChoice::PlayDataOnly);
	AnswerConfigWithEveryFeature(Fixture);
	Fixture.StartFlock();

	TestEqual(TEXT("Their answer is what the build collects under"), static_cast<int32>(Fixture.Playtest->GetPlaytestConsent()),
		static_cast<int32>(EFlockPlaytestConsentChoice::PlayDataOnly));
	TestFalse(TEXT("So the screen is still not recorded"),
		Fixture.Playtest->IsPlaytestFeatureEnabled(FlockPlaytestFeatures::VideoRecording));
	return true;
}

/** Forgetting the answer puts the question back, which is what a game's "ask me again" offers. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestConsentForgettingTest,
	"Flock.Playtest.Consent.ForgettingTheAnswerAsksAgain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestConsentForgettingTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture(/*bTurnRetriesOff*/ true, /*bUseTestIdentitySources*/ true, EFlockPlaytestConsentChoice::VideoAndPlayData);
	AnswerConfigWithEveryFeature(Fixture);
	Fixture.StartFlock();
	ExpectPlaytestStatus(*this, TEXT("Precondition: it is ready"), Fixture.Playtest->GetStatus(), EFlockPlaytestStatus::Ready);

	TestTrue(TEXT("The answer is forgotten"), Fixture.Playtest->SetPlaytestConsent(EFlockPlaytestConsentChoice::NotAnswered));

	ExpectPlaytestStatus(*this, TEXT("The question is waiting again"), Fixture.Playtest->GetStatus(),
		EFlockPlaytestStatus::WaitingForPlayerConsent);
	TestFalse(TEXT("And nothing is collected meanwhile"),
		Fixture.Playtest->IsPlaytestFeatureEnabled(FlockPlaytestFeatures::HeavyAnalytics));
	TestFalse(TEXT("The kept answer is gone, so the next launch asks too"),
		IFileManager::Get().FileExists(*Fixture.ConsentFilePath));
	return true;
}

/**
 * Nothing an earlier launch left is sent once the player has asked for nothing to be collected -- and it *is* sent
 * otherwise, which is the counter-case: a build that had quietly stopped pushing them would pass the first half alone.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestConsentEarlierRecordingsTest,
	"Flock.Playtest.Consent.NothingStopsWhatEarlierLaunchesLeft",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestConsentEarlierRecordingsTest::RunTest(const FString& Parameters)
{
	// Both halves of one test, so the difference between them is the answer and nothing else.
	for (const EFlockPlaytestConsentChoice Answer : { EFlockPlaytestConsentChoice::VideoAndPlayData,
		EFlockPlaytestConsentChoice::Nothing })
	{
		const bool bShouldBeSent = Answer != EFlockPlaytestConsentChoice::Nothing;
		const FString Which = FlockPlaytestConsent::ToWire(Answer);

		FScopedPlaytestSettings Settings(true, UsableUrl);
		FPlaytestFixture Fixture(/*bTurnRetriesOff*/ true, /*bUseTestIdentitySources*/ true, Answer);

		const FString Recordings = FPaths::Combine(Fixture.Folder, TEXT("Recordings"));
		if (!PlantARecordingAnEarlierLaunchLeft(*this, Recordings))
		{
			return false;
		}

		Fixture.StartFlock();
		// The push waits on a ticker for Flock and for the launch pass, both of which are done by now.
		RunPendingPlaytestRetries();

		TestEqual(*(Which + TEXT(": links asked for")),
			Fixture.Transport->CountRequestsEndingWith(UploadLinkRouteForTheKeptRecording()), bShouldBeSent ? 1 : 0);

		// Either way the file is left on disk rather than deleted: the recordings disk budget is what clears it.
		const TArray<FFlockPlaytestRecordingWaitingToUpload> Waiting =
			FFlockPlaytestRecordingsFolder::FindRecordingsWaitingToUpload(Recordings);
		TestEqual(*(Which + TEXT(": the recording is still on disk")), Waiting.Num(), 1);
	}
	return true;
}

/**
 * **Nothing of an earlier launch's goes out while this launch's question is still on screen.** The answer is seconds
 * away, and a player who then asks for nothing would otherwise have had their last session's video uploaded while they
 * were reading the question. Answering releases it, in the same launch, whichever way they answer.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestConsentEarlierRecordingsWaitTest,
	"Flock.Playtest.Consent.WhatEarlierLaunchesLeftWaitsForTheAnswer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestConsentEarlierRecordingsWaitTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture(/*bTurnRetriesOff*/ true, /*bUseTestIdentitySources*/ true, EFlockPlaytestConsentChoice::NotAnswered);

	const FString Recordings = FPaths::Combine(Fixture.Folder, TEXT("Recordings"));
	if (!PlantARecordingAnEarlierLaunchLeft(*this, Recordings))
	{
		return false;
	}

	Fixture.StartFlock();
	RunPendingPlaytestRetries();
	ExpectPlaytestStatus(*this, TEXT("Precondition: the player is being asked"), Fixture.Playtest->GetStatus(),
		EFlockPlaytestStatus::WaitingForPlayerConsent);
	TestEqual(TEXT("Nothing is sent while they are reading the question"),
		Fixture.Transport->CountRequestsEndingWith(UploadLinkRouteForTheKeptRecording()), 0);

	// They allow it, and what was waiting goes in this launch rather than a later one.
	TestTrue(TEXT("The player answers"), Fixture.Playtest->SetPlaytestConsent(EFlockPlaytestConsentChoice::VideoAndPlayData));
	RunPendingPlaytestRetries();
	TestEqual(TEXT("Then it is sent"), Fixture.Transport->CountRequestsEndingWith(UploadLinkRouteForTheKeptRecording()), 1);
	return true;
}

/**
 * A player who refuses and then changes their mind has what was waiting sent in the same launch. Giving up for the
 * launch would leave it on disk with the recordings disk budget free to evict it before the next one.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestConsentEarlierRecordingsChangeOfMindTest,
	"Flock.Playtest.Consent.ChangingTheAnswerSendsWhatWasWaiting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestConsentEarlierRecordingsChangeOfMindTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture(/*bTurnRetriesOff*/ true, /*bUseTestIdentitySources*/ true, EFlockPlaytestConsentChoice::Nothing);

	const FString Recordings = FPaths::Combine(Fixture.Folder, TEXT("Recordings"));
	if (!PlantARecordingAnEarlierLaunchLeft(*this, Recordings))
	{
		return false;
	}

	Fixture.StartFlock();
	RunPendingPlaytestRetries();
	TestEqual(TEXT("Precondition: their refusal held it back"),
		Fixture.Transport->CountRequestsEndingWith(UploadLinkRouteForTheKeptRecording()), 0);

	TestTrue(TEXT("They change their mind"), Fixture.Playtest->SetPlaytestConsent(EFlockPlaytestConsentChoice::VideoOnly));
	RunPendingPlaytestRetries();
	TestEqual(TEXT("And it is sent in this launch"),
		Fixture.Transport->CountRequestsEndingWith(UploadLinkRouteForTheKeptRecording()), 1);
	return true;
}

#endif
