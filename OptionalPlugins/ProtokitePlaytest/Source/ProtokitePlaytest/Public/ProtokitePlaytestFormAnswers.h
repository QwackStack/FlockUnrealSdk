// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "ProtokitePlaytestConfig.h"
#include "ProtokitePlaytestFormAnswers.generated.h"

/** Something wrong with an answer, named so the form can point at the question it belongs to. */
USTRUCT(BlueprintType)
struct PROTOKITEPLAYTEST_API FProtokitePlaytestFormProblem
{
	GENERATED_BODY()

	/** The field's id, for finding the question again. */
	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	FString FieldId;

	/** What to tell the player, in their own terms. */
	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	FString Message;
};

/** One question's answer. Which member counts is decided by the question's kind, never by the answer itself. */
USTRUCT(BlueprintType)
struct PROTOKITEPLAYTEST_API FProtokitePlaytestFormAnswer
{
	GENERATED_BODY()

	/** A text, textarea or select answer -- and any kind this plugin does not know, which the server reads as text. */
	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	FString Text;

	/** 1 to 5 for a rating. */
	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	int32 Rating = 0;

	/** A checkbox's state. */
	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	bool Checked = false;

	/** Whether anything was recorded for this question at all, which is not the same as it holding something. */
	UPROPERTY(BlueprintReadOnly, Category = "Protokite|Playtest")
	bool Answered = false;
};

/**
 * What a player has filled in, and whether the server would take it.
 *
 * Inert: no widget, no network, no clock. The form widget writes into it and the submission reads it, which is what
 * lets every rule below be tested without either.
 *
 * **The rules are the server's, read from its own validator rather than guessed** (`validate_answers`, measured
 * 2026-09-16). Three of them are not what you would assume:
 * - **A required checkbox is satisfied by an unticked box.** The server asks whether an answer is *present*, and
 *   `false` is present. Refusing an unticked box here would stop a submit the server would have taken.
 * - **An empty answer to an optional question is left out**, not sent as an empty string.
 * - **A question of a kind this plugin does not know is treated as text**, exactly as the server treats it, so a
 *   form using a newer kind still submits.
 *
 * It reports **every** problem rather than stopping at the first, which the server does. That is a deliberate
 * difference and a safe one: a player fixing one question at a time would otherwise submit repeatedly to be told
 * about the next, and reporting more problems never accepts something the server would refuse.
 */
USTRUCT(BlueprintType)
struct PROTOKITEPLAYTEST_API FProtokitePlaytestFormAnswers
{
	GENERATED_BODY()

	/** Records a text, textarea or unknown-kind answer. */
	void SetText(const FString& FieldId, const FString& Text);

	/** Records a rating. 0 clears it, which reads as unanswered. */
	void SetRating(const FString& FieldId, int32 Rating);

	/** Records a checkbox, ticked or not. Recording false is an answer, not the absence of one. */
	void SetChecked(const FString& FieldId, bool bChecked);

	/** Records the option a player picked for a select. */
	void SetChosenOption(const FString& FieldId, const FString& Option);

	FString GetText(const FString& FieldId) const;
	int32 GetRating(const FString& FieldId) const;
	bool IsChecked(const FString& FieldId) const;

	/** Whether anything at all was recorded for this question. */
	bool IsAnswered(const FString& FieldId) const;

	void Clear() { Answers.Reset(); }

	/**
	 * Every question the server would turn this form away over, in the order the studio arranged them. Empty means it
	 * would be taken.
	 *
	 * Questions not on the form are ignored rather than reported: the server reads the form's own fields and drops
	 * anything else, so an answer left over from a form that has since changed is not the player's problem.
	 */
	TArray<FProtokitePlaytestFormProblem> FindProblems(const FProtokitePlaytestForm& Form) const;

	/**
	 * The answers as the server stores them: a rating as a number, a tickbox as true or false, and everything else as
	 * trimmed text.
	 *
	 * **An empty answer to an optional question is left out rather than sent empty**, and a question the form does not
	 * ask is not sent at all -- both exactly what the server does with what it receives, so what is sent and what is
	 * kept cannot drift apart.
	 */
	TSharedRef<FJsonObject> ToWireObject(const FProtokitePlaytestForm& Form) const;

	/** Whether a question, read as its own kind, holds nothing -- the server's own emptiness test. */
	static bool IsEmptyAnswer(const FProtokitePlaytestFormField& Field, const FProtokitePlaytestFormAnswer& Answer);

	/**
	 * The questions whose ids differ only in letter case, as "'Q1' and 'q1'", joined; empty when there are none. The server
	 * keeps such questions apart and this plugin cannot -- answers are held and sent under an engine string key, which
	 * ignores case -- so their answers would land on one of them. Said once when a form loads, so the studio renames one.
	 */
	static FString DescribeQuestionsItCannotTellApart(const FProtokitePlaytestForm& Form);

private:
	FProtokitePlaytestFormAnswer& FindOrAdd(const FString& FieldId);

	UPROPERTY()
	TMap<FString, FProtokitePlaytestFormAnswer> Answers;
};
