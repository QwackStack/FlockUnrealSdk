// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "ProtokitePlaytestSubsystem.h"
#include "SProtokitePlaytestFormWidget.h"
#include "Styling/SlateBrush.h"
#include "Tests/ProtokitePlaytestSubsystemTestSupport.h"

using namespace ProtokitePlaytestSubsystemTesting;
using namespace ProtokitePlaytestFixtures;

namespace
{
	/** Reads a form out of a config answer, so a test uses the same parse the game does rather than building one by hand. */
	bool ReadFormFromConfigBody(FAutomationTestBase& Test, const FString& ConfigJson, FProtokitePlaytestForm& OutForm)
	{
		TSharedPtr<FJsonObject> Envelope;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ConfigJson);
		if (!Test.TestTrue(TEXT("The config answer parses"), FJsonSerializer::Deserialize(Reader, Envelope) && Envelope.IsValid()))
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* Result = nullptr;
		if (!Test.TestTrue(TEXT("It has a result"), Envelope->TryGetObjectField(TEXT("result"), Result)))
		{
			return false;
		}
		FProtokitePlaytestConfig Config;
		FString Error;
		if (!Test.TestTrue(FString::Printf(TEXT("The config reads (%s)"), *Error),
			FProtokitePlaytestConfig::FromWireObject(Result->ToSharedRef(), Config, Error)))
		{
			return false;
		}
		OutForm = Config.Form;
		return true;
	}

	/** A form a studio edited: its own ids, its own questions, a select with its own options. */
	FString StudioEditedConfigBody()
	{
		return Envelope(FString::Printf(TEXT("{\"session_started_event\":\"session_started\",\"test_id\":\"%s\",")
			TEXT("\"flock_game_version_id\":\"%s\",\"features\":{},")
			TEXT("\"form\":{\"id\":\"01KX0FORM111111111111111111\",\"test_id\":\"%s\",\"game_id\":\"g\",")
			TEXT("\"title\":\"Tell us about the boss\",\"description\":\"Two questions, then back to it.\",\"is_published\":true,\"fields\":[")
			TEXT("{\"id\":\"boss_difficulty\",\"type\":\"select\",\"label\":\"How hard was the boss?\",\"required\":true,")
			TEXT("\"help_text\":\"Pick the closest.\",\"options\":[\"Too easy\",\"About right\",\"Too hard\"]},")
			TEXT("{\"id\":\"free_text_notes\",\"type\":\"textarea\",\"label\":\"Anything else?\",\"required\":false,\"help_text\":null,\"options\":[]}],")
			TEXT("\"created_at\":\"2026-09-16T10:00:00Z\",\"updated_at\":\"2026-09-16T10:00:00Z\"}}"),
			TestId, GameVersionId, TestId));
	}
}

/**
 * The backend's own default form, read through the real parse and built into the real widget. Nothing about it is
 * written into the plugin: the questions, their kinds, their options and which are needed all come from the config.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormBuildsDefaultFormTest,
	"Protokite.Playtest.Form.BuildsTheBackendsDefaultForm",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormBuildsDefaultFormTest::RunTest(const FString& Parameters)
{
	FProtokitePlaytestForm Form;
	if (!ReadFormFromConfigBody(*this, ConfigBody(), Form))
	{
		return false;
	}
	TestEqual(TEXT("Three questions"), Form.Fields.Num(), 3);

	const TSharedRef<SProtokitePlaytestFormWidget> Widget = SNew(SProtokitePlaytestFormWidget).Form(Form);

	// Nothing filled in: the two needed questions are refused, and named.
	TArray<FProtokitePlaytestFormProblem> Problems = Widget->TrySubmitForTesting();
	if (TestEqual(TEXT("The two needed questions are refused"), Problems.Num(), 2))
	{
		TestEqual(TEXT("The rating first, in the studio's order"), Problems[0].FieldId, FString(TEXT("rating")));
		TestEqual(TEXT("Then the select"), Problems[1].FieldId, FString(TEXT("category")));
	}
	return true;
}

/**
 * A studio's own form, with ids and questions this plugin has never seen. Anything hardcoded would show up here as a
 * form that renders the wrong questions or refuses the right answers.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormBuildsStudioFormTest,
	"Protokite.Playtest.Form.BuildsAStudioEditedFormWithItsOwnIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormBuildsStudioFormTest::RunTest(const FString& Parameters)
{
	FProtokitePlaytestForm Form;
	if (!ReadFormFromConfigBody(*this, StudioEditedConfigBody(), Form))
	{
		return false;
	}
	TestEqual(TEXT("Its title"), Form.Title, FString(TEXT("Tell us about the boss")));
	if (!TestEqual(TEXT("Its two questions"), Form.Fields.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("Its own select id"), Form.Fields[0].Id, FString(TEXT("boss_difficulty")));
	TestEqual(TEXT("With its own options"), Form.Fields[0].Options.Num(), 3);

	const TSharedRef<SProtokitePlaytestFormWidget> Widget = SNew(SProtokitePlaytestFormWidget).Form(Form);

	bool bSubmitted = false;
	FProtokitePlaytestFormAnswers Sent;
	const TSharedRef<SProtokitePlaytestFormWidget> Listening = SNew(SProtokitePlaytestFormWidget)
		.Form(Form)
		.OnSubmitted_Lambda([&bSubmitted, &Sent](const FProtokitePlaytestFormAnswers& Answers)
		{
			bSubmitted = true;
			Sent = Answers;
		});

	// An option this form never offered is refused, named.
	FProtokitePlaytestFormAnswers Answers;
	Answers.SetChosenOption(TEXT("boss_difficulty"), TEXT("Just right"));
	TArray<FProtokitePlaytestFormProblem> Problems = Answers.FindProblems(Form);
	if (TestEqual(TEXT("An option it never offered is refused"), Problems.Num(), 1))
	{
		TestEqual(TEXT("Named"), Problems[0].FieldId, FString(TEXT("boss_difficulty")));
	}

	// One of its own options, and the optional question left blank, is a form the server would take.
	Answers.SetChosenOption(TEXT("boss_difficulty"), TEXT("Too hard"));
	TestEqual(TEXT("Its own option is taken"), Answers.FindProblems(Form).Num(), 0);
	return true;
}

/** A playtest with no published form offers nothing to open, so a game leaves its feedback entry out. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormNoFormNoOpenTest,
	"Protokite.Playtest.Form.APlaytestWithNoFormOffersNothingToOpen",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormNoFormNoOpenTest::RunTest(const FString& Parameters)
{
	// A config with every other member and no form at all, which is what a playtest that published none sends.
	const FString NoFormBody = Envelope(FString::Printf(
		TEXT("{\"session_started_event\":\"session_started\",\"test_id\":\"%s\",\"flock_game_version_id\":\"%s\",")
		TEXT("\"features\":{},\"form\":null}"), TestId, GameVersionId));

	// The settings a ready playtest needs, set by the test itself: read from the project's own ini, this passes in a
	// project that happens to have playtesting on and fails in every other, which is what a studio's project is.
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	Fixture.AnswerConfig(FProtokitePlaytestFakeTransport::Status(200, NoFormBody));
	Fixture.StartFlock();

	TestEqual(TEXT("Precondition: the playtest is otherwise ready"),
		static_cast<int32>(Fixture.Playtest->GetStatus()), static_cast<int32>(EProtokitePlaytestStatus::Ready));
	TestFalse(TEXT("There is nothing to open"), Fixture.Playtest->CanOpenFeedbackForm());
	TestFalse(TEXT("And opening it does nothing"), Fixture.Playtest->OpenFeedbackForm());
	TestFalse(TEXT("So none is open"), Fixture.Playtest->IsFeedbackFormOpen());
	return true;
}

/** The counter-case for the one above: the same playtest with a form does offer one. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormWithAFormOffersOneTest,
	"Protokite.Playtest.Form.APlaytestWithAFormOffersOne",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormWithAFormOffersOneTest::RunTest(const FString& Parameters)
{
	// The settings a ready playtest needs, set by the test itself: read from the project's own ini, this passes in a
	// project that happens to have playtesting on and fails in every other, which is what a studio's project is.
	FScopedPlaytestSettings Settings(true, UsableUrl);
	FPlaytestFixture Fixture;
	Fixture.StartFlock();

	TestEqual(TEXT("Precondition: ready"),
		static_cast<int32>(Fixture.Playtest->GetStatus()), static_cast<int32>(EProtokitePlaytestStatus::Ready));
	// Without this, "no form offers nothing" would pass just as well against a build that never offers anything.
	TestTrue(TEXT("The default form is there to open"), Fixture.Playtest->CanOpenFeedbackForm());
	return true;
}

/**
 * The form carries the Qwacks icon beside its title. It is read from the plugin's own Resources folder, so renaming or
 * moving the file would quietly drop it from every form; this names the file the form looks for.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormShowsTheIconTest,
	"Protokite.Playtest.Form.ShowsTheQwacksIcon",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormShowsTheIconTest::RunTest(const FString& Parameters)
{
	const FString IconPath = SProtokitePlaytestFormWidget::GetIconPath();
	TestTrue(TEXT("The icon is in the plugin's Resources folder"), IconPath.EndsWith(TEXT("Resources/FeedbackFormIcon.png")));
	TestTrue(TEXT("And on disk"), FPaths::FileExists(IconPath));

	FProtokitePlaytestForm Form;
	Form.Id = TEXT("form-1");
	const TSharedRef<SProtokitePlaytestFormWidget> Widget = SNew(SProtokitePlaytestFormWidget).Form(Form);
	const FSlateBrush* Icon = Widget->GetIconBrushForTesting();
	if (TestNotNull(TEXT("The form shows it"), Icon))
	{
		// A brush that loads its own file. A plain image brush draws only what a registered style set loaded, and drew
		// a white square here while every check above passed.
		TestTrue(TEXT("Loading the file itself when drawn"), Icon->IsDynamicallyLoaded());
		TestEqual(TEXT("From the file on disk"), Icon->GetResourceName().ToString(), IconPath);
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
