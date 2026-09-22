// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "ProtokitePlaytestFormAnswers.h"

FProtokitePlaytestFormAnswer& FProtokitePlaytestFormAnswers::FindOrAdd(const FString& FieldId)
{
	FProtokitePlaytestFormAnswer& Answer = Answers.FindOrAdd(FieldId);
	Answer.Answered = true;
	return Answer;
}

void FProtokitePlaytestFormAnswers::SetText(const FString& FieldId, const FString& Text)
{
	FindOrAdd(FieldId).Text = Text;
}

void FProtokitePlaytestFormAnswers::SetRating(const FString& FieldId, int32 Rating)
{
	FindOrAdd(FieldId).Rating = Rating;
}

void FProtokitePlaytestFormAnswers::SetChecked(const FString& FieldId, bool bChecked)
{
	// Recording false is an answer: the server counts a checkbox as present whichever way it is set, so a form that
	// records every checkbox as it is built answers its required ones exactly as the server expects.
	FindOrAdd(FieldId).Checked = bChecked;
}

void FProtokitePlaytestFormAnswers::SetChosenOption(const FString& FieldId, const FString& Option)
{
	FindOrAdd(FieldId).Text = Option;
}

FString FProtokitePlaytestFormAnswers::GetText(const FString& FieldId) const
{
	const FProtokitePlaytestFormAnswer* Answer = Answers.Find(FieldId);
	return Answer != nullptr ? Answer->Text : FString();
}

int32 FProtokitePlaytestFormAnswers::GetRating(const FString& FieldId) const
{
	const FProtokitePlaytestFormAnswer* Answer = Answers.Find(FieldId);
	return Answer != nullptr ? Answer->Rating : 0;
}

bool FProtokitePlaytestFormAnswers::IsChecked(const FString& FieldId) const
{
	const FProtokitePlaytestFormAnswer* Answer = Answers.Find(FieldId);
	return Answer != nullptr && Answer->Checked;
}

bool FProtokitePlaytestFormAnswers::IsAnswered(const FString& FieldId) const
{
	const FProtokitePlaytestFormAnswer* Answer = Answers.Find(FieldId);
	return Answer != nullptr && Answer->Answered;
}

bool FProtokitePlaytestFormAnswers::IsEmptyAnswer(const FProtokitePlaytestFormField& Field, const FProtokitePlaytestFormAnswer& Answer)
{
	if (!Answer.Answered)
	{
		return true;
	}
	if (Field.IsOfKind(ProtokitePlaytestFormFieldTypes::Checkbox))
	{
		// Never empty once recorded, ticked or not: the server reads a checkbox as present whichever way it is set.
		return false;
	}
	if (Field.IsOfKind(ProtokitePlaytestFormFieldTypes::Rating))
	{
		// No rating chosen. One that was chosen but is out of range is a different complaint, made below.
		return Answer.Rating == 0;
	}
	// Text, textarea, select, and every kind this plugin does not know, which the server also reads as text.
	return Answer.Text.TrimStartAndEnd().IsEmpty();
}

FString FProtokitePlaytestFormAnswers::DescribeQuestionsItCannotTellApart(const FProtokitePlaytestForm& Form)
{
	TArray<FString> Pairs;
	for (int32 First = 0; First < Form.Fields.Num(); ++First)
	{
		for (int32 Second = First + 1; Second < Form.Fields.Num(); ++Second)
		{
			const FString& A = Form.Fields[First].Id;
			const FString& B = Form.Fields[Second].Id;
			if (A.Equals(B, ESearchCase::IgnoreCase) && !A.Equals(B, ESearchCase::CaseSensitive))
			{
				Pairs.Add(FString::Printf(TEXT("'%s' and '%s'"), *A, *B));
			}
		}
	}
	return FString::Join(Pairs, TEXT(", "));
}

TArray<FProtokitePlaytestFormProblem> FProtokitePlaytestFormAnswers::FindProblems(const FProtokitePlaytestForm& Form) const
{
	TArray<FProtokitePlaytestFormProblem> Problems;

	// The form's own questions, in order -- never the answers held here. An answer whose question the form no longer
	// has is dropped by the server rather than refused, so it is not something to stop a player over.
	for (const FProtokitePlaytestFormField& Field : Form.Fields)
	{
		const FProtokitePlaytestFormAnswer* Held = Answers.Find(Field.Id);
		const FProtokitePlaytestFormAnswer Answer = Held != nullptr ? *Held : FProtokitePlaytestFormAnswer();
		const bool bEmpty = IsEmptyAnswer(Field, Answer);

		if (Field.Required && bEmpty)
		{
			FProtokitePlaytestFormProblem Problem;
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

		if (Field.IsOfKind(ProtokitePlaytestFormFieldTypes::Rating))
		{
			if (Answer.Rating < ProtokitePlaytestRatings::Lowest || Answer.Rating > ProtokitePlaytestRatings::Highest)
			{
				FProtokitePlaytestFormProblem Problem;
				Problem.FieldId = Field.Id;
				Problem.Message = FString::Printf(TEXT("Choose a rating from %d to %d."), ProtokitePlaytestRatings::Lowest, ProtokitePlaytestRatings::Highest);
				Problems.Add(Problem);
			}
			continue;
		}

		if (Field.IsOfKind(ProtokitePlaytestFormFieldTypes::Select))
		{
			// Compared trimmed and letter for letter, the way the server compares it. TArray::Contains would ignore
			// letter case, and "crash" for the option "Crash" would pass here and be refused there.
			const FString Chosen = Answer.Text.TrimStartAndEnd();
			if (!Field.Options.ContainsByPredicate([&Chosen](const FString& Option)
			{
				return Option.Equals(Chosen, ESearchCase::CaseSensitive);
			}))
			{
				FProtokitePlaytestFormProblem Problem;
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

TSharedRef<FJsonObject> FProtokitePlaytestFormAnswers::ToWireObject(const FProtokitePlaytestForm& Form) const
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();

	// The form's own questions, in order, the way the server reads them. An answer to a question the form no longer
	// asks is dropped here rather than sent to be dropped there.
	for (const FProtokitePlaytestFormField& Field : Form.Fields)
	{
		const FProtokitePlaytestFormAnswer* Held = Answers.Find(Field.Id);
		if (Held == nullptr || IsEmptyAnswer(Field, *Held))
		{
			// Left out, never sent empty: the server stores nothing for an unanswered optional question, so sending an
			// empty string would record an answer the player never gave.
			continue;
		}

		if (Field.IsOfKind(ProtokitePlaytestFormFieldTypes::Rating))
		{
			Object->SetNumberField(Field.Id, Held->Rating);
		}
		else if (Field.IsOfKind(ProtokitePlaytestFormFieldTypes::Checkbox))
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
