// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "ProtokitePlaytestConfig.h"
#include "ProtokitePlaytestConsent.h"
#include "ProtokitePlaytestFormAnswers.h"
#include "ProtokitePlaytestStatus.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Models/FlockCommandModels.h"
#include "ProtokitePlaytestLibrary.generated.h"

/**
 * Blueprint nodes for the playtest plugin. Each finds the playtest subsystem from the calling graph and does nothing
 * without one, so every node is safe in every build: with playtesting turned off they answer false, empty or Turned Off
 * and change nothing.
 *
 * The feature and question-kind names are nodes rather than text a graph types in, because the server owns those names
 * and a misspelt one reads as "off" rather than failing.
 */
UCLASS()
class PROTOKITEPLAYTEST_API UProtokitePlaytestLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// ── Status ──

	/** Whether this build may do playtest work right now, and if not, why. Turned Off when there is no playtest subsystem. */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Protokite Get Playtest Status"))
	static EProtokitePlaytestStatus GetPlaytestStatus(const UObject* WorldContextObject);

	/**
	 * True while playtesting is running: the settings are complete, the Flock SDK is initialized and this build's playtest
	 * is loaded. It does not mean a player is signed in or that the launch's session has started.
	 */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Protokite Is Playtest Ready"))
	static bool IsPlaytestReady(const UObject* WorldContextObject);

	/** One sentence saying what a status means, naming the setting to change when there is one. */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (DisplayName = "Protokite Describe Playtest Status"))
	static FString DescribePlaytestStatus(EProtokitePlaytestStatus Status);

	/**
	 * True only while playtesting is ready and the playtest turns this feature on. Feed it one of the Protokite Playtest
	 * Feature nodes rather than typing the name: a feature the playtest does not mention, or a misspelt one, is off.
	 */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Protokite Is Playtest Feature Enabled"))
	static bool IsPlaytestFeatureEnabled(const UObject* WorldContextObject, const FString& FeatureName);

	/** The playtest feature that records the game's screen. */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (DisplayName = "Protokite Playtest Feature Video Recording"))
	static FString PlaytestFeatureVideoRecording() { return ProtokitePlaytestFeatures::VideoRecording; }

	/** The playtest feature that asks for exceptions. The Flock SDK captures them, with its own settings. */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (DisplayName = "Protokite Playtest Feature Exception Capturing"))
	static FString PlaytestFeatureExceptionCapturing() { return ProtokitePlaytestFeatures::ExceptionCapturing; }

	/** The playtest feature that sends performance windows, level loads and the game's own playtest events. */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (DisplayName = "Protokite Playtest Feature Heavy Analytics"))
	static FString PlaytestFeatureHeavyAnalytics() { return ProtokitePlaytestFeatures::HeavyAnalytics; }

	// ── What the player let the playtest collect ──

	/**
	 * What this build collects under: the player's own answer when they have given one, everything in a build that does
	 * not ask, and nothing in one that does and has not been answered yet.
	 */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Protokite Get Playtest Consent"))
	static EProtokitePlaytestConsentChoice GetPlaytestConsent(const UObject* WorldContextObject);

	/** What the player answered, and Not Answered when they have not been asked or have not answered. */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Protokite Get Players Consent Answer"))
	static EProtokitePlaytestConsentChoice GetPlayersConsentAnswer(const UObject* WorldContextObject);

	/** One sentence saying what an answer lets the playtest collect, in the words the player was shown. */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (DisplayName = "Protokite Describe Playtest Consent"))
	static FString DescribePlaytestConsent(EProtokitePlaytestConsentChoice Choice);

	/**
	 * Records the player's answer, for a game that asks in its own screens. It is kept on this machine, used by every
	 * later launch, and takes effect at once. Not Answered forgets it, so the question is put again.
	 */
	UFUNCTION(BlueprintCallable, Category = "Protokite|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Protokite Set Playtest Consent"))
	static bool SetPlaytestConsent(const UObject* WorldContextObject, EProtokitePlaytestConsentChoice Choice);

	/**
	 * Puts the playtest's consent question to the player now -- what a "change what this playtest collects" entry in a
	 * game's menu calls. It is asked by itself once the playtest is loaded, so this is for changing an answer. False
	 * when no playtest is loaded, the feedback form is open, or there is no viewport to draw it in.
	 *
	 * While it is up the player's input goes to the question and the game keeps running: pause first if asking during
	 * play would leave them unable to act.
	 */
	UFUNCTION(BlueprintCallable, Category = "Protokite|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Protokite Ask For Playtest Consent"))
	static bool AskForPlaytestConsent(const UObject* WorldContextObject);

	/** True while the playtest's consent question is on screen. */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Protokite Is Consent Question Open"))
	static bool IsConsentQuestionOpen(const UObject* WorldContextObject);

	// ── Session ──

	/** The id Protokite gave this launch's session. Empty until the session has started. */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Protokite Get Playtest Session Id"))
	static FString GetPlaytestSessionId(const UObject* WorldContextObject);

	/**
	 * Ends this launch's Protokite session now, for a game that quits on its own schedule. It also ends by itself when
	 * the game shuts down. Returns false, and sends nothing, when no session has started.
	 */
	UFUNCTION(BlueprintCallable, Category = "Protokite|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Protokite End Playtest Session"))
	static bool EndPlaytestSession(const UObject* WorldContextObject);

	// ── Events ──

	/**
	 * Records one of the game's own events for this playtest, under the playtest category. Build Properties with the Set
	 * Command nodes. Recorded only while playtesting is ready and the playtest turns heavy analytics on. Returns false,
	 * and records nothing, otherwise; for a name the plugin sends itself (performance_window, level_loaded); and when the
	 * Flock SDK refuses the event (its analytics off, consent withheld, an empty name, or a name over 200 characters).
	 */
	UFUNCTION(BlueprintCallable, Category = "Protokite|Playtest", meta = (WorldContext = "WorldContextObject",
		AutoCreateRefTerm = "Properties", DisplayName = "Protokite Record Playtest Event"))
	static bool RecordPlaytestEvent(const UObject* WorldContextObject, const FString& EventName, const FFlockCommandData& Properties);

	// ── Video recording ──

	/**
	 * Stops this launch's video recording for good and saves the file, for a game that quits on its own schedule. No
	 * other recording starts this launch. Returns false when no recording is capturing.
	 */
	UFUNCTION(BlueprintCallable, Category = "Protokite|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Protokite Stop Video Recording"))
	static bool StopVideoRecording(const UObject* WorldContextObject);

	/** True while a video recording is capturing the screen. */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Protokite Is Recording Video"))
	static bool IsRecordingVideo(const UObject* WorldContextObject);

	/**
	 * Whether this launch's recording has somewhere to go: one is running, it belongs to the playtest, and the launch's
	 * session has started. Ask this before offering a player a way to send it: a recording with no session cannot be
	 * uploaded at all.
	 */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Protokite Can Send Playtest Recording"))
	static bool CanSendPlaytestRecording(const UObject* WorldContextObject);

	/**
	 * Stops this launch's recording and uploads it straight away, while the player is still in the game. Returns false,
	 * and changes nothing, when no recording is capturing. The upload is not waited for; a recording that does not make
	 * it is kept and a later launch sends it.
	 */
	UFUNCTION(BlueprintCallable, Category = "Protokite|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Protokite Stop And Upload Playtest Recording"))
	static bool StopAndUploadPlaytestRecording(const UObject* WorldContextObject);

	// ── The feedback form ──

	/** Whether there is a feedback form to show: playtesting is ready and the playtest published one. */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Protokite Can Open Feedback Form"))
	static bool CanOpenFeedbackForm(const UObject* WorldContextObject);

	/** Opens the playtest's feedback form over the game. Returns false when there is none to show or it is already open. */
	UFUNCTION(BlueprintCallable, Category = "Protokite|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Protokite Open Feedback Form"))
	static bool OpenFeedbackForm(const UObject* WorldContextObject);

	/** Closes the feedback form without sending it. Returns false when none is open. */
	UFUNCTION(BlueprintCallable, Category = "Protokite|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Protokite Close Feedback Form"))
	static bool CloseFeedbackForm(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Protokite Is Feedback Form Open"))
	static bool IsFeedbackFormOpen(const UObject* WorldContextObject);

	/**
	 * The playtest's published feedback form, for a game that draws its own: its title, and each question's id, kind,
	 * label, help text, options and whether it needs an answer. Returns false, with an empty form, when there is none.
	 */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Protokite Get Feedback Form"))
	static bool GetFeedbackForm(const UObject* WorldContextObject, FProtokitePlaytestForm& Form);

	/**
	 * Sends answers from a form the game drew itself, keeping them on disk when they cannot go now so a later launch
	 * sends them. Returns false, with a warning in the log, when the playtest has no form or there is nobody to attribute
	 * the answers to. Check them with Find Feedback Form Problems first.
	 */
	UFUNCTION(BlueprintCallable, Category = "Protokite|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Protokite Send Feedback Form Answers"))
	static bool SendFeedbackFormAnswers(const UObject* WorldContextObject, const FProtokitePlaytestFormAnswers& Answers);

	/**
	 * Every question the server would turn these answers away over, in the form's own order. Empty means they would be
	 * taken. A needed checkbox counts as answered once it has been set, ticked or not.
	 */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (DisplayName = "Find Feedback Form Problems"))
	static TArray<FProtokitePlaytestFormProblem> FindFeedbackFormProblems(const FProtokitePlaytestFormAnswers& Answers, const FProtokitePlaytestForm& Form);

	/** Records a text or text-area answer, and any kind of question this plugin does not know, which the server reads as text. */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (DisplayName = "Set Feedback Text Answer"))
	static FProtokitePlaytestFormAnswers SetFeedbackTextAnswer(const FProtokitePlaytestFormAnswers& Answers, const FString& FieldId, const FString& Text);

	/** Records a rating, 1 to 5. 0 clears it, which reads as unanswered. */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (DisplayName = "Set Feedback Rating Answer"))
	static FProtokitePlaytestFormAnswers SetFeedbackRatingAnswer(const FProtokitePlaytestFormAnswers& Answers, const FString& FieldId, int32 Rating);

	/** Records a checkbox, ticked or not. An unticked box is an answer, not the absence of one. */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (DisplayName = "Set Feedback Checkbox Answer"))
	static FProtokitePlaytestFormAnswers SetFeedbackCheckboxAnswer(const FProtokitePlaytestFormAnswers& Answers, const FString& FieldId, bool bChecked);

	/** Records the option a player picked for a select question. It must be one of the question's options, letter for letter. */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (DisplayName = "Set Feedback Chosen Option"))
	static FProtokitePlaytestFormAnswers SetFeedbackChosenOption(const FProtokitePlaytestFormAnswers& Answers, const FString& FieldId, const FString& Option);

	/** The question kind for a one-line text answer. */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (DisplayName = "Protokite Feedback Question Kind Text"))
	static FString FeedbackQuestionKindText() { return ProtokitePlaytestFormFieldTypes::Text; }

	/** The question kind for a longer text answer. */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (DisplayName = "Protokite Feedback Question Kind Text Area"))
	static FString FeedbackQuestionKindTextArea() { return ProtokitePlaytestFormFieldTypes::TextArea; }

	/** The question kind for a 1-to-5 rating. */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (DisplayName = "Protokite Feedback Question Kind Rating"))
	static FString FeedbackQuestionKindRating() { return ProtokitePlaytestFormFieldTypes::Rating; }

	/** The question kind for picking one of a list of options. */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (DisplayName = "Protokite Feedback Question Kind Select"))
	static FString FeedbackQuestionKindSelect() { return ProtokitePlaytestFormFieldTypes::Select; }

	/** The question kind for a tickbox. */
	UFUNCTION(BlueprintPure, Category = "Protokite|Playtest", meta = (DisplayName = "Protokite Feedback Question Kind Checkbox"))
	static FString FeedbackQuestionKindCheckbox() { return ProtokitePlaytestFormFieldTypes::Checkbox; }
};
