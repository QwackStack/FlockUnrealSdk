// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FlockPlaytestConsent.h"
#include "FlockPlaytestLibrary.h"
#include "FlockPlaytestSubsystem.h"
#include "Http/FlockJsonUtils.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Tests/FlockPlaytestSubsystemTestSupport.h"

using namespace FlockPlaytestSubsystemTesting;

namespace
{
	/** The game instance of the game actually running, the one a graph in that game resolves its nodes against. */
	UGameInstance* FindRunningGameInstance()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::Game && Context.OwningGameInstance != nullptr)
			{
				return Context.OwningGameInstance;
			}
		}
		return nullptr;
	}

	FString AnswersOnTheWire(const FFlockPlaytestFormAnswers& Answers, const FFlockPlaytestForm& Form)
	{
		FString Json;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Answers.ToWireObject(Form), Writer);
		return Json;
	}
}

/**
 * Every node is safe where there is no playtest to find: no world, or an object in none. It answers false, empty or
 * Turned Off, and changes nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestLibrarySafeWithoutAPlaytestTest, "Flock.Playtest.Library.SafeWithoutAPlaytest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestLibrarySafeWithoutAPlaytestTest::RunTest(const FString& Parameters)
{
	for (const UObject* Context : { static_cast<const UObject*>(nullptr), static_cast<const UObject*>(GetTransientPackage()) })
	{
		const FString Which = Context == nullptr ? TEXT("no object") : TEXT("an object in no world");
		TestEqual(*(Which + TEXT(": status")), static_cast<int32>(UFlockPlaytestLibrary::GetPlaytestStatus(Context)),
			static_cast<int32>(EFlockPlaytestStatus::TurnedOff));
		TestFalse(*(Which + TEXT(": not ready")), UFlockPlaytestLibrary::IsPlaytestReady(Context));
		TestFalse(*(Which + TEXT(": no feature")), UFlockPlaytestLibrary::IsPlaytestFeatureEnabled(Context, FlockPlaytestFeatures::VideoRecording));
		TestTrue(*(Which + TEXT(": no session")), UFlockPlaytestLibrary::GetPlaytestSessionId(Context).IsEmpty());
		TestFalse(*(Which + TEXT(": no session to end")), UFlockPlaytestLibrary::EndPlaytestSession(Context));
		TestFalse(*(Which + TEXT(": no event recorded")), UFlockPlaytestLibrary::RecordPlaytestEvent(Context, TEXT("boss_defeated"), FFlockCommandData()));
		TestFalse(*(Which + TEXT(": no recording to stop")), UFlockPlaytestLibrary::StopVideoRecording(Context));
		TestFalse(*(Which + TEXT(": not recording")), UFlockPlaytestLibrary::IsRecordingVideo(Context));
		TestFalse(*(Which + TEXT(": no recording to send")), UFlockPlaytestLibrary::CanSendPlaytestRecording(Context));
		TestFalse(*(Which + TEXT(": nothing uploaded")), UFlockPlaytestLibrary::StopAndUploadPlaytestRecording(Context));
		TestFalse(*(Which + TEXT(": no form to open")), UFlockPlaytestLibrary::CanOpenFeedbackForm(Context));
		TestFalse(*(Which + TEXT(": the form does not open")), UFlockPlaytestLibrary::OpenFeedbackForm(Context));
		TestFalse(*(Which + TEXT(": no form to close")), UFlockPlaytestLibrary::CloseFeedbackForm(Context));
		TestFalse(*(Which + TEXT(": no form open")), UFlockPlaytestLibrary::IsFeedbackFormOpen(Context));
		FFlockPlaytestForm Form;
		Form.Id = TEXT("left over");
		TestFalse(*(Which + TEXT(": no form to read")), UFlockPlaytestLibrary::GetFeedbackForm(Context, Form));
		TestTrue(*(Which + TEXT(": and the form handed back is empty")), Form.Id.IsEmpty() && Form.Fields.Num() == 0);
		TestFalse(*(Which + TEXT(": no answers sent")), UFlockPlaytestLibrary::SendFeedbackFormAnswers(Context, FFlockPlaytestFormAnswers()));
	}
	TestFalse(TEXT("Every status is described"), UFlockPlaytestLibrary::DescribePlaytestStatus(EFlockPlaytestStatus::TurnedOff).IsEmpty());
	return true;
}

/**
 * In the game that is running, the nodes answer exactly what its playtest subsystem answers, found from the game
 * instance or from its world. This is the lookup a graph uses, so it runs where a graph does: under -game, against
 * the subsystem the game instance made, not one a test built.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestLibraryRunningGameTest, "Flock.Playtest.Library.AnswersForTheRunningGame",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestLibraryRunningGameTest::RunTest(const FString& Parameters)
{
	UGameInstance* GameInstance = FindRunningGameInstance();
	if (!TestNotNull(TEXT("A game is running"), GameInstance))
	{
		return false;
	}
	const UFlockPlaytestSubsystem* Playtest = GameInstance->GetSubsystem<UFlockPlaytestSubsystem>();
	if (!TestNotNull(TEXT("With its playtest subsystem"), Playtest))
	{
		return false;
	}
	// A lookup that found nothing answers Turned Off, so only a game whose playtesting is on tells the two apart.
	if (Playtest->GetStatus() == EFlockPlaytestStatus::TurnedOff)
	{
		AddInfo(TEXT("Playtesting is turned off in this project, so a node that found no subsystem would answer the same."));
	}

	for (const UObject* Context : { static_cast<const UObject*>(GameInstance), static_cast<const UObject*>(GameInstance->GetWorld()) })
	{
		const FString Which = Context == GameInstance ? TEXT("from the game instance") : TEXT("from its world");
		TestEqual(*(Which + TEXT(": status")), static_cast<int32>(UFlockPlaytestLibrary::GetPlaytestStatus(Context)),
			static_cast<int32>(Playtest->GetStatus()));
		TestEqual(*(Which + TEXT(": ready")), UFlockPlaytestLibrary::IsPlaytestReady(Context),
			Playtest->GetStatus() == EFlockPlaytestStatus::Ready);
		for (const TCHAR* Feature : { FlockPlaytestFeatures::VideoRecording, FlockPlaytestFeatures::ExceptionCapturing, FlockPlaytestFeatures::HeavyAnalytics })
		{
			TestEqual(*(Which + TEXT(": feature ") + Feature), UFlockPlaytestLibrary::IsPlaytestFeatureEnabled(Context, Feature),
				Playtest->IsPlaytestFeatureEnabled(Feature));
		}
		TestEqual(*(Which + TEXT(": what the player let it collect")),
			static_cast<int32>(UFlockPlaytestLibrary::GetPlaytestConsent(Context)),
			static_cast<int32>(Playtest->GetPlaytestConsent()));
		TestEqual(*(Which + TEXT(": what the player answered")),
			static_cast<int32>(UFlockPlaytestLibrary::GetPlayersConsentAnswer(Context)),
			static_cast<int32>(Playtest->GetPlayersConsentAnswer()));
		TestEqual(*(Which + TEXT(": the consent question open")), UFlockPlaytestLibrary::IsConsentQuestionOpen(Context),
			Playtest->IsConsentQuestionOpen());
		TestEqual(*(Which + TEXT(": session id")), UFlockPlaytestLibrary::GetPlaytestSessionId(Context), Playtest->GetPlaytestSessionId());
		TestEqual(*(Which + TEXT(": recording")), UFlockPlaytestLibrary::IsRecordingVideo(Context), Playtest->IsRecordingVideo());
		TestEqual(*(Which + TEXT(": recording to send")), UFlockPlaytestLibrary::CanSendPlaytestRecording(Context), Playtest->CanSendTheRecording());
		TestEqual(*(Which + TEXT(": form to open")), UFlockPlaytestLibrary::CanOpenFeedbackForm(Context), Playtest->CanOpenFeedbackForm());
		TestEqual(*(Which + TEXT(": form open")), UFlockPlaytestLibrary::IsFeedbackFormOpen(Context), Playtest->IsFeedbackFormOpen());
		FFlockPlaytestForm Form;
		TestEqual(*(Which + TEXT(": form to read")), UFlockPlaytestLibrary::GetFeedbackForm(Context, Form), Playtest->CanOpenFeedbackForm());
		TestEqual(*(Which + TEXT(": the form read")), Form.Id, Playtest->CanOpenFeedbackForm() ? Playtest->GetPlaytestConfig().Form.Id : FString());
	}

	// Describing an answer is the plugin's own sentence, whatever this game is running.
	for (const EFlockPlaytestConsentChoice Choice : { EFlockPlaytestConsentChoice::NotAnswered,
		EFlockPlaytestConsentChoice::VideoAndPlayData, EFlockPlaytestConsentChoice::VideoOnly,
		EFlockPlaytestConsentChoice::PlayDataOnly, EFlockPlaytestConsentChoice::Nothing })
	{
		TestEqual(TEXT("Describe playtest consent says what the rules say"),
			UFlockPlaytestLibrary::DescribePlaytestConsent(Choice), FlockPlaytestConsent::Describe(Choice));
	}
	return true;
}

/**
 * The name nodes are the server's names letter for letter. A graph never types them, so these nodes are the only place
 * a misspelling could enter, and a misspelt feature reads as off rather than failing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestLibraryNamesTest, "Flock.Playtest.Library.NamesAreTheServersNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestLibraryNamesTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("video_recording"), UFlockPlaytestLibrary::PlaytestFeatureVideoRecording().Equals(TEXT("video_recording"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("exception_capturing"), UFlockPlaytestLibrary::PlaytestFeatureExceptionCapturing().Equals(TEXT("exception_capturing"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("heavy_analytics"), UFlockPlaytestLibrary::PlaytestFeatureHeavyAnalytics().Equals(TEXT("heavy_analytics"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("text"), UFlockPlaytestLibrary::FeedbackQuestionKindText().Equals(TEXT("text"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("textarea"), UFlockPlaytestLibrary::FeedbackQuestionKindTextArea().Equals(TEXT("textarea"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("rating"), UFlockPlaytestLibrary::FeedbackQuestionKindRating().Equals(TEXT("rating"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("select"), UFlockPlaytestLibrary::FeedbackQuestionKindSelect().Equals(TEXT("select"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("checkbox"), UFlockPlaytestLibrary::FeedbackQuestionKindCheckbox().Equals(TEXT("checkbox"), ESearchCase::CaseSensitive));
	return true;
}

/**
 * Answers a graph builds for a form of its own are the answers C++ builds: the same problems found, the same answers on
 * the wire. The nodes delegate, so a graph and C++ cannot disagree about what the server will take.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestLibraryOwnFormTest, "Flock.Playtest.Library.OwnFormAnswersMatchCpp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestLibraryOwnFormTest::RunTest(const FString& Parameters)
{
	FFlockPlaytestConfig Config;
	FString Error;
	if (!TestTrue(TEXT("The fixture config reads"), FFlockJsonUtils::UnwrapResultToStruct(FlockPlaytestFixtures::ConfigBody(), Config, Error)))
	{
		return false;
	}
	const FFlockPlaytestForm& Form = Config.Form;

	// Nothing answered yet: the two needed questions are reported, by both.
	const FFlockPlaytestFormAnswers Empty;
	TestEqual(TEXT("An empty form's problems"), UFlockPlaytestLibrary::FindFeedbackFormProblems(Empty, Form).Num(), Empty.FindProblems(Form).Num());
	TestEqual(TEXT("Are the two needed questions"), Empty.FindProblems(Form).Num(), 2);

	FFlockPlaytestFormAnswers ByNodes = UFlockPlaytestLibrary::SetFeedbackRatingAnswer(FFlockPlaytestFormAnswers(), TEXT("rating"), 5);
	ByNodes = UFlockPlaytestLibrary::SetFeedbackChosenOption(ByNodes, TEXT("category"), TEXT("Crash"));
	ByNodes = UFlockPlaytestLibrary::SetFeedbackTextAnswer(ByNodes, TEXT("steps"), TEXT("  Jumped off the ledge  "));
	ByNodes = UFlockPlaytestLibrary::SetFeedbackCheckboxAnswer(ByNodes, TEXT("not_on_the_form"), false);

	FFlockPlaytestFormAnswers ByCpp;
	ByCpp.SetRating(TEXT("rating"), 5);
	ByCpp.SetChosenOption(TEXT("category"), TEXT("Crash"));
	ByCpp.SetText(TEXT("steps"), TEXT("  Jumped off the ledge  "));
	ByCpp.SetChecked(TEXT("not_on_the_form"), false);

	TestEqual(TEXT("No problems either way"), UFlockPlaytestLibrary::FindFeedbackFormProblems(ByNodes, Form).Num(), 0);
	TestEqual(TEXT("The same answers on the wire"), AnswersOnTheWire(ByNodes, Form), AnswersOnTheWire(ByCpp, Form));
	TestTrue(TEXT("Chaining a node leaves its input alone"),
		UFlockPlaytestLibrary::SetFeedbackRatingAnswer(Empty, TEXT("rating"), 3).IsAnswered(TEXT("rating")) && !Empty.IsAnswered(TEXT("rating")));

	// An option the question never offered is a problem for the graph exactly as it is for C++.
	const FFlockPlaytestFormAnswers WrongOption = UFlockPlaytestLibrary::SetFeedbackChosenOption(ByNodes, TEXT("category"), TEXT("crash"));
	const TArray<FFlockPlaytestFormProblem> Problems = UFlockPlaytestLibrary::FindFeedbackFormProblems(WrongOption, Form);
	if (TestEqual(TEXT("An option not offered, letter for letter"), Problems.Num(), 1))
	{
		TestEqual(TEXT("Named against its question"), Problems[0].FieldId, FString(TEXT("category")));
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
