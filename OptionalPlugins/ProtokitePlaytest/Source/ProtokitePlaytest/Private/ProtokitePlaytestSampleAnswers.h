// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "ProtokitePlaytestConfig.h"
#include "ProtokitePlaytestFormAnswers.h"

/**
 * An answer of the right shape for every question on a form, so the server takes it exactly as it takes a player's: the
 * highest rating, ticked for a checkbox, a select's own first option, and Text for everything else. For the development
 * tools that send a form without a player: the test-feedback console command and the self-test.
 */
inline FProtokitePlaytestFormAnswers ProtokitePlaytestAnswerEveryQuestion(const FProtokitePlaytestForm& Form, const FString& Text)
{
	FProtokitePlaytestFormAnswers Answers;
	for (const FProtokitePlaytestFormField& Field : Form.Fields)
	{
		if (Field.IsOfKind(ProtokitePlaytestFormFieldTypes::Rating))
		{
			Answers.SetRating(Field.Id, ProtokitePlaytestRatings::Highest);
		}
		else if (Field.IsOfKind(ProtokitePlaytestFormFieldTypes::Checkbox))
		{
			Answers.SetChecked(Field.Id, true);
		}
		else if (Field.IsOfKind(ProtokitePlaytestFormFieldTypes::Select))
		{
			// Its own first option, never a guess: any other value is one the server refuses.
			Answers.SetChosenOption(Field.Id, Field.Options.Num() > 0 ? Field.Options[0] : FString());
		}
		else
		{
			Answers.SetText(Field.Id, Text);
		}
	}
	return Answers;
}
