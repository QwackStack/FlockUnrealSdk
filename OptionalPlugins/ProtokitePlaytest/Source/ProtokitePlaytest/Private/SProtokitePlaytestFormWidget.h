// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "ProtokitePlaytestConfig.h"
#include "ProtokitePlaytestFormAnswers.h"
#include "Widgets/SCompoundWidget.h"

struct FSlateBrush;

class SVerticalBox;

/**
 * The playtest feedback form, built from whatever questions the studio published.
 *
 * **Nothing about the form is written down here.** Every question, its label, its help text, whether it is needed and a
 * select's options all come from the config, so a studio renaming a field or adding a question changes the form with no
 * new build. A kind this plugin does not know is drawn as a text box, which is how the server reads it too, so a form
 * using a newer kind is still answerable rather than half-blank.
 *
 * Problems are shown against the questions they belong to, and only after a submit is tried: a form that complains
 * while someone is still typing reads as broken.
 */
class SProtokitePlaytestFormWidget : public SCompoundWidget
{
public:
	DECLARE_DELEGATE_OneParam(FOnFormSubmitted, const FProtokitePlaytestFormAnswers&);
	DECLARE_DELEGATE_RetVal(bool, FCanSendRecording);
	DECLARE_DELEGATE_RetVal(bool, FOnSendRecording);

	SLATE_BEGIN_ARGS(SProtokitePlaytestFormWidget) {}
		/** The published form to draw. */
		SLATE_ARGUMENT(FProtokitePlaytestForm, Form)
		/** Called with the answers once they are ones the server would take. */
		SLATE_EVENT(FOnFormSubmitted, OnSubmitted)
		/** Called when the player closes the form without sending it. */
		SLATE_EVENT(FSimpleDelegate, OnClosed)
		/**
		 * Whether the recording has somewhere to go. Not merely whether one is running: a recording no playtest session
		 * started for cannot be uploaded at all, and offering it would stop the player's recording and send nothing.
		 */
		SLATE_EVENT(FCanSendRecording, CanSendRecording)
		/** Stops the recording and sends it. Answers whether sending actually began. */
		SLATE_EVENT(FOnSendRecording, OnSendRecording)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/**
	 * The form takes keyboard focus, which is what a UI-only input mode hands it and what a controller moves around
	 * inside. Without this the engine refuses the focus outright ("Attempting to focus Non-Focusable widget"), leaving a
	 * form a player can see, and click, but cannot type in with a pad -- which only a run with a real viewport shows.
	 */
	virtual bool SupportsKeyboardFocus() const override { return true; }

	/** What has been filled in so far. */
	const FProtokitePlaytestFormAnswers& GetAnswers() const { return Answers; }

	/** Checks the answers and, when there is nothing wrong, hands them over. Returns what was wrong, if anything. */
	TArray<FProtokitePlaytestFormProblem> TrySubmitForTesting() { return TrySubmit(); }

	EVisibility GetSendRecordingVisibilityForTesting() const { return GetSendRecordingVisibility(); }
	EVisibility GetRecordingOnItsWayVisibilityForTesting() const { return GetRecordingOnItsWayVisibility(); }
	void SendTheRecordingForTesting() { SendTheRecording(); }
	const FSlateBrush* GetIconBrushForTesting() const { return IconBrush.Get(); }

	/** The Qwacks icon drawn beside the form's title, inside the plugin's Resources folder. */
	static FString GetIconPath();

private:
	TArray<FProtokitePlaytestFormProblem> TrySubmit();

	/** One question, drawn as its own kind, with its label, its help text and somewhere to show what is wrong. */
	TSharedRef<SWidget> BuildField(const FProtokitePlaytestFormField& Field);
	TSharedRef<SWidget> BuildAnswerControl(const FProtokitePlaytestFormField& Field);
	TSharedRef<SWidget> BuildRating(const FProtokitePlaytestFormField& Field);
	TSharedRef<SWidget> BuildSelect(const FProtokitePlaytestFormField& Field);

	/** The problem to show under a question, empty when there is none. */
	FText GetProblemText(FString FieldId) const;
	EVisibility GetProblemVisibility(FString FieldId) const;

	FProtokitePlaytestForm Form;
	FProtokitePlaytestFormAnswers Answers;

	/** What was wrong the last time a submit was tried. Empty until then, which is why nothing complains early. */
	TArray<FProtokitePlaytestFormProblem> Problems;

	/**
	 * Kept alive for the select boxes, which hold a pointer to their list of options. Each list lives on its own on the
	 * heap: held by value in a container, a second select question could move the first one's list and leave its box
	 * pointing at freed memory.
	 */
	TArray<TSharedRef<TArray<TSharedPtr<FString>>>> SelectOptionLists;

	/** The icon beside the title. Unset when its file cannot be found, and the form then shows no icon rather than a gap. */
	TSharedPtr<FSlateBrush> IconBrush;

	/** The button offering to send the recording, and the note that replaces it once it has been asked for. */
	EVisibility GetSendRecordingVisibility() const;
	EVisibility GetRecordingOnItsWayVisibility() const;
	FReply SendTheRecording();

	FOnFormSubmitted OnSubmitted;
	FSimpleDelegate OnClosed;
	FCanSendRecording CanSendRecording;
	FOnSendRecording OnSendRecording;

	/** Set once the player has asked for the recording, so they are not offered it twice. */
	bool bAskedForTheRecording = false;

	/**
	 * Whether sending actually began. The note only appears when it did: telling a player their video is on its way
	 * when nothing was sent is worse than saying nothing, and that is exactly what this said before.
	 */
	bool bTheRecordingIsOnItsWay = false;
};
