// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "ProtokitePlaytestFormAnswers.h"

namespace
{
	FProtokitePlaytestFormField MakeField(const FString& Id, const TCHAR* Type, bool bRequired = false,
		const TArray<FString>& Options = {})
	{
		FProtokitePlaytestFormField Field;
		Field.Id = Id;
		Field.Type = Type;
		Field.Label = Id;
		Field.Required = bRequired;
		Field.Options = Options;
		return Field;
	}

	FProtokitePlaytestForm MakeForm(const TArray<FProtokitePlaytestFormField>& Fields)
	{
		FProtokitePlaytestForm Form;
		Form.Id = TEXT("form-1");
		Form.Fields = Fields;
		return Form;
	}

	bool HasProblemFor(const TArray<FProtokitePlaytestFormProblem>& Problems, const FString& FieldId)
	{
		return Problems.ContainsByPredicate([&](const FProtokitePlaytestFormProblem& P) { return P.FieldId == FieldId; });
	}
}

/** A required question left blank is refused, and the problem names the question so the form can point at it. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormRequiredTest,
	"Protokite.Playtest.Form.RequiredAnswersAreNeeded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormRequiredTest::RunTest(const FString& Parameters)
{
	const FProtokitePlaytestForm Form = MakeForm({
		MakeField(TEXT("what_happened"), ProtokitePlaytestFormFieldTypes::TextArea, /*bRequired*/ true),
		MakeField(TEXT("anything_else"), ProtokitePlaytestFormFieldTypes::Text, /*bRequired*/ false),
	});

	FProtokitePlaytestFormAnswers Answers;
	TArray<FProtokitePlaytestFormProblem> Problems = Answers.FindProblems(Form);
	if (TestEqual(TEXT("The required one is missing"), Problems.Num(), 1))
	{
		TestEqual(TEXT("And it is named"), Problems[0].FieldId, FString(TEXT("what_happened")));
	}

	// Whitespace is not an answer -- the server trims before deciding.
	Answers.SetText(TEXT("what_happened"), TEXT("   \t  "));
	TestEqual(TEXT("Whitespace does not answer it"), Answers.FindProblems(Form).Num(), 1);

	Answers.SetText(TEXT("what_happened"), TEXT("The lift fell through the floor."));
	TestEqual(TEXT("Answered, and the optional one is still fine left blank"), Answers.FindProblems(Form).Num(), 0);
	return true;
}

/**
 * The rule that is not what you would assume, taken from the server's own validator: an answer is required to be
 * *present*, and an unticked checkbox is present. Refusing it here would stop a submit the server would take.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormRequiredCheckboxTest,
	"Protokite.Playtest.Form.ARequiredCheckboxIsAnsweredByAnUntickedBox",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormRequiredCheckboxTest::RunTest(const FString& Parameters)
{
	const FProtokitePlaytestForm Form = MakeForm({
		MakeField(TEXT("may_contact_me"), ProtokitePlaytestFormFieldTypes::Checkbox, /*bRequired*/ true),
	});

	FProtokitePlaytestFormAnswers Answers;
	TestEqual(TEXT("Never touched, it is missing"), Answers.FindProblems(Form).Num(), 1);

	Answers.SetChecked(TEXT("may_contact_me"), false);
	TestEqual(TEXT("Recorded as unticked, the server would take it"), Answers.FindProblems(Form).Num(), 0);

	Answers.SetChecked(TEXT("may_contact_me"), true);
	TestEqual(TEXT("And ticked, of course"), Answers.FindProblems(Form).Num(), 0);
	return true;
}

/** A rating is 1 to 5, and nothing else -- the same range the server keeps. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormRatingRangeTest,
	"Protokite.Playtest.Form.ARatingRunsFromOneToFive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormRatingRangeTest::RunTest(const FString& Parameters)
{
	const FProtokitePlaytestForm Form = MakeForm({
		MakeField(TEXT("how_was_it"), ProtokitePlaytestFormFieldTypes::Rating, /*bRequired*/ false),
	});

	FProtokitePlaytestFormAnswers Answers;
	TestEqual(TEXT("Unrated and optional is fine"), Answers.FindProblems(Form).Num(), 0);

	for (const int32 Good : {1, 3, 5})
	{
		Answers.SetRating(TEXT("how_was_it"), Good);
		TestEqual(FString::Printf(TEXT("%d is a rating"), Good), Answers.FindProblems(Form).Num(), 0);
	}
	for (const int32 Bad : {-1, 6, 99})
	{
		Answers.SetRating(TEXT("how_was_it"), Bad);
		TestEqual(FString::Printf(TEXT("%d is not"), Bad), Answers.FindProblems(Form).Num(), 1);
	}
	return true;
}

/** A select takes one of the options it was given, compared trimmed, the way the server compares it. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormSelectTest,
	"Protokite.Playtest.Form.ASelectTakesOnlyItsOwnOptions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormSelectTest::RunTest(const FString& Parameters)
{
	const FProtokitePlaytestForm Form = MakeForm({
		MakeField(TEXT("which_level"), ProtokitePlaytestFormFieldTypes::Select, /*bRequired*/ true,
			{TEXT("The docks"), TEXT("The tower")}),
	});

	FProtokitePlaytestFormAnswers Answers;
	Answers.SetChosenOption(TEXT("which_level"), TEXT("The cellar"));
	if (TestEqual(TEXT("An option it was never offered is refused"), Answers.FindProblems(Form).Num(), 1))
	{
		TestEqual(TEXT("Named"), Answers.FindProblems(Form)[0].FieldId, FString(TEXT("which_level")));
	}

	Answers.SetChosenOption(TEXT("which_level"), TEXT("  The tower  "));
	TestEqual(TEXT("One of its own, trimmed, is taken"), Answers.FindProblems(Form).Num(), 0);

	// The server compares letter for letter. Taken here, this would be refused there and the answers dropped.
	Answers.SetChosenOption(TEXT("which_level"), TEXT("the tower"));
	TestEqual(TEXT("One of its own in other letter case is refused, as the server refuses it"), Answers.FindProblems(Form).Num(), 1);
	return true;
}

/**
 * The server keeps questions whose ids differ only in letter case apart; this plugin cannot, so it names them for the
 * studio to rename one. Ids that are simply different are not named.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormQuestionsItCannotTellApartTest,
	"Protokite.Playtest.Form.NamesQuestionsDifferingOnlyInLetterCase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormQuestionsItCannotTellApartTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Named, both spellings"), FProtokitePlaytestFormAnswers::DescribeQuestionsItCannotTellApart(MakeForm({
			MakeField(TEXT("Q1"), ProtokitePlaytestFormFieldTypes::Text),
			MakeField(TEXT("steps"), ProtokitePlaytestFormFieldTypes::TextArea),
			MakeField(TEXT("q1"), ProtokitePlaytestFormFieldTypes::Text),
		})), FString(TEXT("'Q1' and 'q1'")));
	TestTrue(TEXT("Different ids are not named"), FProtokitePlaytestFormAnswers::DescribeQuestionsItCannotTellApart(MakeForm({
			MakeField(TEXT("Q1"), ProtokitePlaytestFormFieldTypes::Text),
			MakeField(TEXT("Q2"), ProtokitePlaytestFormFieldTypes::Text),
		})).IsEmpty());
	return true;
}

/**
 * A question of a kind this plugin does not know still submits, because the server reads any unknown kind as text.
 * Treating it as unanswerable would make a form using a newer kind impossible to send.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormUnknownKindTest,
	"Protokite.Playtest.Form.AnUnknownKindIsAnsweredAsText",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormUnknownKindTest::RunTest(const FString& Parameters)
{
	const FProtokitePlaytestForm Form = MakeForm({
		MakeField(TEXT("mood"), TEXT("emoji_picker"), /*bRequired*/ true),
	});

	FProtokitePlaytestFormAnswers Answers;
	TestEqual(TEXT("Blank, it is still needed"), Answers.FindProblems(Form).Num(), 1);

	Answers.SetText(TEXT("mood"), TEXT("delighted"));
	TestEqual(TEXT("Answered as text, it passes"), Answers.FindProblems(Form).Num(), 0);
	return true;
}

/** An answer to a question the form no longer has is dropped by the server, so it must not stop a player here. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormStrayAnswerTest,
	"Protokite.Playtest.Form.AnAnswerTheFormNoLongerAsksForIsIgnored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormStrayAnswerTest::RunTest(const FString& Parameters)
{
	const FProtokitePlaytestForm Form = MakeForm({
		MakeField(TEXT("what_happened"), ProtokitePlaytestFormFieldTypes::Text, /*bRequired*/ true),
	});

	FProtokitePlaytestFormAnswers Answers;
	Answers.SetText(TEXT("what_happened"), TEXT("It went well."));
	// Left over from a form the studio has since edited.
	Answers.SetRating(TEXT("a_question_that_was_removed"), 99);

	TestEqual(TEXT("The stray answer is not the player's problem"), Answers.FindProblems(Form).Num(), 0);
	return true;
}

/** Every problem at once, so a player is not told about them one submit at a time. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormEveryProblemTest,
	"Protokite.Playtest.Form.ReportsEveryProblemAtOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormEveryProblemTest::RunTest(const FString& Parameters)
{
	const FProtokitePlaytestForm Form = MakeForm({
		MakeField(TEXT("one"), ProtokitePlaytestFormFieldTypes::Text, /*bRequired*/ true),
		MakeField(TEXT("two"), ProtokitePlaytestFormFieldTypes::Rating, /*bRequired*/ true),
		MakeField(TEXT("three"), ProtokitePlaytestFormFieldTypes::Select, /*bRequired*/ true, {TEXT("Yes")}),
	});

	FProtokitePlaytestFormAnswers Answers;
	Answers.SetRating(TEXT("two"), 9);
	Answers.SetChosenOption(TEXT("three"), TEXT("Maybe"));

	const TArray<FProtokitePlaytestFormProblem> Problems = Answers.FindProblems(Form);
	if (TestEqual(TEXT("All three are reported"), Problems.Num(), 3))
	{
		TestTrue(TEXT("The blank one"), HasProblemFor(Problems, TEXT("one")));
		TestTrue(TEXT("The out-of-range rating"), HasProblemFor(Problems, TEXT("two")));
		TestTrue(TEXT("The option it was never offered"), HasProblemFor(Problems, TEXT("three")));
		TestEqual(TEXT("In the order the studio arranged them"), Problems[0].FieldId, FString(TEXT("one")));
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
