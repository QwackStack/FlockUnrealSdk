// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "UObject/Object.h"
#include "ProtokitePlaytestSelfTest.generated.h"

class FProtokiteClient;
class IFlockHttpAdapter;
class UProtokitePlaytestSubsystem;
class UFlockSubsystem;
struct FFlockError;
struct FProtokitePlaytestFormSubmission;

/** How one self-test step came out. */
enum class EProtokitePlaytestSelfTestOutcome : uint8
{
	Passed,
	Failed,
	/** Not checked, and the step says why: a feature the playtest does not turn on, or a check that needs an argument. */
	Skipped,
};

/** One check the self-test made. */
struct FProtokitePlaytestSelfTestStep
{
	FString Name;
	EProtokitePlaytestSelfTestOutcome Outcome = EProtokitePlaytestSelfTestOutcome::Failed;
	FString Detail;
};

/** The self-test's steps, in the order they run. Each counter-case sits beside the step it proves. */
namespace ProtokitePlaytestSelfTestSteps
{
	inline constexpr const TCHAR* PlaytestLoaded = TEXT("The playtest config loads");
	inline constexpr const TCHAR* WrongApiKeyRefused = TEXT("A wrong API key is refused (401)");
	inline constexpr const TCHAR* MissingApiKeyRefused = TEXT("A missing API key is refused (422)");
	inline constexpr const TCHAR* VersionWithNoPlaytestRefused = TEXT("A version no playtest is linked to is refused (404)");
	inline constexpr const TCHAR* SessionStarted = TEXT("This launch's Protokite session starts");
	inline constexpr const TCHAR* SessionWithNoPlayerRefused = TEXT("A session start naming no player is refused (422)");
	inline constexpr const TCHAR* ClosedPlaytestRefused = TEXT("A session start for a closed playtest is refused (400)");
	inline constexpr const TCHAR* ExceptionReported = TEXT("An exception is queued once, its repeat counted, and it is sent");
	inline constexpr const TCHAR* PlaytestEventRecorded = TEXT("A playtest event is recorded and sent, and the plugin's own event name is refused");
	inline constexpr const TCHAR* FormMissingANeededAnswerRefused = TEXT("A form missing a needed answer is refused (422, naming it)");
	inline constexpr const TCHAR* FormWithAnOptionNotOnTheListRefused = TEXT("A form choosing an option not on the list is refused (422, naming it)");
	inline constexpr const TCHAR* FormForASessionThatDoesNotExistRefused = TEXT("A form naming a session that does not exist is refused (404)");
	inline constexpr const TCHAR* FormTaken = TEXT("A filled-in form is taken");
	inline constexpr const TCHAR* UploadLinkForASessionThatDoesNotExistRefused = TEXT("An upload link for a session that does not exist is refused (404)");
	inline constexpr const TCHAR* RecordingUploaded = TEXT("This launch's recording is uploaded");
	inline constexpr const TCHAR* EndForASessionThatDoesNotExistRefused = TEXT("An end for a session that does not exist is refused (404)");
	inline constexpr const TCHAR* SessionEnded = TEXT("This launch's Protokite session ends");
}

/** Hands the recording's upload-finished event to the self-test, since a Blueprint event binds only to a UFUNCTION. */
UCLASS()
class UProtokitePlaytestSelfTestListener : public UObject
{
	GENERATED_BODY()

public:
	TFunction<void(bool bUploaded, const FString& WhyNot)> OnUploadFinished;

	UFUNCTION()
	void HandleRecordingUploadFinished(bool bUploaded, const FString& WhyNot)
	{
		if (OnUploadFinished)
		{
			OnUploadFinished(bUploaded, WhyNot);
		}
	}
};

/**
 * The live playtest self-test: one run, against the real backend, of everything a playtest build does, each step with
 * its counter-case, ending on a count of what passed. Development builds only, started by ProtokitePlaytest.SelfTest.
 *
 * It uses the running game's own playtest for the real work -- its config, its session, its recording, its end -- and
 * a Protokite client of its own, which never retries, for every refusal it provokes. A refusal therefore never touches
 * the launch's own state, and a counter-case that should have been refused but created a session has that session ended
 * at once, so a run leaves no session open behind it. The filled-in form goes through that client too: the game's own
 * send keeps a form on disk and retries it, and tells its caller nothing about the server's answer.
 *
 * A refusal passes only when it is the one expected: a failure with that HTTP status and, where the server names a
 * question, that question. Protokite's refusals carry no code, so the status and the named question are what tell them
 * apart; anything else, a success included, fails the step.
 *
 * It signs nobody in, as a playtest never does: the session step waits for the game's own sign-in, and a harness signs in
 * first with Flock.LoginWithDevice. It ends this launch's Protokite session as its last step, so nothing of the playtest
 * that needs a session works for the rest of the launch.
 */
class FProtokitePlaytestSelfTest : public TSharedFromThis<FProtokitePlaytestSelfTest>
{
public:
	struct FOptions
	{
		/** The Game Version ID of a closed playtest of this game. Empty skips the closed-playtest counter-case, saying why. */
		FString ClosedPlaytestGameVersionId;

		/** How long to wait for the playtest to load, for the launch's session to start and for a queued event to go. */
		float WaitSeconds = 60.f;

		/** How long to wait for the recording's upload, which takes as long as the recording is large. */
		float UploadWaitSeconds = 300.f;

		/** The transport the self-test's own Protokite client uses; the engine's HTTP when unset. */
		TSharedPtr<IFlockHttpAdapter> HttpAdapterForTesting;
	};

	/** A Game Version ID no playtest is linked to. Protokite answers it exactly as it answers a release version: 404. */
	static constexpr const TCHAR* GameVersionIdWithNoPlaytest = TEXT("01ZZZZZZZZZZZZZZZZZZZZZZZZ");

	/** A Protokite session id that does not exist. */
	static constexpr const TCHAR* SessionIdThatDoesNotExist = TEXT("01ZZZZZZZZZZZZZZZZZZZZZZZZ");

	/** The key sent where a wrong one is wanted. */
	static constexpr const TCHAR* WrongApiKey = TEXT("flock-playtest-self-test-wrong-key");

	/** The playtest event it records, under the playtest category like any the game records. */
	static constexpr const TCHAR* SelfTestEventName = TEXT("playtest_self_test");

	/** Starts a run. OnFinished hears every step once the last has run; each step is also logged as it finishes. */
	static TSharedRef<FProtokitePlaytestSelfTest> Start(UProtokitePlaytestSubsystem* Playtest, UFlockSubsystem* Flock,
		const FOptions& Options, TFunction<void(const TArray<FProtokitePlaytestSelfTestStep>&)> OnFinished = nullptr);

	~FProtokitePlaytestSelfTest();

	bool IsFinished() const { return bFinished; }
	const TArray<FProtokitePlaytestSelfTestStep>& GetSteps() const { return Steps; }

	/**
	 * Whether an answer is the refusal a counter-case expects: a failure with ExpectedStatus and, when QuestionItMustName is
	 * given, a complaint naming exactly that question. A success, another status, a request that never reached the server,
	 * or a complaint about another question is not. OutWhatHappened says what the answer was, for the step's line.
	 */
	static bool IsTheExpectedRefusal(bool bSucceeded, const FFlockError& Error, int32 ExpectedStatus,
		const FString& QuestionItMustName, FString& OutWhatHappened);

	/**
	 * An option the question does not offer, as close to one it does as can be: its first option in other letters where
	 * that is not also an option, since the server compares options letter for letter.
	 */
	static FString MakeOptionNotOnTheList(const TArray<FString>& Options);

private:
	/** What a step needs before it can run; a step whose need is not met is skipped, saying which. */
	enum class ENeeds : uint8
	{
		Nothing,
		/** The playtest config loaded. */
		Playtest,
		/** This launch's Protokite session started. */
		Session,
	};

	struct FStepToRun
	{
		const TCHAR* Name;
		ENeeds Needs;
		void (FProtokitePlaytestSelfTest::*Run)();
	};

	FProtokitePlaytestSelfTest() = default;

	void RunNextStep();

	/** Records the running step's outcome, logs it and starts the next. Called exactly once per step. */
	void Finish(EProtokitePlaytestSelfTestOutcome Outcome, const FString& Detail);
	void FinishWithRefusal(bool bSucceeded, const FFlockError& Error, int32 ExpectedStatus, const FString& QuestionItMustName = FString());

	/** The game went away mid-run: the first step to notice fails, and every later one is skipped saying why. */
	void FinishBecauseTheGameShutDown();

	/**
	 * Calls Then(true) once Condition holds, or Then(false) after Seconds of ticks. Checked at once, then every tick.
	 * Then runs while this run is alive, so it may use this.
	 */
	void WaitUntil(TFunction<bool()> Condition, float Seconds, TFunction<void(bool bHappened)> Then);
	void StopWaiting();
	void Report();
	void StopListeningForTheUpload();

	TMap<FString, FString> Headers() const;
	FString ProtokiteApiUrl() const;
	FProtokitePlaytestFormSubmission MakeFormSubmission(const FString& PlaytestSessionId, const FString& AnswersJson) const;

	/** Sends a form expected to be refused with ExpectedStatus, naming QuestionItMustName when that is given. */
	void SendFormExpectingRefusal(const FProtokitePlaytestFormSubmission& Submission, int32 ExpectedStatus, const FString& QuestionItMustName);

	/** Flushes Flock's analytics and passes once PendingCount drops below CountBeforeTheFlush. */
	void FlushAndWaitUntilSent(TFunction<int32()> PendingCount, int32 CountBeforeTheFlush, const FString& WhatWasQueued);

	void CheckPlaytestLoaded();
	void CheckWrongApiKeyRefused();
	void CheckMissingApiKeyRefused();
	void CheckVersionWithNoPlaytestRefused();
	void CheckSessionStarted();
	void CheckSessionWithNoPlayerRefused();
	void CheckClosedPlaytestRefused();
	void CheckExceptionReported();
	void CheckPlaytestEventRecorded();
	void CheckFormMissingANeededAnswerRefused();
	void CheckFormWithAnOptionNotOnTheListRefused();
	void CheckFormForASessionThatDoesNotExistRefused();
	void CheckFormTaken();
	void CheckUploadLinkForASessionThatDoesNotExistRefused();
	void CheckRecordingUploaded();
	void CheckEndForASessionThatDoesNotExistRefused();
	void CheckSessionEnded();

	/** Ends a session a counter-case was given when it should have been refused, so the run leaves nothing open. */
	void EndSessionThatShouldNotExist(const TMap<FString, FString>& SessionHeaders, const FString& PlaytestSessionId);

	TWeakObjectPtr<UProtokitePlaytestSubsystem> Playtest;
	TWeakObjectPtr<UFlockSubsystem> Flock;
	FOptions Options;
	TSharedPtr<FProtokiteClient> Client;
	TFunction<void(const TArray<FProtokitePlaytestSelfTestStep>&)> OnFinished;

	TArray<FStepToRun> StepsToRun;
	int32 NextStep = 0;
	const TCHAR* RunningStepName = nullptr;
	TArray<FProtokitePlaytestSelfTestStep> Steps;
	bool bPlaytestLoaded = false;
	bool bSessionStarted = false;
	bool bGameShutDown = false;
	bool bFinished = false;

	/** Tells this run's fault apart from an earlier run's in the same launch, whose repeats would only be counted. */
	FString RunId;

	FTSTicker::FDelegateHandle WaitHandle;

	/** Rooted while it listens: the event holds it only weakly. */
	UProtokitePlaytestSelfTestListener* Listener = nullptr;
	bool bUploadFinished = false;
	bool bUploaded = false;
	FString UploadWhyNot;
};
