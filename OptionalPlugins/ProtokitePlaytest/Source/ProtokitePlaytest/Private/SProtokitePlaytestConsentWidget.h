// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "ProtokitePlaytestConsent.h"
#include "Widgets/SCompoundWidget.h"

/** One answer the player can give, as it is offered to them. */
struct FProtokitePlaytestConsentOption
{
	EProtokitePlaytestConsentChoice Choice;

	/** The button's own words. */
	const TCHAR* Title;

	/** What that answer lets the playtest collect, under the button. */
	const TCHAR* Explanation;
};

/**
 * The playtest's consent question, drawn over the game at launch.
 *
 * **It asks about the playtest and nothing else**, and says so: a game that asks its players about privacy or
 * analytics asks that separately, in its own words, and neither answer moves the other. The four answers are the two
 * things a playtest collects, either of them on its own, and nothing at all.
 *
 * Every word shown here lives in this file rather than in the playtest config: what a build collects is decided by the
 * build, so what it promises a player must be readable in the build too, not changed from a dashboard afterwards.
 */
class SProtokitePlaytestConsentWidget : public SCompoundWidget
{
public:
	DECLARE_DELEGATE_OneParam(FOnConsentChosen, EProtokitePlaytestConsentChoice);

	SLATE_BEGIN_ARGS(SProtokitePlaytestConsentWidget) {}
		/** Called with the answer the player gave. */
		SLATE_EVENT(FOnConsentChosen, OnChosen)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Takes keyboard focus, so the question can be answered with a pad as well as a mouse. Same reason as the form's. */
	virtual bool SupportsKeyboardFocus() const override { return true; }

	/** The answers offered, in the order they are shown. One per choice a player can give; never NotAnswered. */
	static TArrayView<const FProtokitePlaytestConsentOption> GetOptions();

	/** Answers as pressing that option's button does. */
	void ChooseForTesting(EProtokitePlaytestConsentChoice Choice) { Choose(Choice); }

private:
	/** Hands the answer over. The one place an answer leaves this widget. */
	FReply Choose(EProtokitePlaytestConsentChoice Choice);

	TSharedRef<SWidget> BuildOption(const FProtokitePlaytestConsentOption& Option);

	FOnConsentChosen OnChosen;
};
