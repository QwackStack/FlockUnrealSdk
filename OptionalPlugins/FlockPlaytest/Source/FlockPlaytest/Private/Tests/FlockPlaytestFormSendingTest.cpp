// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockPlaytestFormSpool.h"
#include "FlockPlaytestFormSubmission.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/FlockPlaytestSubsystemTestSupport.h"
#include "Tests/FlockPlaytestTestSupport.h"

using namespace FlockPlaytestSubsystemTesting;
using namespace FlockPlaytestFixtures;

namespace
{
	const TCHAR* const FeedbackFormRoute = TEXT("/game/sdk/feedback-form");
	const TCHAR* const EarlierSessionId = TEXT("01KX0EARLIERSESSION0000001");
	const TCHAR* const EarlierGameVersionId = TEXT("pt-earlier-version");
	const TCHAR* const EarlierDeviceId = TEXT("0f8fad5b-d9cb-469f-a165-70867728950e");

	/** The server's answer to a form it took: the stored response, enveloped. */
	FString FormTakenBody()
	{
		return Envelope(TEXT("{\"id\":\"01KX0FORMRESPONSE000000001\",\"form_id\":\"01KX0FORM000000000000000000\"}"));
	}

	/** Answers the default fixture form takes: a rating, a picked option and some text. */
	FFlockPlaytestFormAnswers FilledInAnswers()
	{
		FFlockPlaytestFormAnswers Answers;
		Answers.SetRating(TEXT("rating"), 4);
		Answers.SetChosenOption(TEXT("category"), TEXT("Bug"));
		Answers.SetText(TEXT("steps"), TEXT("Opened the map"));
		return Answers;
	}

	TSharedPtr<FJsonObject> ParseObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	/** Forms waiting in one fixture's own folder. */
	int32 FormsKept(const FString& Folder)
	{
		return FFlockPlaytestFormSpool(Folder).CountWaiting();
	}

	/** A ready fixture whose kept forms live in a folder of its own, so no test sends another's. */
	FString StartReadyWithItsOwnFormFolder(FPlaytestFixture& Fixture)
	{
		const FString Folder = FPaths::Combine(Fixture.Folder, TEXT("FeedbackForms"));
		Fixture.Playtest->SetFormSpoolFolderForTesting(Folder);
		Fixture.StartFlock();
		return Folder;
	}

	/** A config with no published form: the server sends form as null until the studio publishes one. */
	FString ConfigBodyWithNoForm()
	{
		return Envelope(FString::Printf(TEXT("{\"session_started_event\":\"session_started\",\"test_id\":\"%s\",")
			TEXT("\"flock_game_version_id\":\"%s\",\"features\":{\"video_recording\":false,\"exception_capturing\":true,")
			TEXT("\"heavy_analytics\":false},\"form\":null}"), TestId, GameVersionId));
	}

	/** Keeps a form the way an earlier launch that could not send it left it. */
	bool KeepFormFromAnEarlierLaunch(const FString& Folder)
	{
		FFlockPlaytestFormSubmission Earlier;
		Earlier.PlaytestSessionId = EarlierSessionId;
		Earlier.Identity.DeviceId = EarlierDeviceId;
		Earlier.ProtokiteApiUrl = UsableUrl;
		Earlier.FlockGameVersionId = EarlierGameVersionId;
		Earlier.AnswersJson = TEXT("{\"rating\":5}");
		FString Error;
		return !FFlockPlaytestFormSpool(Folder).Keep(Earlier, Error).IsEmpty();
	}
}

/**
 * A filled-in form is written down before it is sent, sent with this launch's key and version, and forgotten once the
 * server takes it. Until now this was proven only against a running backend.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestFormSendingSentThenForgottenTest,
	"Flock.Playtest.Form.Sending.SentAndForgottenOnceTaken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestFormSendingSentThenForgottenTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	Fixture.Transport->Answer(FeedbackFormRoute, FFlockPlaytestFakeTransport::Status(200, FormTakenBody()));
	const FString Folder = StartReadyWithItsOwnFormFolder(Fixture);
	ExpectPlaytestStatus(*this, TEXT("Ready"), Fixture.Playtest->GetStatus(), EFlockPlaytestStatus::Ready);

	TestTrue(TEXT("Sending reports it went"), Fixture.Playtest->SendFilledInForm(FilledInAnswers()));

	TestEqual(TEXT("One form was sent"), Fixture.Transport->CountRequestsEndingWith(FeedbackFormRoute), 1);
	const FFlockHttpRequest* Request = Fixture.Transport->FindLastRequestEndingWith(FeedbackFormRoute);
	if (TestNotNull(TEXT("The request"), Request))
	{
		TestTrue(TEXT("To this build's Protokite"), Request->Url.StartsWith(UsableUrl, ESearchCase::CaseSensitive));
		TestEqual(TEXT("With this launch's API key"), Request->Headers.FindRef(TEXT("X-Flock-API-Key")), FString(TEXT("secret")));
		TestEqual(TEXT("And its Game Version ID"), Request->Headers.FindRef(TEXT("X-Game-Version-ID")), FString(GameVersionId));
		const TSharedPtr<FJsonObject> Body = ParseObject(Request->JsonBody);
		TestFalse(TEXT("Naming who filled it in"), StringMember(Body, TEXT("device_id")).Equals(TEXT("<absent>")));
		const TSharedPtr<FJsonObject>* Answers = nullptr;
		if (TestTrue(TEXT("With the answers"), Body.IsValid() && Body->TryGetObjectField(TEXT("answers"), Answers) && Answers != nullptr))
		{
			TestEqual(TEXT("The rating as a number"), (*Answers)->GetNumberField(TEXT("rating")), 4.0);
			TestEqual(TEXT("The picked option"), StringMember(*Answers, TEXT("category")), FString(TEXT("Bug")));
			TestEqual(TEXT("The text"), StringMember(*Answers, TEXT("steps")), FString(TEXT("Opened the map")));
		}
	}
	TestEqual(TEXT("Nothing is left waiting once the server took it"), FormsKept(Folder), 0);
	return true;
}

/** A form that cannot go now is kept for a later launch: the answers are the one thing that cannot be asked for again. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestFormSendingKeptTest,
	"Flock.Playtest.Form.Sending.KeptWhenItCannotGoNow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestFormSendingKeptTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	Fixture.Transport->AnswerInOrder(FeedbackFormRoute, {
		FFlockPlaytestFakeTransport::Status(503, TEXT("{\"detail\":\"Service Unavailable\"}")),
		FFlockPlaytestFakeTransport::ConnectionFailure() });
	const FString Folder = StartReadyWithItsOwnFormFolder(Fixture);

	TestTrue(TEXT("A server having a moment: reported as on its way"), Fixture.Playtest->SendFilledInForm(FilledInAnswers()));
	TestEqual(TEXT("And kept"), FormsKept(Folder), 1);

	TestTrue(TEXT("No network at all: reported as on its way"), Fixture.Playtest->SendFilledInForm(FilledInAnswers()));
	TestEqual(TEXT("And kept too"), FormsKept(Folder), 2);
	return true;
}

/**
 * A refusal the server will repeat is dropped rather than kept, and the question it named is logged: Protokite puts it
 * only inside the message.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestFormSendingRefusedTest,
	"Flock.Playtest.Form.Sending.DroppedWhenTheServerRefusesIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestFormSendingRefusedTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestLogCapture Log;
	FPlaytestFixture Fixture;
	Fixture.Transport->AnswerInOrder(FeedbackFormRoute, {
		FFlockPlaytestFakeTransport::Status(422, TEXT("{\"detail\":\"Missing required answer 'rating'\"}")),
		FFlockPlaytestFakeTransport::Status(404, TEXT("{\"detail\":\"Session not found for this test\"}")) });
	const FString Folder = StartReadyWithItsOwnFormFolder(Fixture);

	Fixture.Playtest->SendFilledInForm(FilledInAnswers());
	TestEqual(TEXT("A 422 is not kept"), FormsKept(Folder), 0);
	const TArray<FPlaytestLogCapture::FLine> Lines = Log.LinesContaining(TEXT("The feedback form was not sent"));
	if (TestEqual(TEXT("It is logged"), Lines.Num(), 1))
	{
		TestEqual(TEXT("As a warning"), static_cast<int32>(Lines[0].Verbosity), static_cast<int32>(ELogVerbosity::Warning));
		TestTrue(TEXT("Naming the refused question"), Lines[0].Message.Contains(TEXT("the question 'rating' was refused"), ESearchCase::CaseSensitive));
	}

	Fixture.Playtest->SendFilledInForm(FilledInAnswers());
	TestEqual(TEXT("Nor is a 404"), FormsKept(Folder), 0);
	TestEqual(TEXT("Both were sent"), Fixture.Transport->CountRequestsEndingWith(FeedbackFormRoute), 2);
	return true;
}

/** With no published form nothing is sent or kept, and the call says so rather than returning quietly. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestFormSendingNoFormTest,
	"Flock.Playtest.Form.Sending.NothingWithoutAPublishedForm",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestFormSendingNoFormTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestLogCapture Log;
	FPlaytestFixture Fixture;
	Fixture.AnswerConfig(FFlockPlaytestFakeTransport::Status(200, ConfigBodyWithNoForm()));
	Fixture.Transport->Answer(FeedbackFormRoute, FFlockPlaytestFakeTransport::Status(200, FormTakenBody()));
	const FString Folder = StartReadyWithItsOwnFormFolder(Fixture);
	ExpectPlaytestStatus(*this, TEXT("Ready"), Fixture.Playtest->GetStatus(), EFlockPlaytestStatus::Ready);
	TestFalse(TEXT("Precondition: there is no form to open"), Fixture.Playtest->CanOpenFeedbackForm());

	TestFalse(TEXT("Sending reports it did not go"), Fixture.Playtest->SendFilledInForm(FilledInAnswers()));
	TestEqual(TEXT("Nothing was sent"), Fixture.Transport->CountRequestsEndingWith(FeedbackFormRoute), 0);
	TestEqual(TEXT("Nothing was kept"), FormsKept(Folder), 0);
	const TArray<FPlaytestLogCapture::FLine> Lines = Log.LinesContaining(TEXT("no published feedback form"));
	if (TestEqual(TEXT("It says why"), Lines.Num(), 1))
	{
		TestEqual(TEXT("As a warning"), static_cast<int32>(Lines[0].Verbosity), static_cast<int32>(ELogVerbosity::Warning));
	}
	return true;
}

/**
 * A form an earlier launch kept is sent once this launch's Flock SDK is running: this launch's API key, but the Game
 * Version ID the form's own session ran under, because Protokite finds the playtest from it. Inside one launch the two
 * versions are equal, so only a kept form can tell them apart.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestFormSendingLaterLaunchTest,
	"Flock.Playtest.Form.Sending.ALaterLaunchSendsWhatWasKept",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestFormSendingLaterLaunchTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	const FString Folder = FPaths::Combine(Fixture.Folder, TEXT("FeedbackForms"));
	if (!TestTrue(TEXT("An earlier launch's form is kept"), KeepFormFromAnEarlierLaunch(Folder)))
	{
		return false;
	}
	Fixture.Transport->Answer(FeedbackFormRoute, FFlockPlaytestFakeTransport::Status(200, FormTakenBody()));
	StartReadyWithItsOwnFormFolder(Fixture);

	// Sent from the ticker that waits for the Flock SDK and for the launch's pass over the recordings.
	RunPendingPlaytestRetries();

	TestEqual(TEXT("The kept form was sent"), Fixture.Transport->CountRequestsEndingWith(FeedbackFormRoute), 1);
	const FFlockHttpRequest* Request = Fixture.Transport->FindLastRequestEndingWith(FeedbackFormRoute);
	if (TestNotNull(TEXT("The request"), Request))
	{
		TestEqual(TEXT("With this launch's API key"), Request->Headers.FindRef(TEXT("X-Flock-API-Key")), FString(TEXT("secret")));
		TestEqual(TEXT("And the Game Version ID its own session ran under"), Request->Headers.FindRef(TEXT("X-Game-Version-ID")),
			FString(EarlierGameVersionId));
		const TSharedPtr<FJsonObject> Body = ParseObject(Request->JsonBody);
		TestEqual(TEXT("Naming its own session"), StringMember(Body, TEXT("session_id")), FString(EarlierSessionId));
		TestEqual(TEXT("And whoever filled it in then"), StringMember(Body, TEXT("device_id")), FString(EarlierDeviceId));
	}
	TestEqual(TEXT("It stops waiting once taken"), FormsKept(Folder), 0);
	return true;
}

/** The counter-case: a kept form that still cannot go stays kept for the launch after. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestFormSendingStillKeptTest,
	"Flock.Playtest.Form.Sending.StillKeptWhenItCannotGoAgain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestFormSendingStillKeptTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	const FString Folder = FPaths::Combine(Fixture.Folder, TEXT("FeedbackForms"));
	if (!TestTrue(TEXT("An earlier launch's form is kept"), KeepFormFromAnEarlierLaunch(Folder)))
	{
		return false;
	}
	Fixture.Transport->Answer(FeedbackFormRoute, FFlockPlaytestFakeTransport::Status(503, TEXT("{\"detail\":\"Service Unavailable\"}")));
	StartReadyWithItsOwnFormFolder(Fixture);
	RunPendingPlaytestRetries();

	TestEqual(TEXT("It was tried"), Fixture.Transport->CountRequestsEndingWith(FeedbackFormRoute), 1);
	TestEqual(TEXT("And is still kept"), FormsKept(Folder), 1);
	return true;
}

/** A form whose question ids differ only in letter case is named once, as a warning, when it loads. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestFormSendingCaseWarningTest,
	"Flock.Playtest.Form.WarnsAboutQuestionsItCannotTellApart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestFormSendingCaseWarningTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestLogCapture Log;
	FPlaytestFixture Fixture;
	Fixture.AnswerConfig(FFlockPlaytestFakeTransport::Status(200, Envelope(FString::Printf(
		TEXT("{\"session_started_event\":\"session_started\",\"test_id\":\"%s\",\"flock_game_version_id\":\"%s\",")
		TEXT("\"features\":{},\"form\":{\"id\":\"01KX0FORM000000000000000000\",\"title\":\"Feedback\",\"fields\":[")
		TEXT("{\"id\":\"Q1\",\"type\":\"text\",\"label\":\"First\",\"required\":false,\"options\":[]},")
		TEXT("{\"id\":\"q1\",\"type\":\"text\",\"label\":\"Second\",\"required\":false,\"options\":[]}]}}"),
		TestId, GameVersionId))));
	StartReadyWithItsOwnFormFolder(Fixture);
	ExpectPlaytestStatus(*this, TEXT("Ready"), Fixture.Playtest->GetStatus(), EFlockPlaytestStatus::Ready);

	const TArray<FPlaytestLogCapture::FLine> Lines = Log.LinesContaining(TEXT("differ only in letter case"));
	if (TestEqual(TEXT("Said once"), Lines.Num(), 1))
	{
		TestEqual(TEXT("As a warning"), static_cast<int32>(Lines[0].Verbosity), static_cast<int32>(ELogVerbosity::Warning));
		TestTrue(TEXT("Naming both"), Lines[0].Message.Contains(TEXT("'Q1' and 'q1'"), ESearchCase::CaseSensitive));
	}
	return true;
}

/** The control: the fixture's own form, whose ids all differ, loads without a word about it. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestFormSendingNoCaseWarningTest,
	"Flock.Playtest.Form.SaysNothingWhenEveryQuestionIdDiffers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestFormSendingNoCaseWarningTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestLogCapture Log;
	FPlaytestFixture Fixture;
	StartReadyWithItsOwnFormFolder(Fixture);
	ExpectPlaytestStatus(*this, TEXT("Ready"), Fixture.Playtest->GetStatus(), EFlockPlaytestStatus::Ready);
	TestTrue(TEXT("Precondition: a form loaded"), Fixture.Playtest->CanOpenFeedbackForm());
	TestEqual(TEXT("Nothing said"), Log.LinesContaining(TEXT("differ only in letter case")).Num(), 0);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
