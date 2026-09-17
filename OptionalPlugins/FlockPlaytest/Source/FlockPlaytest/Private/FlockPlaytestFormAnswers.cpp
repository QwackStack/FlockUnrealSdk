// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockPlaytestFormAnswers.h"

namespace
{
	/** The lowest and highest a rating may be, as the server reads them. */
	constexpr int32 LowestRating = 1;
	constexpr int32 HighestRating = 5;

	bool IsKind(const FFlockPlaytestFormField& Field, const TCHAR* Kind)
	{
		return Field.Type.Equals(Kind, ESearchCase::CaseSensitive);
	}
}

FFlockPlaytestFormAnswer& FFlockPlaytestFormAnswers::FindOrAdd(const FString& FieldId)
{
	FFlockPlaytestFormAnswer& Answer = Answers.FindOrAdd(FieldId);
	Answer.Answered = true;
	return Answer;
}

void FFlockPlaytestFormAnswers::SetText(const FString& FieldId, const FString& Text)
{
	FindOrAdd(FieldId).Text = Text;
}

void FFlockPlaytestFormAnswers::SetRating(const FString& FieldId, int32 Rating)
{
	FindOrAdd(FieldId).Rating = Rating;
}

void FFlockPlaytestFormAnswers::SetChecked(const FString& FieldId, bool bChecked)
{
	// Recording false is an answer: the server counts a checkbox as present whichever way it is set, so a form that
	// records every checkbox as it is built answers its required ones exactly as the server expects.
	FindOrAdd(FieldId).Checked = bChecked;
}

void FFlockPlaytestFormAnswers::SetChosenOption(const FString& FieldId, const FString& Option)
{
	FindOrAdd(FieldId).Text = Option;
}

FString FFlockPlaytestFormAnswers::GetText(const FString& FieldId) const
{
	const FFlockPlaytestFormAnswer* Answer = Answers.Find(FieldId);
	return Answer != nullptr ? Answer->Text : FString();
}

int32 FFlockPlaytestFormAnswers::GetRating(const FString& FieldId) const
{
	const FFlockPlaytestFormAnswer* Answer = Answers.Find(FieldId);
	return Answer != nullptr ? Answer->Rating : 0;
}

bool FFlockPlaytestFormAnswers::IsChecked(const FString& FieldId) const
{
	const FFlockPlaytestFormAnswer* Answer = Answers.Find(FieldId);
	return Answer != nullptr && Answer->Checked;
}

bool FFlockPlaytestFormAnswers::IsAnswered(const FString& FieldId) const
{
	const FFlockPlaytestFormAnswer* Answer = Answers.Find(FieldId);
	return Answer != nullptr && Answer->Answered;
}

bool FFlockPlaytestFormAnswers::IsEmptyAnswer(const FFlockPlaytestFormField& Field, const FFlockPlaytestFormAnswer& Answer)
{
	if (!Answer.Answered)
	{
		return true;
	}
	if (IsKind(Field, FlockPlaytestFormFieldTypes::Checkbox))
	{
		// Never empty once recorded, ticked or not: the server reads a checkbox as present whichever way it is set.
		return false;
	}
	if (IsKind(Field, FlockPlaytestFormFieldTypes::Rating))
	{
		// No rating chosen. One that was chosen but is out of range is a different complaint, made below.
		return Answer.Rating == 0;
	}
	// Text, textarea, select, and every kind this plugin does not know, which the server also reads as text.
	return Answer.Text.TrimStartAndEnd().IsEmpty();
}

TArray<FFlockPlaytestFormProblem> FFlockPlaytestFormAnswers::FindProblems(const FFlockPlaytestForm& Form) const
{
	TArray<FFlockPlaytestFormProblem> Problems;

	// The form's own questions, in order -- never the answers held here. An answer whose question the form no longer
	// has is dropped by the server rather than refused, so it is not something to stop a player over.
	for (const FFlockPlaytestFormField& Field : Form.Fields)
	{
		const FFlockPlaytestFormAnswer* Held = Answers.Find(Field.Id);
		const FFlockPlaytestFormAnswer Answer = Held != nullptr ? *Held : FFlockPlaytestFormAnswer();
		const bool bEmpty = IsEmptyAnswer(Field, Answer);

		if (Field.Required && bEmpty)
		{
			FFlockPlaytestFormProblem Problem;
			Problem.FieldId = Field.Id;
			Problem.Message = TEXT("This one is needed.");
			Problems.Add(Problem);
			continue;
		}
		if (bEmpty)
		{
			// Left blank and not needed: the server leaves it out of what it stores, so there is nothing to check.
			continue;
		}

		if (IsKind(Field, FlockPlaytestFormFieldTypes::Rating))
		{
			if (Answer.Rating < LowestRating || Answer.Rating > HighestRating)
			{
				FFlockPlaytestFormProblem Problem;
				Problem.FieldId = Field.Id;
				Problem.Message = FString::Printf(TEXT("Choose a rating from %d to %d."), LowestRating, HighestRating);
				Problems.Add(Problem);
			}
			continue;
		}

		if (IsKind(Field, FlockPlaytestFormFieldTypes::Select))
		{
			// Compared trimmed, the way the server compares it.
			const FString Chosen = Answer.Text.TrimStartAndEnd();
			if (!Field.Options.Contains(Chosen))
			{
				FFlockPlaytestFormProblem Problem;
				Problem.FieldId = Field.Id;
				Problem.Message = TEXT("Choose one of the options given.");
				Problems.Add(Problem);
			}
			continue;
		}

		// A checkbox cannot be wrong, and text of any length is taken as it is.
	}

	return Problems;
}

TSharedRef<FJsonObject> FFlockPlaytestFormAnswers::ToWireObject(const FFlockPlaytestForm& Form) const
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();

	// The form's own questions, in order, the way the server reads them. An answer to a question the form no longer
	// asks is dropped here rather than sent to be dropped there.
	for (const FFlockPlaytestFormField& Field : Form.Fields)
	{
		const FFlockPlaytestFormAnswer* Held = Answers.Find(Field.Id);
		if (Held == nullptr || IsEmptyAnswer(Field, *Held))
		{
			// Left out, never sent empty: the server stores nothing for an unanswered optional question, so sending an
			// empty string would record an answer the player never gave.
			continue;
		}

		if (IsKind(Field, FlockPlaytestFormFieldTypes::Rating))
		{
			Object->SetNumberField(Field.Id, Held->Rating);
		}
		else if (IsKind(Field, FlockPlaytestFormFieldTypes::Checkbox))
		{
			Object->SetBoolField(Field.Id, Held->Checked);
		}
		else
		{
			// Text, textarea, select, and any kind this plugin does not know -- all text, trimmed, as the server keeps them.
			Object->SetStringField(Field.Id, Held->Text.TrimStartAndEnd());
		}
	}

	return Object;
}
