// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "FlockPlaytestSelfTest.h"
#include "FlockPlaytestVideoEncoder.h"
#include "FlockPlaytestSubsystem.h"
#include "Http/FlockError.h"
#include "Tests/FlockPlaytestFakeFileUploader.h"
#include "Tests/FlockPlaytestSubsystemTestSupport.h"
#include "Tests/FlockPlaytestTestSupport.h"
#include "Tests/FlockPlaytestVideoTestSupport.h"
#include "UObject/StrongObjectPtr.h"

using namespace FlockPlaytestSubsystemTesting;
using namespace FlockPlaytestFixtures;

namespace
{
	using EOutcome = EFlockPlaytestSelfTestOutcome;

	/** A session the counter-cases hand out when Protokite wrongly accepts one. */
	const TCHAR* const SelfTestWronglyCreatedSessionId = TEXT("01KX0SESSIONTHATSHOULDNOT1");
	const TCHAR* const SelfTestClosedGameVersionId = TEXT("pt-closed-version");
	const TCHAR* const SelfTestFeedbackFormRoute = TEXT("/game/sdk/feedback-form");

	/** Protokite's refusals, copied from the local server (2026-09-18). */
	const TCHAR* const SelfTestSessionNotFoundBody = TEXT("{\"detail\":\"Playtest session not found\"}");
	FString SelfTestComplaintBody(const FString& Sentence)
	{
		return FString::Printf(TEXT("{\"detail\":\"%s\"}"), *Sentence);
	}

	FString SelfTestUnknownSessionRoute(const TCHAR* Tail)
	{
		return FString(TEXT("/game/sdk/playtest-session/")) + FFlockPlaytestSelfTest::SessionIdThatDoesNotExist + Tail;
	}

	FString SelfTestLaunchUploadLinkRoute()
	{
		return FString(TEXT("/game/sdk/playtest-session/")) + PlaytestSessionId + TEXT("/recording-upload");
	}

	FFlockError SelfTestErrorWithStatus(int32 StatusCode, const FString& ServerMessage)
	{
		FFlockError Error;
		Error.Type = EFlockErrorType::Validation;
		Error.StatusCode = StatusCode;
		Error.Message = ServerMessage;
		Error.ServerMessage = ServerMessage;
		return Error;
	}

	/** The step with Name, or null when the run never recorded one. */
	const FFlockPlaytestSelfTestStep* FindSelfTestStep(const TArray<FFlockPlaytestSelfTestStep>& Steps, const TCHAR* Name)
	{
		return Steps.FindByPredicate([Name](const FFlockPlaytestSelfTestStep& Step) { return Step.Name.Equals(Name, ESearchCase::CaseSensitive); });
	}

	/** Checks one step's outcome, and that its detail says DetailMustContain when that is given. */
	void ExpectSelfTestStep(FAutomationTestBase& Test, const TArray<FFlockPlaytestSelfTestStep>& Steps, const TCHAR* Name,
		EOutcome Outcome, const FString& DetailMustContain = FString())
	{
		const FFlockPlaytestSelfTestStep* Step = FindSelfTestStep(Steps, Name);
		if (!Test.TestNotNull(FString::Printf(TEXT("'%s' ran"), Name), Step))
		{
			return;
		}
		Test.TestEqual(FString::Printf(TEXT("'%s' came out as expected (detail: %s)"), Name, *Step->Detail),
			static_cast<int32>(Step->Outcome), static_cast<int32>(Outcome));
		if (!DetailMustContain.IsEmpty())
		{
			Test.TestTrue(FString::Printf(TEXT("'%s' says '%s' (detail: %s)"), Name, *DetailMustContain, *Step->Detail),
				Step->Detail.Contains(DetailMustContain));
		}
	}

	/** Ticks the engine's core ticker, the way frames do, until the run finishes or Seconds of ticks have passed. */
	bool TickUntilTheSelfTestFinishes(const TSharedRef<FFlockPlaytestSelfTest>& Run, float Seconds)
	{
		constexpr float Tick = 0.1f;
		for (float Waited = 0.f; !Run->IsFinished() && Waited < Seconds; Waited += Tick)
		{
			FTSTicker::GetCoreTicker().Tick(Tick);
		}
		return Run->IsFinished();
	}

	FFlockPlaytestSelfTest::FOptions SelfTestOptions(const FPlaytestFixture& Fixture, const FString& ClosedGameVersionId = FString())
	{
		FFlockPlaytestSelfTest::FOptions Options;
		Options.HttpAdapterForTesting = Fixture.Transport;
		Options.ClosedPlaytestGameVersionId = ClosedGameVersionId;
		Options.WaitSeconds = 2.f;
		Options.UploadWaitSeconds = 2.f;
		return Options;
	}

	/** Every question answered the way the server takes it, then the refusals each counter-case expects, in order. */
	void AnswerEveryCounterCase(FPlaytestFixture& Fixture)
	{
		// The playtest's own config and session have been fetched and started by now, so what follows answers only the
		// self-test's own requests.
		Fixture.Transport->AnswerInOrder(PlaytestConfigRoute, {
			FFlockPlaytestFakeTransport::Status(401, InvalidApiKeyBody),
			FFlockPlaytestFakeTransport::Status(422, MissingApiKeyBody),
			FFlockPlaytestFakeTransport::Status(404, NotLinkedBody) });
		Fixture.Transport->AnswerInOrder(PlaytestSessionStartRoute, {
			FFlockPlaytestFakeTransport::Status(422, NoPlayerIdentityBody),
			FFlockPlaytestFakeTransport::Status(400, NoLongerCollectingBody) });
		Fixture.Transport->AnswerInOrder(SelfTestFeedbackFormRoute, {
			FFlockPlaytestFakeTransport::Status(422, SelfTestComplaintBody(TEXT("Missing required answer 'rating'"))),
			FFlockPlaytestFakeTransport::Status(422, SelfTestComplaintBody(TEXT("Invalid option for 'category'"))),
			FFlockPlaytestFakeTransport::Status(404, SelfTestSessionNotFoundBody),
			FFlockPlaytestFakeTransport::Status(200, Envelope(TEXT("{\"id\":\"01KX0FORMRESPONSE000000001\"}"))) });
		Fixture.Transport->Answer(SelfTestUnknownSessionRoute(TEXT("/recording-upload")),
			FFlockPlaytestFakeTransport::Status(404, SelfTestSessionNotFoundBody));
		Fixture.Transport->Answer(SelfTestUnknownSessionRoute(TEXT("/end")),
			FFlockPlaytestFakeTransport::Status(404, SelfTestSessionNotFoundBody));
	}

	/** The requests sent to an address ending with Route, from the FirstIndex-th request on, in order. */
	TArray<FFlockHttpRequest> SelfTestRequestsTo(const FFlockPlaytestFakeTransport& Transport, const FString& Route, int32 FirstIndex)
	{
		TArray<FFlockHttpRequest> Found;
		for (int32 Index = FirstIndex; Index < Transport.Requests.Num(); ++Index)
		{
			if (Transport.Requests[Index].Url.EndsWith(Route))
			{
				Found.Add(Transport.Requests[Index]);
			}
		}
		return Found;
	}

	TSharedPtr<FJsonObject> SelfTestParseBody(const FFlockHttpRequest& Request)
	{
		TSharedPtr<FJsonObject> Body;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Request.JsonBody);
		FJsonSerializer::Deserialize(Reader, Body);
		return Body;
	}

	/** A form request's answers object; null when it sent none. */
	TSharedPtr<FJsonObject> SelfTestSentAnswers(const FFlockHttpRequest& Request)
	{
		const TSharedPtr<FJsonObject> Body = SelfTestParseBody(Request);
		const TSharedPtr<FJsonObject>* Answers = nullptr;
		return Body.IsValid() && Body->TryGetObjectField(TEXT("answers"), Answers) ? *Answers : nullptr;
	}

	/** Sets the Flock SDK's repeat window for one test and puts the previous value back when it ends. */
	struct FScopedSelfTestRepeatWindow
	{
		UFlockConfig* FlockSettings = GetMutableDefault<UFlockConfig>();
		float SavedWindow = FlockSettings->AnalyticsExceptionRepeatWindow;

		explicit FScopedSelfTestRepeatWindow(float Seconds) { FlockSettings->AnalyticsExceptionRepeatWindow = Seconds; }
		~FScopedSelfTestRepeatWindow() { FlockSettings->AnalyticsExceptionRepeatWindow = SavedWindow; }
	};

	/** Sends whatever a test left in the Flock SDK's queues, so the next test that keeps them does not send it. */
	void SendWhatTheSelfTestLeftQueued(FPlaytestFixture& Fixture)
	{
		if (FFlockAnalyticsProvider* Analytics = Fixture.Flock->GetAnalyticsProvider())
		{
			Analytics->Flush();
		}
	}

	/** Records half a second of a playtest video, with an upload link and storage standing by for it. */
	TSharedRef<FFlockPlaytestFakeFileUploader> RecordForTheSelfTest(FPlaytestFixture& Fixture)
	{
		const TSharedRef<FFlockPlaytestFakeFileUploader> Uploader = MakeShared<FFlockPlaytestFakeFileUploader>();
		Uploader->On(TEXT("/put/"));
		Fixture.Playtest->SetFileUploaderForTesting(Uploader);
		Fixture.Transport->Answer(SelfTestLaunchUploadLinkRoute(), FFlockPlaytestFakeTransport::Status(200, Envelope(
			TEXT("{\"upload_url\":\"http://storage.local/put/self-test.webm?signature=abc\",\"bucket\":\"b\",\"key\":\"k\"}"))));
		Fixture.Playtest->SetVideoFrameSourceFactoryForTesting([](FIntPoint MaxVideoSize, FString&) -> TSharedPtr<IFlockPlaytestVideoFrameSource>
		{
			return MakeShared<FlockPlaytestVideoTesting::FTestVideoFrameSource>(
				FitVideoSizeInside(FIntPoint(1280, 720), MaxVideoSize));
		});
		return Uploader;
	}

	void RecordHalfASecond(FPlaytestFixture& Fixture)
	{
		for (int32 Frame = 0; Frame < 30; ++Frame)
		{
			Fixture.Playtest->TickVideoRecordingForTesting(1.f / 60.f);
			Fixture.Playtest->WaitUntilVideoWrittenForTesting();
		}
	}
}

/** A counter-case passes on its own refusal and nothing else: a success is the very thing it exists to catch. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSelfTestJudgeAcceptedIsNotARefusalTest,
	"Flock.Playtest.SelfTest.Judge.AnAcceptedRequestIsNotTheRefusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSelfTestJudgeAcceptedIsNotARefusalTest::RunTest(const FString& Parameters)
{
	FString WhatHappened;
	TestFalse(TEXT("A success fails the step"),
		FFlockPlaytestSelfTest::IsTheExpectedRefusal(true, FFlockError(), 404, FString(), WhatHappened));
	TestTrue(TEXT("Saying it was accepted"), WhatHappened.Contains(TEXT("accepted")));

	TestTrue(TEXT("The refusal it expects passes"), FFlockPlaytestSelfTest::IsTheExpectedRefusal(false,
		SelfTestErrorWithStatus(404, TEXT("Playtest session not found")), 404, FString(), WhatHappened));
	TestTrue(TEXT("Saying what the server said"), WhatHappened.Contains(TEXT("Playtest session not found")));
	return true;
}

/** Protokite's refusals carry no code, so the status is what tells one from another: any other status fails. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSelfTestJudgeOtherStatusTest,
	"Flock.Playtest.SelfTest.Judge.AnotherStatusIsNotTheRefusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSelfTestJudgeOtherStatusTest::RunTest(const FString& Parameters)
{
	FString WhatHappened;
	TestFalse(TEXT("A 500 is not the 404 expected"), FFlockPlaytestSelfTest::IsTheExpectedRefusal(false,
		SelfTestErrorWithStatus(500, TEXT("Internal Server Error")), 404, FString(), WhatHappened));
	TestTrue(TEXT("Naming both statuses"), WhatHappened.Contains(TEXT("500")) && WhatHappened.Contains(TEXT("404")));

	TestFalse(TEXT("A 422 is not the 401 a wrong key gets"), FFlockPlaytestSelfTest::IsTheExpectedRefusal(false,
		SelfTestErrorWithStatus(422, TEXT("Field required")), 401, FString(), WhatHappened));

	FFlockError NeverReached;
	NeverReached.Type = EFlockErrorType::Connection;
	NeverReached.Message = TEXT("Connection refused");
	TestFalse(TEXT("A request that never reached Protokite is no refusal at all"),
		FFlockPlaytestSelfTest::IsTheExpectedRefusal(false, NeverReached, 404, FString(), WhatHappened));
	TestTrue(TEXT("And says so"), WhatHappened.Contains(TEXT("never reached")));
	return true;
}

/**
 * A form refusal passes only when it names the question the counter-case broke. Every form complaint is a 422, so a
 * refusal for another question -- the form changed, or the answers were built wrong -- would pass on status alone.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSelfTestJudgeNamedQuestionTest,
	"Flock.Playtest.SelfTest.Judge.ARefusalMustNameTheQuestion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSelfTestJudgeNamedQuestionTest::RunTest(const FString& Parameters)
{
	FString WhatHappened;
	TestTrue(TEXT("Naming the question passes"), FFlockPlaytestSelfTest::IsTheExpectedRefusal(false,
		SelfTestErrorWithStatus(422, TEXT("Missing required answer 'rating'")), 422, TEXT("rating"), WhatHappened));

	TestFalse(TEXT("Naming another question fails"), FFlockPlaytestSelfTest::IsTheExpectedRefusal(false,
		SelfTestErrorWithStatus(422, TEXT("Missing required answer 'category'")), 422, TEXT("rating"), WhatHappened));
	TestTrue(TEXT("Saying which it named"), WhatHappened.Contains(TEXT("'category'")));

	TestFalse(TEXT("The same question in other letters fails: the server names its questions letter for letter"),
		FFlockPlaytestSelfTest::IsTheExpectedRefusal(false, SelfTestErrorWithStatus(422, TEXT("Missing required answer 'Rating'")),
			422, TEXT("rating"), WhatHappened));

	TestFalse(TEXT("Naming no question fails"), FFlockPlaytestSelfTest::IsTheExpectedRefusal(false,
		SelfTestErrorWithStatus(422, TEXT("Field required")), 422, TEXT("rating"), WhatHappened));
	return true;
}

/** The option the counter-case sends is never one the question offers, compared letter for letter as the server does. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSelfTestOptionNotOnTheListTest,
	"Flock.Playtest.SelfTest.MakesAnOptionThatIsNotOnTheList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSelfTestOptionNotOnTheListTest::RunTest(const FString& Parameters)
{
	const TArray<TArray<FString>> Lists = {
		{ TEXT("Bug"), TEXT("Crash") },
		{ TEXT("BUG"), TEXT("bug") },
		{ TEXT("42") },
		{ TEXT("bug"), TEXT("BUG"), TEXT("Bug") },
	};
	for (const TArray<FString>& Options : Lists)
	{
		const FString Made = FFlockPlaytestSelfTest::MakeOptionNotOnTheList(Options);
		const bool bOnTheList = Options.ContainsByPredicate([&Made](const FString& Option) { return Option.Equals(Made, ESearchCase::CaseSensitive); });
		TestFalse(FString::Printf(TEXT("'%s' is not one of [%s]"), *Made, *FString::Join(Options, TEXT(", "))), bOnTheList);
	}
	TestEqualSensitive(TEXT("Where it can, it is the first option in other letters"),
		FFlockPlaytestSelfTest::MakeOptionNotOnTheList({ TEXT("Bug"), TEXT("Crash") }), TEXT("BUG"));
	return true;
}

/**
 * The whole run against a playtest that turns everything on: every step passes, each counter-case on its own refusal,
 * and the launch's session is ended exactly once. The refusals are the fake's, so what this proves is that each step
 * sends the request it names and judges the answer; the live run proves Protokite answers them that way.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSelfTestFullRunTest,
	"Flock.Playtest.SelfTest.EveryStepPassesAgainstAPlaytestThatTurnsEverythingOn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSelfTestFullRunTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings FlockAnalytics(true, /*bCaptureExceptions*/ true, /*bKeepEventsUntilSent*/ true);
	FPlaytestFixture Fixture;
	const TSharedRef<FFlockPlaytestFakeFileUploader> Uploader = RecordForTheSelfTest(Fixture);
	Fixture.AnswerConfig(FFlockPlaytestFakeTransport::Status(200,
		ConfigBody(GameVersionId, /*bHeavyAnalytics*/ true, /*bVideoRecording*/ true, /*bExceptionCapturing*/ true)));
	Fixture.SignInToFlockOnStart();
	Fixture.StartFlock();
	RecordHalfASecond(Fixture);
	// Every platform but 64-bit Windows builds with no video encoder: there the recording step is skipped, saying why.
	const bool bBuiltWithVideo = FFlockPlaytestVideoEncoder::IsBuiltWithVideo();
	if (!TestEqual(TEXT("Precondition: the launch's session started"), static_cast<int32>(Fixture.Playtest->GetPlaytestSessionState()), static_cast<int32>(EFlockPlaytestSessionState::Started))
		|| !TestEqual(TEXT("Precondition: it is recording where this build can"), Fixture.Playtest->IsRecordingVideo(), bBuiltWithVideo))
	{
		return false;
	}
	AnswerEveryCounterCase(Fixture);
	const int32 RequestsBefore = Fixture.Transport->Requests.Num();

	const TSharedRef<FFlockPlaytestSelfTest> Run = FFlockPlaytestSelfTest::Start(Fixture.Playtest, Fixture.Flock,
		SelfTestOptions(Fixture, SelfTestClosedGameVersionId));
	TestTrue(TEXT("The run finishes"), TickUntilTheSelfTestFinishes(Run, 10.f));

	using namespace FlockPlaytestSelfTestSteps;
	const TArray<FFlockPlaytestSelfTestStep>& Steps = Run->GetSteps();
	TestEqual(TEXT("It ran seventeen steps"), Steps.Num(), 17);
	for (const FFlockPlaytestSelfTestStep& Step : Steps)
	{
		if (!bBuiltWithVideo && Step.Name.Equals(RecordingUploaded, ESearchCase::CaseSensitive))
		{
			TestEqual(FString::Printf(TEXT("'%s' is skipped where this build has no video (detail: %s)"), *Step.Name, *Step.Detail),
				static_cast<int32>(Step.Outcome), static_cast<int32>(EOutcome::Skipped));
			TestTrue(TEXT("Saying why"), Step.Detail.Contains(TEXT("no video encoder"), ESearchCase::CaseSensitive));
			continue;
		}
		TestEqual(FString::Printf(TEXT("'%s' passed (detail: %s)"), *Step.Name, *Step.Detail),
			static_cast<int32>(Step.Outcome), static_cast<int32>(EOutcome::Passed));
	}

	// What the counter-cases sent, beyond judging the answers: the fake answers by address alone, so a step that sent the
	// real key where it meant a wrong one would pass on the answer.
	const TArray<FFlockHttpRequest> ConfigProbes = SelfTestRequestsTo(*Fixture.Transport, PlaytestConfigRoute, RequestsBefore);
	if (TestEqual(TEXT("Three config requests were sent"), ConfigProbes.Num(), 3))
	{
		TestEqualSensitive(TEXT("The first with a wrong key"), ConfigProbes[0].Headers.FindRef(TEXT("X-Flock-API-Key")),
			FFlockPlaytestSelfTest::WrongApiKey);
		TestFalse(TEXT("The second with no key"), ConfigProbes[1].Headers.Contains(TEXT("X-Flock-API-Key")));
		TestEqualSensitive(TEXT("The second with this build's version"), ConfigProbes[1].Headers.FindRef(TEXT("X-Game-Version-ID")), GameVersionId);
		TestEqualSensitive(TEXT("The third with the real key"), ConfigProbes[2].Headers.FindRef(TEXT("X-Flock-API-Key")), TEXT("secret"));
		TestEqualSensitive(TEXT("And a version no playtest is linked to"), ConfigProbes[2].Headers.FindRef(TEXT("X-Game-Version-ID")),
			FFlockPlaytestSelfTest::GameVersionIdWithNoPlaytest);
	}
	const TArray<FFlockHttpRequest> StartProbes = SelfTestRequestsTo(*Fixture.Transport, PlaytestSessionStartRoute, RequestsBefore);
	if (TestEqual(TEXT("Two session starts were provoked, neither of them the launch's"), StartProbes.Num(), 2))
	{
		const TSharedPtr<FJsonObject> NoPlayer = SelfTestParseBody(StartProbes[0]);
		TestEqual(TEXT("The first names no device"), StringMember(NoPlayer, TEXT("device_id")), FString(TEXT("<absent>")));
		TestEqual(TEXT("And no Steam account"), StringMember(NoPlayer, TEXT("steam_id")), FString(TEXT("<absent>")));
		TestNotEqual(TEXT("The second names the player, so only the closed playtest can be refused"),
			StringMember(SelfTestParseBody(StartProbes[1]), TEXT("device_id")), FString(TEXT("<absent>")));
		TestEqualSensitive(TEXT("Under the closed playtest's version"), StartProbes[1].Headers.FindRef(TEXT("X-Game-Version-ID")),
			SelfTestClosedGameVersionId);
	}
	const TArray<FFlockHttpRequest> Forms = SelfTestRequestsTo(*Fixture.Transport, SelfTestFeedbackFormRoute, RequestsBefore);
	if (TestEqual(TEXT("Four forms were sent"), Forms.Num(), 4))
	{
		const TSharedPtr<FJsonObject> Missing = SelfTestSentAnswers(Forms[0]);
		TestTrue(TEXT("The first leaves out the needed rating"), Missing.IsValid() && !Missing->HasField(TEXT("rating")));
		TestTrue(TEXT("And answers the rest"), HasMemberSpelled(Missing, TEXT("category")));
		// Letter for letter: the engine's TestEqual ignores case for strings, and "BUG" against "Bug" is the whole point.
		TestEqualSensitive(TEXT("The second chooses an option not on the list"), StringMember(SelfTestSentAnswers(Forms[1]), TEXT("category")),
			TEXT("BUG"));
		TestEqualSensitive(TEXT("The third names a session that does not exist"), StringMember(SelfTestParseBody(Forms[2]), TEXT("session_id")),
			FFlockPlaytestSelfTest::SessionIdThatDoesNotExist);
		TestEqualSensitive(TEXT("The fourth names the launch's session"), StringMember(SelfTestParseBody(Forms[3]), TEXT("session_id")),
			PlaytestSessionId);
		TestEqualSensitive(TEXT("With the answer the server takes"), StringMember(SelfTestSentAnswers(Forms[3]), TEXT("category")), TEXT("Bug"));
	}
	TestEqual(TEXT("The recording was uploaded once, where this build records"), Uploader->Uploads.Num(), bBuiltWithVideo ? 1 : 0);
	TestEqual(TEXT("The launch's session was ended once"),
		Fixture.Transport->CountRequestsEndingWith(FString::Printf(TEXT("/%s/end"), PlaytestSessionId)), 1);
	TestEqual(TEXT("And the playtest shows it ended"), static_cast<int32>(Fixture.Playtest->GetPlaytestSessionState()), static_cast<int32>(EFlockPlaytestSessionState::Ended));

	// The run is over, so teardown must not end the session a second time.
	Fixture.Playtest->Deinitialize();
	TestEqual(TEXT("Teardown sent no second end"),
		Fixture.Transport->CountRequestsEndingWith(FString::Printf(TEXT("/%s/end"), PlaytestSessionId)), 1);
	return true;
}

/**
 * The counter-case for every refusal step: a request Protokite should have refused and accepted fails its step, and a
 * session a counter-case was given is ended at once, so a run leaves nothing open.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSelfTestAcceptedRefusalsFailTest,
	"Flock.Playtest.SelfTest.AnAcceptedCounterCaseFailsAndItsSessionIsEnded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSelfTestAcceptedRefusalsFailTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings FlockAnalytics(true);
	FPlaytestFixture Fixture;
	Fixture.SignInToFlockOnStart();
	Fixture.StartFlock();
	if (!TestEqual(TEXT("Precondition: the launch's session started"), static_cast<int32>(Fixture.Playtest->GetPlaytestSessionState()), static_cast<int32>(EFlockPlaytestSessionState::Started)))
	{
		return false;
	}

	// A server that takes everything.
	Fixture.AnswerConfig(FFlockPlaytestFakeTransport::Status(200, ConfigBody()));
	Fixture.Transport->Answer(PlaytestSessionStartRoute,
		FFlockPlaytestFakeTransport::Status(200, SessionStartBody(SelfTestWronglyCreatedSessionId)));
	Fixture.Transport->Answer(SelfTestFeedbackFormRoute,
		FFlockPlaytestFakeTransport::Status(200, Envelope(TEXT("{\"id\":\"01KX0FORMRESPONSE000000001\"}"))));
	Fixture.Transport->Answer(SelfTestUnknownSessionRoute(TEXT("/recording-upload")), FFlockPlaytestFakeTransport::Status(200, Envelope(
		TEXT("{\"upload_url\":\"http://storage.local/put/nowhere.webm\",\"bucket\":\"b\",\"key\":\"k\"}"))));

	const TSharedRef<FFlockPlaytestSelfTest> Run = FFlockPlaytestSelfTest::Start(Fixture.Playtest, Fixture.Flock,
		SelfTestOptions(Fixture, SelfTestClosedGameVersionId));
	TestTrue(TEXT("The run finishes"), TickUntilTheSelfTestFinishes(Run, 10.f));

	using namespace FlockPlaytestSelfTestSteps;
	const TArray<FFlockPlaytestSelfTestStep>& Steps = Run->GetSteps();
	for (const TCHAR* Refusal : { WrongApiKeyRefused, MissingApiKeyRefused, VersionWithNoPlaytestRefused, SessionWithNoPlayerRefused,
		ClosedPlaytestRefused, FormMissingANeededAnswerRefused, FormWithAnOptionNotOnTheListRefused,
		FormForASessionThatDoesNotExistRefused, UploadLinkForASessionThatDoesNotExistRefused, EndForASessionThatDoesNotExistRefused })
	{
		ExpectSelfTestStep(*this, Steps, Refusal, EOutcome::Failed, TEXT("accepted"));
	}

	// Two starts were wrongly accepted, the no-player one and the closed-playtest one, and each was ended at once.
	TestEqual(TEXT("Both sessions it should never have been given were ended"),
		Fixture.Transport->CountRequestsEndingWith(FString::Printf(TEXT("/%s/end"), SelfTestWronglyCreatedSessionId)), 2);
	TestEqual(TEXT("The launch's own session still ended once"),
		Fixture.Transport->CountRequestsEndingWith(FString::Printf(TEXT("/%s/end"), PlaytestSessionId)), 1);
	return true;
}

/**
 * A run where nobody has said what the playtest may collect is SKIPPED, not FAILED: this build's playtest loaded, which
 * is what the step checks, and a harness run has nobody at the keyboard to answer a question drawn over the game.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSelfTestSkipsWithoutConsentTest,
	"Flock.Playtest.SelfTest.SkipsWhenNobodyHasSaidWhatItMayCollect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSelfTestSkipsWithoutConsentTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings FlockAnalytics(true);
	FPlaytestFixture Fixture(/*bTurnRetriesOff*/ true, /*bUseTestIdentitySources*/ true,
		EFlockPlaytestConsentChoice::NotAnswered);
	Fixture.SignInToFlockOnStart();
	Fixture.StartFlock();
	AnswerEveryCounterCase(Fixture);

	const TSharedRef<FFlockPlaytestSelfTest> Run = FFlockPlaytestSelfTest::Start(Fixture.Playtest, Fixture.Flock, SelfTestOptions(Fixture));
	TestTrue(TEXT("The run finishes"), TickUntilTheSelfTestFinishes(Run, 10.f));

	using namespace FlockPlaytestSelfTestSteps;
	const TArray<FFlockPlaytestSelfTestStep>& Steps = Run->GetSteps();
	// Not a failure, and it names the command a run with nobody there answers with.
	ExpectSelfTestStep(*this, Steps, PlaytestLoaded, EOutcome::Skipped, TEXT("FlockPlaytest.AnswerConsent"));
	ExpectSelfTestStep(*this, Steps, SessionStarted, EOutcome::Skipped);
	TestEqual(TEXT("And nothing of this launch's was sent"), Fixture.SessionStarts(), 0);

	// The counter-case, in the same test: with an answer, the same run finds the playtest.
	FPlaytestFixture Answered(/*bTurnRetriesOff*/ true, /*bUseTestIdentitySources*/ true,
		EFlockPlaytestConsentChoice::VideoAndPlayData);
	Answered.SignInToFlockOnStart();
	Answered.StartFlock();
	AnswerEveryCounterCase(Answered);
	const TSharedRef<FFlockPlaytestSelfTest> AnsweredRun =
		FFlockPlaytestSelfTest::Start(Answered.Playtest, Answered.Flock, SelfTestOptions(Answered));
	TestTrue(TEXT("That run finishes too"), TickUntilTheSelfTestFinishes(AnsweredRun, 10.f));
	ExpectSelfTestStep(*this, AnsweredRun->GetSteps(), PlaytestLoaded, EOutcome::Passed);
	return true;
}

/** A step whose feature the playtest does not turn on is skipped, saying so, and sends nothing. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSelfTestSkipsWhatIsOffTest,
	"Flock.Playtest.SelfTest.SkipsWhatThePlaytestDoesNotTurnOn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSelfTestSkipsWhatIsOffTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings FlockAnalytics(true);
	FPlaytestFixture Fixture;
	Fixture.AnswerConfig(FFlockPlaytestFakeTransport::Status(200, Envelope(FString::Printf(
		TEXT("{\"session_started_event\":\"session_started\",\"test_id\":\"%s\",\"flock_game_version_id\":\"%s\",")
		TEXT("\"features\":{\"video_recording\":false,\"exception_capturing\":false,\"heavy_analytics\":false},\"form\":null}"),
		TestId, GameVersionId))));
	Fixture.SignInToFlockOnStart();
	Fixture.StartFlock();
	AnswerEveryCounterCase(Fixture);

	const TSharedRef<FFlockPlaytestSelfTest> Run = FFlockPlaytestSelfTest::Start(Fixture.Playtest, Fixture.Flock, SelfTestOptions(Fixture));
	TestTrue(TEXT("The run finishes"), TickUntilTheSelfTestFinishes(Run, 10.f));

	using namespace FlockPlaytestSelfTestSteps;
	const TArray<FFlockPlaytestSelfTestStep>& Steps = Run->GetSteps();
	ExpectSelfTestStep(*this, Steps, ClosedPlaytestRefused, EOutcome::Skipped, TEXT("FlockPlaytest.SelfTest <Game Version ID>"));
	ExpectSelfTestStep(*this, Steps, ExceptionReported, EOutcome::Skipped, TEXT("does not turn exception capturing on"));
	ExpectSelfTestStep(*this, Steps, PlaytestEventRecorded, EOutcome::Skipped, TEXT("does not turn heavy analytics on"));
	for (const TCHAR* FormStep : { FormMissingANeededAnswerRefused, FormWithAnOptionNotOnTheListRefused,
		FormForASessionThatDoesNotExistRefused, FormTaken })
	{
		ExpectSelfTestStep(*this, Steps, FormStep, EOutcome::Skipped, TEXT("publishes no feedback form"));
	}
	ExpectSelfTestStep(*this, Steps, RecordingUploaded, EOutcome::Skipped, TEXT("does not record video"));
	ExpectSelfTestStep(*this, Steps, SessionEnded, EOutcome::Passed);

	TestEqual(TEXT("No form was sent"), Fixture.Transport->CountRequestsEndingWith(SelfTestFeedbackFormRoute), 0);
	TestEqual(TEXT("Nor a playtest event"), Fixture.SentPlaytestEvents().Num(), 0);
	return true;
}

/**
 * With nobody signed in, no session starts: the session step fails naming the sign-in it waits for, every step that
 * needs a session is skipped, and nothing is ended.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSelfTestNeedsASignInTest,
	"Flock.Playtest.SelfTest.WithNobodySignedInTheSessionStepFailsAndWhatNeedsItIsSkipped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSelfTestNeedsASignInTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings FlockAnalytics(true);
	FPlaytestFixture Fixture;
	Fixture.StartFlock();
	AnswerEveryCounterCase(Fixture);

	const TSharedRef<FFlockPlaytestSelfTest> Run = FFlockPlaytestSelfTest::Start(Fixture.Playtest, Fixture.Flock, SelfTestOptions(Fixture));
	TestTrue(TEXT("The run finishes once the wait is over"), TickUntilTheSelfTestFinishes(Run, 10.f));

	using namespace FlockPlaytestSelfTestSteps;
	const TArray<FFlockPlaytestSelfTestStep>& Steps = Run->GetSteps();
	ExpectSelfTestStep(*this, Steps, SessionStarted, EOutcome::Failed, TEXT("Flock.LoginWithDevice"));
	for (const TCHAR* NeedsASession : { ClosedPlaytestRefused, PlaytestEventRecorded, FormMissingANeededAnswerRefused,
		FormWithAnOptionNotOnTheListRefused, FormForASessionThatDoesNotExistRefused, FormTaken, RecordingUploaded, SessionEnded })
	{
		ExpectSelfTestStep(*this, Steps, NeedsASession, EOutcome::Skipped, TEXT("session did not start"));
	}
	// The steps that need only the playtest still ran.
	ExpectSelfTestStep(*this, Steps, WrongApiKeyRefused, EOutcome::Passed);
	ExpectSelfTestStep(*this, Steps, SessionWithNoPlayerRefused, EOutcome::Passed);
	TestEqual(TEXT("No end was sent for the launch's session, which never started"),
		Fixture.Transport->CountRequestsEndingWith(FString::Printf(TEXT("/%s/end"), PlaytestSessionId)), 0);
	return true;
}

/** A playtest that does not load fails the first step, and nothing else is sent. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSelfTestNeedsThePlaytestTest,
	"Flock.Playtest.SelfTest.WhenThePlaytestDoesNotLoadNothingElseRuns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSelfTestNeedsThePlaytestTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings FlockAnalytics(true);
	FPlaytestFixture Fixture;
	Fixture.AnswerConfig(FFlockPlaytestFakeTransport::Status(404, NotLinkedBody));
	Fixture.StartFlock();
	const int32 RequestsBefore = Fixture.Transport->Requests.Num();

	const TSharedRef<FFlockPlaytestSelfTest> Run = FFlockPlaytestSelfTest::Start(Fixture.Playtest, Fixture.Flock, SelfTestOptions(Fixture));
	// Not linked is final for the launch, so there is nothing to wait for.
	TestTrue(TEXT("The run finishes without waiting out a status that cannot change"), Run->IsFinished());

	const TArray<FFlockPlaytestSelfTestStep>& Steps = Run->GetSteps();
	ExpectSelfTestStep(*this, Steps, FlockPlaytestSelfTestSteps::PlaytestLoaded, EOutcome::Failed, TEXT("PlaytestNotLinked"));
	TestEqual(TEXT("Every other step was skipped"), Steps.FilterByPredicate([](const FFlockPlaytestSelfTestStep& Step)
		{
			return Step.Outcome == EOutcome::Skipped && Step.Detail.Contains(TEXT("did not load"));
		}).Num(), 16);
	TestEqual(TEXT("And nothing was sent"), Fixture.Transport->Requests.Num(), RequestsBefore);
	return true;
}

/**
 * With Flock sending each report the moment it is made, the self-test cannot watch one arrive, so the two report steps
 * say so rather than fail a game whose reports are fine. What it can see still counts: the plugin's own event name is
 * refused.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSelfTestReportsNotWatchableTest,
	"Flock.Playtest.SelfTest.SaysWhenReportsCannotBeWatched",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSelfTestReportsNotWatchableTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings FlockAnalytics(true, /*bCaptureExceptions*/ true, /*bKeepEventsUntilSent*/ false);
	FPlaytestFixture Fixture;
	Fixture.AnswerConfig(FFlockPlaytestFakeTransport::Status(200,
		ConfigBody(GameVersionId, /*bHeavyAnalytics*/ true, /*bVideoRecording*/ false, /*bExceptionCapturing*/ true)));
	Fixture.SignInToFlockOnStart();
	Fixture.StartFlock();
	AnswerEveryCounterCase(Fixture);

	const TSharedRef<FFlockPlaytestSelfTest> Run = FFlockPlaytestSelfTest::Start(Fixture.Playtest, Fixture.Flock, SelfTestOptions(Fixture));
	TestTrue(TEXT("The run finishes"), TickUntilTheSelfTestFinishes(Run, 10.f));

	using namespace FlockPlaytestSelfTestSteps;
	ExpectSelfTestStep(*this, Run->GetSteps(), ExceptionReported, EOutcome::Skipped, TEXT("Analytics Cache Failed Events is off"));
	ExpectSelfTestStep(*this, Run->GetSteps(), PlaytestEventRecorded, EOutcome::Skipped, TEXT("Analytics Cache Failed Events is off"));
	return true;
}

/**
 * Every counter-case is sent once. The self-test's own client never retries, whatever the Flock SDK's retry settings say:
 * a refusal it provokes is the answer it wants, and a server failing it should fail the step at once, not after backoff.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSelfTestSendsEachCounterCaseOnceTest,
	"Flock.Playtest.SelfTest.SendsEachCounterCaseOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSelfTestSendsEachCounterCaseOnceTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings FlockAnalytics(true);
	FScopedFlockRetrySettings Retries(3, /*bRetryUseJitter*/ false);
	FPlaytestFixture Fixture;
	Fixture.SignInToFlockOnStart();
	Fixture.StartFlock();

	// A server failing every request the self-test makes of its own.
	const FFlockHttpResponse Failing = FFlockPlaytestFakeTransport::Status(500, TEXT("{\"detail\":\"Internal Server Error\"}"));
	Fixture.AnswerConfig(Failing);
	Fixture.Transport->Answer(PlaytestSessionStartRoute, Failing);
	Fixture.Transport->Answer(SelfTestFeedbackFormRoute, Failing);
	Fixture.Transport->Answer(SelfTestUnknownSessionRoute(TEXT("/recording-upload")), Failing);
	Fixture.Transport->Answer(SelfTestUnknownSessionRoute(TEXT("/end")), Failing);
	const int32 RequestsBefore = Fixture.Transport->Requests.Num();

	const TSharedRef<FFlockPlaytestSelfTest> Run = FFlockPlaytestSelfTest::Start(Fixture.Playtest, Fixture.Flock, SelfTestOptions(Fixture));
	TestTrue(TEXT("The run finishes"), TickUntilTheSelfTestFinishes(Run, 10.f));

	TestEqual(TEXT("Three config requests"), SelfTestRequestsTo(*Fixture.Transport, PlaytestConfigRoute, RequestsBefore).Num(), 3);
	TestEqual(TEXT("One session start"), SelfTestRequestsTo(*Fixture.Transport, PlaytestSessionStartRoute, RequestsBefore).Num(), 1);
	TestEqual(TEXT("Four forms"), SelfTestRequestsTo(*Fixture.Transport, SelfTestFeedbackFormRoute, RequestsBefore).Num(), 4);
	TestEqual(TEXT("One upload link"),
		SelfTestRequestsTo(*Fixture.Transport, SelfTestUnknownSessionRoute(TEXT("/recording-upload")), RequestsBefore).Num(), 1);
	TestEqual(TEXT("One end"), SelfTestRequestsTo(*Fixture.Transport, SelfTestUnknownSessionRoute(TEXT("/end")), RequestsBefore).Num(), 1);
	ExpectSelfTestStep(*this, Run->GetSteps(), FlockPlaytestSelfTestSteps::WrongApiKeyRefused, EOutcome::Failed, TEXT("HTTP 500"));
	return true;
}

/**
 * The exception step passes only when the repeat was counted rather than queued. With the Flock SDK's repeat window at
 * zero every occurrence is reported, so the same fault raised twice queues two reports, and the step says so.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSelfTestRepeatQueuedFailsTest,
	"Flock.Playtest.SelfTest.AnExceptionWhoseRepeatIsQueuedFailsTheStep",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSelfTestRepeatQueuedFailsTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings FlockAnalytics(true, /*bCaptureExceptions*/ true, /*bKeepEventsUntilSent*/ true);
	FScopedSelfTestRepeatWindow NoRepeatWindow(0.f);
	FPlaytestFixture Fixture;
	Fixture.SignInToFlockOnStart();
	Fixture.StartFlock();
	AnswerEveryCounterCase(Fixture);

	const TSharedRef<FFlockPlaytestSelfTest> Run = FFlockPlaytestSelfTest::Start(Fixture.Playtest, Fixture.Flock, SelfTestOptions(Fixture));
	TestTrue(TEXT("The run finishes"), TickUntilTheSelfTestFinishes(Run, 10.f));

	ExpectSelfTestStep(*this, Run->GetSteps(), FlockPlaytestSelfTestSteps::ExceptionReported, EOutcome::Failed, TEXT("queued 2 reports"));
	SendWhatTheSelfTestLeftQueued(Fixture);
	return true;
}

/**
 * A flush that succeeds is not proof the event went: a flush that finds another already sending succeeds without
 * sending anything. So the step passes only once the queue goes down, and fails when it does not.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSelfTestEventNotSentFailsTest,
	"Flock.Playtest.SelfTest.AnEventStillQueuedAfterTheFlushFailsTheStep",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSelfTestEventNotSentFailsTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings FlockAnalytics(true, /*bCaptureExceptions*/ true, /*bKeepEventsUntilSent*/ true);
	FPlaytestFixture Fixture;
	Fixture.AnswerConfig(FFlockPlaytestFakeTransport::Status(200,
		ConfigBody(GameVersionId, /*bHeavyAnalytics*/ true, /*bVideoRecording*/ false, /*bExceptionCapturing*/ false)));
	Fixture.SignInToFlockOnStart();
	Fixture.StartFlock();
	AnswerEveryCounterCase(Fixture);

	// A delivery already on its way, whose answer has not come back: the self-test's flush finds it and sends nothing.
	FFlockAnalyticsProvider* Analytics = Fixture.Flock->GetAnalyticsProvider();
	if (!TestNotNull(TEXT("Precondition: Flock analytics is on"), Analytics))
	{
		return false;
	}
	Fixture.Transport->HoldRepliesToUrlsContaining.Add(TEXT("/analytics/events"));
	Fixture.Playtest->RecordPlaytestEvent(TEXT("already_on_its_way"));
	Analytics->Flush();

	const TSharedRef<FFlockPlaytestSelfTest> Run = FFlockPlaytestSelfTest::Start(Fixture.Playtest, Fixture.Flock, SelfTestOptions(Fixture));
	TestTrue(TEXT("The run finishes"), TickUntilTheSelfTestFinishes(Run, 10.f));
	ExpectSelfTestStep(*this, Run->GetSteps(), FlockPlaytestSelfTestSteps::PlaytestEventRecorded, EOutcome::Failed, TEXT("still queued"));

	Fixture.Transport->HoldRepliesToUrlsContaining.Reset();
	Fixture.Transport->ReleaseAllHeldReplies();
	SendWhatTheSelfTestLeftQueued(Fixture);
	TestEqual(TEXT("Nothing is left queued for another test"), Analytics->GetPendingAnalyticsEventCount(), 0);
	return true;
}

/** A launch whose session has already ended has none to check: the session step fails saying so, and nothing is ended twice. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSelfTestSessionAlreadyEndedTest,
	"Flock.Playtest.SelfTest.ASessionThatHasAlreadyEndedFailsTheSessionStep",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSelfTestSessionAlreadyEndedTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings FlockAnalytics(true);
	FPlaytestFixture Fixture;
	Fixture.SignInToFlockOnStart();
	Fixture.StartFlock();
	AnswerEveryCounterCase(Fixture);
	TestTrue(TEXT("Precondition: the game ended its session"), Fixture.Playtest->EndPlaytestSession());

	const TSharedRef<FFlockPlaytestSelfTest> Run = FFlockPlaytestSelfTest::Start(Fixture.Playtest, Fixture.Flock, SelfTestOptions(Fixture));
	TestTrue(TEXT("The run finishes"), TickUntilTheSelfTestFinishes(Run, 10.f));

	using namespace FlockPlaytestSelfTestSteps;
	ExpectSelfTestStep(*this, Run->GetSteps(), SessionStarted, EOutcome::Failed, TEXT("already ended"));
	ExpectSelfTestStep(*this, Run->GetSteps(), SessionEnded, EOutcome::Skipped, TEXT("session did not start"));
	TestEqual(TEXT("The session was ended once, by the game"),
		Fixture.Transport->CountRequestsEndingWith(FString::Printf(TEXT("/%s/end"), PlaytestSessionId)), 1);
	return true;
}

// The upload-finished event needs a recording, which only a build with video can make.
#if WITH_FLOCK_PLAYTEST_VIDEO

namespace
{
/** Hears the recording's upload-finished event for a test. */
struct FSelfTestUploadHeard
{
	TStrongObjectPtr<UFlockPlaytestSelfTestListener> Listener{ NewObject<UFlockPlaytestSelfTestListener>() };
	TSharedRef<TArray<TPair<bool, FString>>> Heard = MakeShared<TArray<TPair<bool, FString>>>();

	explicit FSelfTestUploadHeard(UFlockPlaytestSubsystem* Playtest)
	{
		const TSharedRef<TArray<TPair<bool, FString>>> Into = Heard;
		Listener->OnUploadFinished = [Into](bool bUploaded, const FString& WhyNot) { Into->Add({ bUploaded, WhyNot }); };
		Playtest->OnRecordingUploadFinished.AddDynamic(Listener.Get(), &UFlockPlaytestSelfTestListener::HandleRecordingUploadFinished);
	}

	/** Ticks the engine's core ticker until the event is heard or Seconds of ticks have passed. */
	void TickUntilHeard(float Seconds) const
	{
		for (float Waited = 0.f; Heard->Num() == 0 && Waited < Seconds; Waited += 0.1f)
		{
			FTSTicker::GetCoreTicker().Tick(0.1f);
		}
	}
};
}

/** The upload-finished event is raised once, uploaded, when the recording reaches storage. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestUploadFinishedWhenUploadedTest,
	"Flock.Playtest.RecordingUpload.RaisesUploadFinishedOnceUploaded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestUploadFinishedWhenUploadedTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	const TSharedRef<FFlockPlaytestFakeFileUploader> Uploader = RecordForTheSelfTest(Fixture);
	Fixture.AnswerConfig(FFlockPlaytestFakeTransport::Status(200, ConfigBody(GameVersionId, false, /*bVideoRecording*/ true)));
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(FirstFlockSessionId);
	RecordHalfASecond(Fixture);
	FSelfTestUploadHeard Heard(Fixture.Playtest);

	TestTrue(TEXT("Precondition: it stopped the recording"), Fixture.Playtest->StopVideoRecordingAndUploadIt());

	TestEqual(TEXT("Precondition: the recording was uploaded"), Uploader->Uploads.Num(), 1);
	if (TestEqual(TEXT("The event was raised once"), Heard.Heard->Num(), 1))
	{
		TestTrue(TEXT("Uploaded"), (*Heard.Heard)[0].Key);
		TestTrue(TEXT("With no reason"), (*Heard.Heard)[0].Value.IsEmpty());
	}
	return true;
}

/** A failed upload raises it too, not uploaded, with the reason. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestUploadFinishedWhenFailedTest,
	"Flock.Playtest.RecordingUpload.RaisesUploadFinishedWithTheReasonWhenItFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestUploadFinishedWhenFailedTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	const TSharedRef<FFlockPlaytestFakeFileUploader> Uploader = RecordForTheSelfTest(Fixture);
	Uploader->OnStatus(TEXT("/put/"), 500, TEXT("storage is down"));
	Fixture.AnswerConfig(FFlockPlaytestFakeTransport::Status(200, ConfigBody(GameVersionId, false, /*bVideoRecording*/ true)));
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(FirstFlockSessionId);
	RecordHalfASecond(Fixture);
	FSelfTestUploadHeard Heard(Fixture.Playtest);

	Fixture.Playtest->StopVideoRecordingAndUploadIt();
	// A failed upload is tried once more with a fresh link, which may come round on a later frame.
	Heard.TickUntilHeard(5.f);

	TestTrue(TEXT("Precondition: an upload was tried"), Uploader->Uploads.Num() >= 1);
	if (TestEqual(TEXT("The event was raised once"), Heard.Heard->Num(), 1))
	{
		TestFalse(TEXT("Not uploaded"), (*Heard.Heard)[0].Key);
		TestFalse(TEXT("With a reason"), (*Heard.Heard)[0].Value.IsEmpty());
	}
	return true;
}

/**
 * An upload that cannot even begin -- here, no Protokite session to send it to -- still raises the event, with the
 * reason. Without it, whoever asked for the upload would wait for an answer that never comes.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestUploadFinishedWhenItCannotBeginTest,
	"Flock.Playtest.RecordingUpload.RaisesUploadFinishedWhenItCannotBegin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestUploadFinishedWhenItCannotBeginTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	const TSharedRef<FFlockPlaytestFakeFileUploader> Uploader = RecordForTheSelfTest(Fixture);
	Fixture.AnswerConfig(FFlockPlaytestFakeTransport::Status(200, ConfigBody(GameVersionId, false, /*bVideoRecording*/ true)));
	Fixture.StartFlock();
	RecordHalfASecond(Fixture);
	FSelfTestUploadHeard Heard(Fixture.Playtest);
	TestEqual(TEXT("Precondition: no session started"), static_cast<int32>(Fixture.Playtest->GetPlaytestSessionState()), static_cast<int32>(EFlockPlaytestSessionState::NotStarted));

	TestTrue(TEXT("Precondition: it stopped the recording"), Fixture.Playtest->StopVideoRecordingAndUploadIt());

	TestEqual(TEXT("Precondition: nothing was uploaded"), Uploader->Uploads.Num(), 0);
	if (TestEqual(TEXT("The event was raised once"), Heard.Heard->Num(), 1))
	{
		TestFalse(TEXT("Not uploaded"), (*Heard.Heard)[0].Key);
		TestTrue(TEXT("Saying there was no session to send it to"), (*Heard.Heard)[0].Value.Contains(TEXT("session")));
	}
	return true;
}

/** A test video is never uploaded and nobody waits on one, so its finish raises nothing. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestUploadFinishedNotForTestVideoTest,
	"Flock.Playtest.RecordingUpload.RaisesNothingForATestVideo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestUploadFinishedNotForTestVideoTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	RecordForTheSelfTest(Fixture);
	Fixture.StartFlock();
	FSelfTestUploadHeard Heard(Fixture.Playtest);

	if (!TestTrue(TEXT("Precondition: a test video started"), Fixture.Playtest->StartTestVideoRecording(10.0)))
	{
		return false;
	}
	RecordHalfASecond(Fixture);
	TestTrue(TEXT("Precondition: it stopped"), Fixture.Playtest->StopVideoRecording());
	// A stop only asks: the finish is applied on a later video tick once the file is written, and that is where an
	// upload would begin. Checked before then, nothing could have been raised yet and this test would prove nothing.
	Fixture.Playtest->WaitUntilVideoWrittenForTesting();
	Fixture.Playtest->TickVideoRecordingForTesting(1.f / 60.f);
	TestFalse(TEXT("Precondition: the test video was finished and saved"), Fixture.Playtest->GetFinishedVideoRecordingPath().IsEmpty());

	TestEqual(TEXT("Nothing was raised"), Heard.Heard->Num(), 0);
	return true;
}

/** A game stopping its recording for good gets it uploaded like any finished one, and hears so. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestUploadFinishedAfterAStopTest,
	"Flock.Playtest.RecordingUpload.AStopForGoodUploadsItToo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestUploadFinishedAfterAStopTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	const TSharedRef<FFlockPlaytestFakeFileUploader> Uploader = RecordForTheSelfTest(Fixture);
	Fixture.AnswerConfig(FFlockPlaytestFakeTransport::Status(200, ConfigBody(GameVersionId, false, /*bVideoRecording*/ true)));
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(FirstFlockSessionId);
	RecordHalfASecond(Fixture);
	FSelfTestUploadHeard Heard(Fixture.Playtest);

	TestTrue(TEXT("Precondition: it stopped"), Fixture.Playtest->StopVideoRecording());
	Fixture.Playtest->WaitUntilVideoWrittenForTesting();
	Fixture.Playtest->TickVideoRecordingForTesting(1.f / 60.f);

	TestEqual(TEXT("It was uploaded"), Uploader->Uploads.Num(), 1);
	if (TestEqual(TEXT("And the event was raised once"), Heard.Heard->Num(), 1))
	{
		TestTrue(TEXT("Uploaded"), (*Heard.Heard)[0].Key);
	}
	return true;
}

/**
 * Nothing is raised into the game while it closes: its handlers would run against objects being torn down, and nothing
 * they did could act on the answer. The recording is kept for a later launch all the same.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestUploadFinishedNotWhileClosingTest,
	"Flock.Playtest.RecordingUpload.RaisesNothingWhileTheGameCloses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestUploadFinishedNotWhileClosingTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	const TSharedRef<FFlockPlaytestFakeFileUploader> Uploader = RecordForTheSelfTest(Fixture);
	Fixture.AnswerConfig(FFlockPlaytestFakeTransport::Status(200, ConfigBody(GameVersionId, false, /*bVideoRecording*/ true)));
	Fixture.StartFlock();
	Fixture.RegisterFlockSession(FirstFlockSessionId);
	RecordHalfASecond(Fixture);
	FSelfTestUploadHeard Heard(Fixture.Playtest);

	// The game instance goes away, which finishes the recording.
	Fixture.Playtest->Deinitialize();

	TestFalse(TEXT("Precondition: the recording was finished and kept"), Fixture.Playtest->GetFinishedVideoRecordingPath().IsEmpty());
	TestEqual(TEXT("Precondition: nothing was uploaded"), Uploader->Uploads.Num(), 0);
	TestEqual(TEXT("Nothing was raised"), Heard.Heard->Num(), 0);
	return true;
}

/**
 * A handler hearing that the upload could not begin finds the recording fully put away: the video ticker is settled
 * before anything is told, so a handler that calls back in does not meet a half-finished recording.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestUploadFinishedAfterTheTickerSettlesTest,
	"Flock.Playtest.RecordingUpload.RaisesItOnceTheRecordingIsPutAway",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestUploadFinishedAfterTheTickerSettlesTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	RecordForTheSelfTest(Fixture);
	Fixture.AnswerConfig(FFlockPlaytestFakeTransport::Status(200, ConfigBody(GameVersionId, false, /*bVideoRecording*/ true)));
	Fixture.StartFlock();
	RecordHalfASecond(Fixture);
	FSelfTestUploadHeard Heard(Fixture.Playtest);
	UFlockPlaytestSubsystem* Playtest = Fixture.Playtest;
	const TSharedRef<TArray<bool>> TickerRunningWhenHeard = MakeShared<TArray<bool>>();
	Heard.Listener->OnUploadFinished = [Playtest, TickerRunningWhenHeard](bool, const FString&)
	{
		TickerRunningWhenHeard->Add(Playtest->IsVideoTickerRunningForTesting());
	};

	// No session started, so the upload cannot begin and the answer comes straight away.
	TestTrue(TEXT("Precondition: it stopped the recording"), Fixture.Playtest->StopVideoRecordingAndUploadIt());

	if (TestEqual(TEXT("The event was raised once"), TickerRunningWhenHeard->Num(), 1))
	{
		TestFalse(TEXT("With the video ticker already stopped"), (*TickerRunningWhenHeard)[0]);
	}
	return true;
}

#endif // WITH_FLOCK_PLAYTEST_VIDEO

/** A game that goes away mid-run is one failure, not one for every step left: the rest are skipped, saying why. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestSelfTestGameShutDownTest,
	"Flock.Playtest.SelfTest.AGameThatShutsDownMidRunFailsOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestSelfTestGameShutDownTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FScopedFlockAnalyticsSettings FlockAnalytics(true);
	FPlaytestFixture Fixture;
	Fixture.StartFlock();
	AnswerEveryCounterCase(Fixture);

	// Nobody signs in, so the run waits on the session step; the game goes away while it waits.
	const TSharedRef<FFlockPlaytestSelfTest> Run = FFlockPlaytestSelfTest::Start(Fixture.Playtest, Fixture.Flock, SelfTestOptions(Fixture));
	TestFalse(TEXT("Precondition: it is waiting for the session"), Run->IsFinished());
	Fixture.Playtest->MarkAsGarbage();
	const bool bFinished = TickUntilTheSelfTestFinishes(Run, 10.f);
	Fixture.Playtest->ClearGarbage();
	TestTrue(TEXT("The run finishes"), bFinished);

	const TArray<FFlockPlaytestSelfTestStep>& Steps = Run->GetSteps();
	TestEqual(TEXT("One step fails for it"), Steps.FilterByPredicate([](const FFlockPlaytestSelfTestStep& Step)
		{
			return Step.Outcome == EOutcome::Failed;
		}).Num(), 1);
	ExpectSelfTestStep(*this, Steps, FlockPlaytestSelfTestSteps::SessionStarted, EOutcome::Failed, TEXT("the game shut down"));
	ExpectSelfTestStep(*this, Steps, FlockPlaytestSelfTestSteps::SessionEnded, EOutcome::Skipped, TEXT("the game shut down"));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
