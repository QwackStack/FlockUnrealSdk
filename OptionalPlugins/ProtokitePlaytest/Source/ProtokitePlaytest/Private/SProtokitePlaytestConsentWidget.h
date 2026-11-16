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
};

/** One part of what the "What's collected?" pop-up explains. */
struct FProtokitePlaytestCollectedPart
{
	const TCHAR* Title;
	const TCHAR* Explanation;
};

/**
 * The playtest's consent question, drawn over the game at launch: yes to everything, or not now (nothing), with a
 * "What's collected?" link that opens a pop-up over the question explaining what is collected.
 *
 * **It asks about the playtest and nothing else**, and its pop-up says so: a game that asks its players about privacy or
 * analytics asks that separately, in its own words, and neither answer moves the other.
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

	/** The answers offered, in the order they are shown; never NotAnswered. */
	static TArrayView<const FProtokitePlaytestConsentOption> GetOptions();

	/** What the pop-up explains, in order. */
	static TArrayView<const FProtokitePlaytestCollectedPart> GetWhatIsCollected();

	/** A press this soon after the question appears, or after the pop-up shuts, is not an answer: a player still clicking at the game, or clicking Back twice, never read it. */
	static constexpr double SecondsBeforeAnAnswerCounts = 0.5;

	/** The Qwacks logo drawn at the top of the question: a file in the plugin's Resources folder, packaged with the game. */
	static FString GetQwacksLogoPath();

	/** The logo's brush, or null when its file is missing. */
	const FSlateBrush* GetQwacksLogoBrushForTesting() const { return QwacksLogoBrush.Get(); }

	/** Whether the "What's collected?" pop-up is over the question. */
	bool IsWhatIsCollectedOpen() const { return bWhatIsCollectedOpen; }

	/** Presses that answer's button, as the player does; the reply says where focus goes. */
	FReply PressForTesting(EProtokitePlaytestConsentChoice Choice) { return Press(Choice); }

	/** Presses the "What's collected?" link. */
	void OpenWhatIsCollectedForTesting() { OpenWhatIsCollected(); }

	/** Presses the pop-up's Back button; the reply says where focus goes. */
	FReply BackForTesting() { return Back(); }

	/** Lets presses count at once, as if the player had read the question for long enough. */
	void LetAnswersCountNowForTesting() { AnswersCountFrom = 0.0; }

private:
	/** A button press: an answer only once it can have been read and while the pop-up is shut; a refused press takes focus off its button, so a Submit key cannot press it. */
	FReply Press(EProtokitePlaytestConsentChoice Choice);

	FReply OpenWhatIsCollected();
	FReply Back();

	TSharedRef<SWidget> BuildOption(const FProtokitePlaytestConsentOption& Option, bool bOfferedFirst);
	TSharedRef<SWidget> BuildWhatIsCollected();

	FOnConsentChosen OnChosen;
	TSharedPtr<FSlateBrush> QwacksLogoBrush;
	double AnswersCountFrom = 0.0;
	bool bWhatIsCollectedOpen = false;
};
