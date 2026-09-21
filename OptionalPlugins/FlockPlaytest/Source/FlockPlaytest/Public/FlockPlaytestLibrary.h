// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockPlaytestConfig.h"
#include "FlockPlaytestConsent.h"
#include "FlockPlaytestFormAnswers.h"
#include "FlockPlaytestStatus.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Models/FlockCommandModels.h"
#include "FlockPlaytestLibrary.generated.h"

/**
 * Blueprint nodes for the playtest plugin. Each finds the playtest subsystem from the calling graph and does nothing
 * without one, so every node is safe in every build: with playtesting turned off they answer false, empty or Turned Off
 * and change nothing.
 *
 * The feature and question-kind names are nodes rather than text a graph types in, because the server owns those names
 * and a misspelt one reads as "off" rather than failing.
 */
UCLASS()
class FLOCKPLAYTEST_API UFlockPlaytestLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// ── Status ──

	/** Whether this build may do playtest work right now, and if not, why. Turned Off when there is no playtest subsystem. */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Get Playtest Status"))
	static EFlockPlaytestStatus GetPlaytestStatus(const UObject* WorldContextObject);

	/**
	 * True while playtesting is running: the settings are complete, the Flock SDK is initialized and this build's playtest
	 * is loaded. It does not mean a player is signed in or that the launch's session has started.
	 */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Is Playtest Ready"))
	static bool IsPlaytestReady(const UObject* WorldContextObject);

	/** One sentence saying what a status means, naming the setting to change when there is one. */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (DisplayName = "Flock Describe Playtest Status"))
	static FString DescribePlaytestStatus(EFlockPlaytestStatus Status);

	/**
	 * True only while playtesting is ready and the playtest turns this feature on. Feed it one of the Flock Playtest
	 * Feature nodes rather than typing the name: a feature the playtest does not mention, or a misspelt one, is off.
	 */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Is Playtest Feature Enabled"))
	static bool IsPlaytestFeatureEnabled(const UObject* WorldContextObject, const FString& FeatureName);

	/** The playtest feature that records the game's screen. */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (DisplayName = "Flock Playtest Feature Video Recording"))
	static FString PlaytestFeatureVideoRecording() { return FlockPlaytestFeatures::VideoRecording; }

	/** The playtest feature that asks for exceptions. The Flock SDK captures them, with its own settings. */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (DisplayName = "Flock Playtest Feature Exception Capturing"))
	static FString PlaytestFeatureExceptionCapturing() { return FlockPlaytestFeatures::ExceptionCapturing; }

	/** The playtest feature that sends performance windows, level loads and the game's own playtest events. */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (DisplayName = "Flock Playtest Feature Heavy Analytics"))
	static FString PlaytestFeatureHeavyAnalytics() { return FlockPlaytestFeatures::HeavyAnalytics; }

	// ── What the player let the playtest collect ──

	/**
	 * What this build collects under: the player's own answer when they have given one, everything in a build that does
	 * not ask, and nothing in one that does and has not been answered yet.
	 */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Get Playtest Consent"))
	static EFlockPlaytestConsentChoice GetPlaytestConsent(const UObject* WorldContextObject);

	/** What the player answered, and Not Answered when they have not been asked or have not answered. */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Get Players Consent Answer"))
	static EFlockPlaytestConsentChoice GetPlayersConsentAnswer(const UObject* WorldContextObject);

	/** One sentence saying what an answer lets the playtest collect, in the words the player was shown. */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (DisplayName = "Flock Describe Playtest Consent"))
	static FString DescribePlaytestConsent(EFlockPlaytestConsentChoice Choice);

	/**
	 * Records the player's answer, for a game that asks in its own screens. It is kept on this machine, used by every
	 * later launch, and takes effect at once. Not Answered forgets it, so the question is put again.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Set Playtest Consent"))
	static bool SetPlaytestConsent(const UObject* WorldContextObject, EFlockPlaytestConsentChoice Choice);

	/**
	 * Puts the playtest's consent question to the player now -- what a "change what this playtest collects" entry in a
	 * game's menu calls. It is asked by itself once the playtest is loaded, so this is for changing an answer. False
	 * when no playtest is loaded, the feedback form is open, or there is no viewport to draw it in.
	 *
	 * While it is up the player's input goes to the question and the game keeps running: pause first if asking during
	 * play would leave them unable to act.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Ask For Playtest Consent"))
	static bool AskForPlaytestConsent(const UObject* WorldContextObject);

	/** True while the playtest's consent question is on screen. */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Is Consent Question Open"))
	static bool IsConsentQuestionOpen(const UObject* WorldContextObject);

	// ── Session ──

	/** The id Protokite gave this launch's session. Empty until the session has started. */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Get Playtest Session Id"))
	static FString GetPlaytestSessionId(const UObject* WorldContextObject);

	/**
	 * Ends this launch's Protokite session now, for a game that quits on its own schedule. It also ends by itself when
	 * the game shuts down. Returns false, and sends nothing, when no session has started.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Flock End Playtest Session"))
	static bool EndPlaytestSession(const UObject* WorldContextObject);

	// ── Events ──

	/**
	 * Records one of the game's own events for this playtest, under the playtest category. Build Properties with the Set
	 * Command nodes. Recorded only while playtesting is ready and the playtest turns heavy analytics on. Returns false,
	 * and records nothing, otherwise; for a name the plugin sends itself (performance_window, level_loaded); and when the
	 * Flock SDK refuses the event (its analytics off, consent withheld, an empty name, or a name over 200 characters).
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject",
		AutoCreateRefTerm = "Properties", DisplayName = "Flock Record Playtest Event"))
	static bool RecordPlaytestEvent(const UObject* WorldContextObject, const FString& EventName, const FFlockCommandData& Properties);

	// ── Video recording ──

	/**
	 * Stops this launch's video recording for good and saves the file, for a game that quits on its own schedule. No
	 * other recording starts this launch. Returns false when no recording is capturing.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Stop Video Recording"))
	static bool StopVideoRecording(const UObject* WorldContextObject);

	/** True while a video recording is capturing the screen. */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Is Recording Video"))
	static bool IsRecordingVideo(const UObject* WorldContextObject);

	/**
	 * Whether this launch's recording has somewhere to go: one is running, it belongs to the playtest, and the launch's
	 * session has started. Ask this before offering a player a way to send it: a recording with no session cannot be
	 * uploaded at all.
	 */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Can Send Playtest Recording"))
	static bool CanSendPlaytestRecording(const UObject* WorldContextObject);

	/**
	 * Stops this launch's recording and uploads it straight away, while the player is still in the game. Returns false,
	 * and changes nothing, when no recording is capturing. The upload is not waited for; a recording that does not make
	 * it is kept and a later launch sends it.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Stop And Upload Playtest Recording"))
	static bool StopAndUploadPlaytestRecording(const UObject* WorldContextObject);

	// ── The feedback form ──

	/** Whether there is a feedback form to show: playtesting is ready and the playtest published one. */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Can Open Feedback Form"))
	static bool CanOpenFeedbackForm(const UObject* WorldContextObject);

	/** Opens the playtest's feedback form over the game. Returns false when there is none to show or it is already open. */
	UFUNCTION(BlueprintCallable, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Open Feedback Form"))
	static bool OpenFeedbackForm(const UObject* WorldContextObject);

	/** Closes the feedback form without sending it. Returns false when none is open. */
	UFUNCTION(BlueprintCallable, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Close Feedback Form"))
	static bool CloseFeedbackForm(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Is Feedback Form Open"))
	static bool IsFeedbackFormOpen(const UObject* WorldContextObject);

	/**
	 * The playtest's published feedback form, for a game that draws its own: its title, and each question's id, kind,
	 * label, help text, options and whether it needs an answer. Returns false, with an empty form, when there is none.
	 */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Get Feedback Form"))
	static bool GetFeedbackForm(const UObject* WorldContextObject, FFlockPlaytestForm& Form);

	/**
	 * Sends answers from a form the game drew itself, keeping them on disk when they cannot go now so a later launch
	 * sends them. Returns false, with a warning in the log, when the playtest has no form or there is nobody to attribute
	 * the answers to. Check them with Find Feedback Form Problems first.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Send Feedback Form Answers"))
	static bool SendFeedbackFormAnswers(const UObject* WorldContextObject, const FFlockPlaytestFormAnswers& Answers);

	/**
	 * Every question the server would turn these answers away over, in the form's own order. Empty means they would be
	 * taken. A needed checkbox counts as answered once it has been set, ticked or not.
	 */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (DisplayName = "Find Feedback Form Problems"))
	static TArray<FFlockPlaytestFormProblem> FindFeedbackFormProblems(const FFlockPlaytestFormAnswers& Answers, const FFlockPlaytestForm& Form);

	/** Records a text or text-area answer, and any kind of question this plugin does not know, which the server reads as text. */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (DisplayName = "Set Feedback Text Answer"))
	static FFlockPlaytestFormAnswers SetFeedbackTextAnswer(const FFlockPlaytestFormAnswers& Answers, const FString& FieldId, const FString& Text);

	/** Records a rating, 1 to 5. 0 clears it, which reads as unanswered. */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (DisplayName = "Set Feedback Rating Answer"))
	static FFlockPlaytestFormAnswers SetFeedbackRatingAnswer(const FFlockPlaytestFormAnswers& Answers, const FString& FieldId, int32 Rating);

	/** Records a checkbox, ticked or not. An unticked box is an answer, not the absence of one. */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (DisplayName = "Set Feedback Checkbox Answer"))
	static FFlockPlaytestFormAnswers SetFeedbackCheckboxAnswer(const FFlockPlaytestFormAnswers& Answers, const FString& FieldId, bool bChecked);

	/** Records the option a player picked for a select question. It must be one of the question's options, letter for letter. */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (DisplayName = "Set Feedback Chosen Option"))
	static FFlockPlaytestFormAnswers SetFeedbackChosenOption(const FFlockPlaytestFormAnswers& Answers, const FString& FieldId, const FString& Option);

	/** The question kind for a one-line text answer. */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (DisplayName = "Flock Feedback Question Kind Text"))
	static FString FeedbackQuestionKindText() { return FlockPlaytestFormFieldTypes::Text; }

	/** The question kind for a longer text answer. */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (DisplayName = "Flock Feedback Question Kind Text Area"))
	static FString FeedbackQuestionKindTextArea() { return FlockPlaytestFormFieldTypes::TextArea; }

	/** The question kind for a 1-to-5 rating. */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (DisplayName = "Flock Feedback Question Kind Rating"))
	static FString FeedbackQuestionKindRating() { return FlockPlaytestFormFieldTypes::Rating; }

	/** The question kind for picking one of a list of options. */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (DisplayName = "Flock Feedback Question Kind Select"))
	static FString FeedbackQuestionKindSelect() { return FlockPlaytestFormFieldTypes::Select; }

	/** The question kind for a tickbox. */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (DisplayName = "Flock Feedback Question Kind Checkbox"))
	static FString FeedbackQuestionKindCheckbox() { return FlockPlaytestFormFieldTypes::Checkbox; }
};
