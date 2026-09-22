// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "ProtokitePlaytestSelfTest.h"

#if !UE_BUILD_SHIPPING

#include "Blueprint/BlueprintExceptionInfo.h"
#include "Config/FlockConfig.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "ProtokitePlaytestConfig.h"
#include "ProtokitePlaytestFormAnswers.h"
#include "ProtokitePlaytestFormSubmission.h"
#include "ProtokitePlaytestLog.h"
#include "ProtokitePlaytestLogger.h"
#include "ProtokitePlaytestPerformanceTimeline.h"
#include "ProtokitePlaytestRecordingUpload.h"
#include "ProtokitePlaytestSampleAnswers.h"
#include "ProtokitePlaytestSession.h"
#include "ProtokitePlaytestSettings.h"
#include "ProtokitePlaytestStatus.h"
#include "ProtokitePlaytestSubsystem.h"
#include "ProtokitePlaytestVideoEncoder.h"
#include "ProtokiteClient.h"
#include "FlockSubsystem.h"
#include "HAL/IConsoleManager.h"
#include "Http/FlockHttpClient.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Models/FlockCommandModels.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Providers/FlockAnalyticsProvider.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Script.h"
#include "UObject/Stack.h"

namespace
{
	/** How long a raised fault may take to reach the queue: the log sink hands it over on the next analytics tick. */
	constexpr float SecondsForAFaultToBeQueued = 5.f;

	const TCHAR* DescribeSelfTestOutcome(EProtokitePlaytestSelfTestOutcome Outcome)
	{
		switch (Outcome)
		{
		case EProtokitePlaytestSelfTestOutcome::Passed:
			return TEXT("PASS");
		case EProtokitePlaytestSelfTestOutcome::Skipped:
			return TEXT("SKIPPED");
		default:
			return TEXT("FAIL");
		}
	}

	FString DescribeSelfTestPlaytestStatus(EProtokitePlaytestStatus Status)
	{
		return StaticEnum<EProtokitePlaytestStatus>()->GetNameStringByValue(static_cast<int64>(Status));
	}

	FString SerializeSelfTestAnswers(const FProtokitePlaytestFormAnswers& Answers, const FProtokitePlaytestForm& Form)
	{
		FString Json;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Answers.ToWireObject(Form), Writer);
		return Json;
	}

	/**
	 * Whether Flock keeps a report in its queue until it is sent, which is the only way the self-test can watch one go.
	 * With Analytics Cache Failed Events off, a report is sent the moment it is made and nothing tells the game whether it
	 * arrived.
	 */
	bool FlockKeepsReportsUntilSent()
	{
		return GetDefault<UFlockConfig>()->bAnalyticsCacheFailedEvents;
	}

	const TCHAR* ReportsNotWatchableReason = TEXT("Analytics Cache Failed Events is off, so Flock sends a report the moment it "
		"is made and nothing tells the game whether it arrived. Turn it on (Project Settings > Plugins > Flock SDK Settings) "
		"to check this step.");

	/** The text answer the self-test gives, so its forms read as the self-test's on the dashboard. */
	const TCHAR* SelfTestAnswerText = TEXT("Sent by ProtokitePlaytest.SelfTest.");

	/**
	 * Raises the same Blueprint Accessed None twice, through the engine's own script-exception broadcast: the call the
	 * Blueprint VM makes when a graph reads through an empty reference. The first is reported and the second counted.
	 */
	bool RaiseTheSameBlueprintFaultTwice(UObject* Context, const FString& RunId)
	{
		UFunction* Function = UObject::StaticClass()->FindFunctionByName(NAME_ExecuteUbergraph);
		if (Function == nullptr || Context == nullptr)
		{
			return false;
		}
		TArray<uint8> Locals;
		Locals.SetNumZeroed(FMath::Max<int32>(Function->ParmsSize, 1));
		FFrame Stack(Context, Function, Locals.GetData());
		const FBlueprintExceptionInfo Info(EBlueprintExceptionType::AccessViolation, FText::FromString(FString::Printf(
			TEXT("Accessed None trying to read property ProtokitePlaytestSelfTestTarget (raised by ProtokitePlaytest.SelfTest, run %s)"), *RunId)));
		FBlueprintCoreDelegates::ThrowScriptException(Context, Stack, Info);
		FBlueprintCoreDelegates::ThrowScriptException(Context, Stack, Info);
		return true;
	}
}

TSharedRef<FProtokitePlaytestSelfTest> FProtokitePlaytestSelfTest::Start(UProtokitePlaytestSubsystem* InPlaytest, UFlockSubsystem* InFlock,
	const FOptions& InOptions, TFunction<void(const TArray<FProtokitePlaytestSelfTestStep>&)> InOnFinished)
{
	const TSharedRef<FProtokitePlaytestSelfTest> Run = MakeShareable(new FProtokitePlaytestSelfTest());
	Run->Playtest = InPlaytest;
	Run->Flock = InFlock;
	Run->Options = InOptions;
	Run->OnFinished = MoveTemp(InOnFinished);
	Run->RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();

	// A client of its own that never retries: a counter-case is sent once, and a refusal it provokes must not be
	// re-sent, logged or remembered as this launch's own.
	const UFlockConfig* FlockSettings = GetDefault<UFlockConfig>();
	const TSharedRef<IFlockLogger> Logger = MakeShared<FProtokitePlaytestLogger>();
	const TSharedRef<FFlockHttpClient> HttpClient = InOptions.HttpAdapterForTesting.IsValid()
		? MakeShared<FFlockHttpClient>(InOptions.HttpAdapterForTesting.ToSharedRef(), Logger, FlockSettings->HttpTimeoutSeconds)
		: FFlockHttpClient::CreateDefault(FlockSettings->HttpTimeoutSeconds, Logger);
	FFlockRetryPolicy NoRetries;
	NoRetries.MaxRetries = 0;
	Run->Client = MakeShared<FProtokiteClient>(HttpClient, NoRetries, Logger);

	using namespace ProtokitePlaytestSelfTestSteps;
	Run->StepsToRun = {
		{ PlaytestLoaded, ENeeds::Nothing, &FProtokitePlaytestSelfTest::CheckPlaytestLoaded },
		{ WrongApiKeyRefused, ENeeds::Playtest, &FProtokitePlaytestSelfTest::CheckWrongApiKeyRefused },
		{ MissingApiKeyRefused, ENeeds::Playtest, &FProtokitePlaytestSelfTest::CheckMissingApiKeyRefused },
		{ VersionWithNoPlaytestRefused, ENeeds::Playtest, &FProtokitePlaytestSelfTest::CheckVersionWithNoPlaytestRefused },
		{ SessionStarted, ENeeds::Playtest, &FProtokitePlaytestSelfTest::CheckSessionStarted },
		{ SessionWithNoPlayerRefused, ENeeds::Playtest, &FProtokitePlaytestSelfTest::CheckSessionWithNoPlayerRefused },
		{ ClosedPlaytestRefused, ENeeds::Session, &FProtokitePlaytestSelfTest::CheckClosedPlaytestRefused },
		{ ExceptionReported, ENeeds::Playtest, &FProtokitePlaytestSelfTest::CheckExceptionReported },
		{ PlaytestEventRecorded, ENeeds::Session, &FProtokitePlaytestSelfTest::CheckPlaytestEventRecorded },
		{ FormMissingANeededAnswerRefused, ENeeds::Session, &FProtokitePlaytestSelfTest::CheckFormMissingANeededAnswerRefused },
		{ FormWithAnOptionNotOnTheListRefused, ENeeds::Session, &FProtokitePlaytestSelfTest::CheckFormWithAnOptionNotOnTheListRefused },
		{ FormForASessionThatDoesNotExistRefused, ENeeds::Session, &FProtokitePlaytestSelfTest::CheckFormForASessionThatDoesNotExistRefused },
		{ FormTaken, ENeeds::Session, &FProtokitePlaytestSelfTest::CheckFormTaken },
		{ UploadLinkForASessionThatDoesNotExistRefused, ENeeds::Playtest, &FProtokitePlaytestSelfTest::CheckUploadLinkForASessionThatDoesNotExistRefused },
		{ RecordingUploaded, ENeeds::Session, &FProtokitePlaytestSelfTest::CheckRecordingUploaded },
		{ EndForASessionThatDoesNotExistRefused, ENeeds::Playtest, &FProtokitePlaytestSelfTest::CheckEndForASessionThatDoesNotExistRefused },
		{ SessionEnded, ENeeds::Session, &FProtokitePlaytestSelfTest::CheckSessionEnded },
	};

	UE_LOG(LogProtokitePlaytest, Display, TEXT("Playtest self-test: starting, against %s."), *Run->ProtokiteApiUrl());
	Run->RunNextStep();
	return Run;
}

FProtokitePlaytestSelfTest::~FProtokitePlaytestSelfTest()
{
	StopWaiting();
	StopListeningForTheUpload();
}

bool FProtokitePlaytestSelfTest::IsTheExpectedRefusal(bool bSucceeded, const FFlockError& Error, int32 ExpectedStatus,
	const FString& QuestionItMustName, FString& OutWhatHappened)
{
	if (bSucceeded)
	{
		OutWhatHappened = FString::Printf(TEXT("it was accepted, where HTTP %d was expected"), ExpectedStatus);
		return false;
	}
	if (Error.StatusCode == 0)
	{
		OutWhatHappened = FString::Printf(TEXT("it never reached Protokite: %s"), *Error.ToDisplayText());
		return false;
	}

	const FString Reason = Error.ServerMessage.IsEmpty() ? Error.ToDisplayText() : Error.ServerMessage;
	if (Error.StatusCode != ExpectedStatus)
	{
		OutWhatHappened = FString::Printf(TEXT("HTTP %d, where %d was expected: %s"), Error.StatusCode, ExpectedStatus, *Reason);
		return false;
	}
	if (!QuestionItMustName.IsEmpty())
	{
		// Protokite names the question only inside its sentence, so a refusal for another question looks the same
		// by status alone. Compared letter for letter, as the server names its questions.
		const FString Named = ProtokitePlaytestFindFieldIdInComplaint(Reason);
		if (!Named.Equals(QuestionItMustName, ESearchCase::CaseSensitive))
		{
			OutWhatHappened = Named.IsEmpty()
				? FString::Printf(TEXT("HTTP %d naming no question, where '%s' was expected: %s"), Error.StatusCode, *QuestionItMustName, *Reason)
				: FString::Printf(TEXT("HTTP %d naming '%s', where '%s' was expected: %s"), Error.StatusCode, *Named, *QuestionItMustName, *Reason);
			return false;
		}
	}
	OutWhatHappened = FString::Printf(TEXT("HTTP %d: %s"), Error.StatusCode, *Reason);
	return true;
}

FString FProtokitePlaytestSelfTest::MakeOptionNotOnTheList(const TArray<FString>& Options)
{
	const FString First = Options.Num() > 0 ? Options[0] : FString(TEXT("option"));
	const auto IsOnTheList = [&Options](const FString& Candidate)
	{
		return Options.ContainsByPredicate([&Candidate](const FString& Option) { return Option.Equals(Candidate, ESearchCase::CaseSensitive); });
	};
	for (const FString& Candidate : { First.ToUpper(), First.ToLower() })
	{
		if (!IsOnTheList(Candidate))
		{
			return Candidate;
		}
	}
	return First + TEXT(" (not on the list)");
}

void FProtokitePlaytestSelfTest::RunNextStep()
{
	if (NextStep >= StepsToRun.Num())
	{
		Report();
		return;
	}
	const FStepToRun Step = StepsToRun[NextStep++];
	RunningStepName = Step.Name;

	if (bGameShutDown || !Playtest.IsValid() || !Flock.IsValid())
	{
		FinishBecauseTheGameShutDown();
		return;
	}
	// A session needs the playtest too, so a playtest that did not load is the reason given for both.
	if (Step.Needs != ENeeds::Nothing && !bPlaytestLoaded)
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Skipped, TEXT("the playtest config did not load"));
		return;
	}
	if (Step.Needs == ENeeds::Session && !bSessionStarted)
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Skipped, TEXT("this launch's Protokite session did not start"));
		return;
	}
	(this->*Step.Run)();
}

void FProtokitePlaytestSelfTest::Finish(EProtokitePlaytestSelfTestOutcome Outcome, const FString& Detail)
{
	const FString Name = RunningStepName != nullptr ? FString(RunningStepName) : FString(TEXT("(no step)"));
	Steps.Add({ Name, Outcome, Detail });

	// A failure is a Warning and never an Error: an Error line is what exception capture reports, and a failed check
	// here is not a fault of the game.
	if (Outcome == EProtokitePlaytestSelfTestOutcome::Failed)
	{
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("Playtest self-test: %-7s %s -- %s"), DescribeSelfTestOutcome(Outcome), *Name, *Detail);
	}
	else
	{
		UE_LOG(LogProtokitePlaytest, Display, TEXT("Playtest self-test: %-7s %s -- %s"), DescribeSelfTestOutcome(Outcome), *Name, *Detail);
	}
	RunNextStep();
}

void FProtokitePlaytestSelfTest::FinishBecauseTheGameShutDown()
{
	const bool bFirstToNotice = !bGameShutDown;
	bGameShutDown = true;
	Finish(bFirstToNotice ? EProtokitePlaytestSelfTestOutcome::Failed : EProtokitePlaytestSelfTestOutcome::Skipped,
		TEXT("the game shut down during the self-test"));
}

void FProtokitePlaytestSelfTest::FinishWithRefusal(bool bSucceeded, const FFlockError& Error, int32 ExpectedStatus,
	const FString& QuestionItMustName)
{
	FString WhatHappened;
	const bool bExpected = IsTheExpectedRefusal(bSucceeded, Error, ExpectedStatus, QuestionItMustName, WhatHappened);
	Finish(bExpected ? EProtokitePlaytestSelfTestOutcome::Passed : EProtokitePlaytestSelfTestOutcome::Failed, WhatHappened);
}

void FProtokitePlaytestSelfTest::WaitUntil(TFunction<bool()> Condition, float Seconds, TFunction<void(bool bHappened)> Then)
{
	if (Condition())
	{
		Then(true);
		return;
	}

	// Ticks, not the wall clock, so a test can step it.
	const TWeakPtr<FProtokitePlaytestSelfTest> WeakSelf = AsShared();
	TSharedRef<float> Waited = MakeShared<float>(0.f);
	WaitHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[WeakSelf, Condition = MoveTemp(Condition), Seconds, Then = MoveTemp(Then), Waited](float DeltaSeconds) -> bool
		{
			const TSharedPtr<FProtokitePlaytestSelfTest> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return false;
			}
			*Waited += DeltaSeconds;
			const bool bHappened = Condition();
			if (!bHappened && *Waited < Seconds)
			{
				return true;
			}
			// Let go of the ticker before Then runs, since Then may start the next wait.
			Self->WaitHandle.Reset();
			Then(bHappened);
			return false;
		}));
}

void FProtokitePlaytestSelfTest::StopWaiting()
{
	if (WaitHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(WaitHandle);
		WaitHandle.Reset();
	}
}

void FProtokitePlaytestSelfTest::Report()
{
	bFinished = true;
	RunningStepName = nullptr;
	StopListeningForTheUpload();

	int32 Passed = 0;
	int32 Failed = 0;
	int32 Skipped = 0;
	for (const FProtokitePlaytestSelfTestStep& Step : Steps)
	{
		Passed += Step.Outcome == EProtokitePlaytestSelfTestOutcome::Passed ? 1 : 0;
		Failed += Step.Outcome == EProtokitePlaytestSelfTestOutcome::Failed ? 1 : 0;
		Skipped += Step.Outcome == EProtokitePlaytestSelfTestOutcome::Skipped ? 1 : 0;
	}
	if (Failed > 0)
	{
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("Playtest self-test finished: %d passed, %d failed, %d skipped."), Passed, Failed, Skipped);
	}
	else
	{
		UE_LOG(LogProtokitePlaytest, Display, TEXT("Playtest self-test finished: %d passed, %d failed, %d skipped."), Passed, Failed, Skipped);
	}

	if (OnFinished)
	{
		const TFunction<void(const TArray<FProtokitePlaytestSelfTestStep>&)> Callback = MoveTemp(OnFinished);
		Callback(Steps);
	}
}

void FProtokitePlaytestSelfTest::StopListeningForTheUpload()
{
	if (Listener == nullptr)
	{
		return;
	}
	if (UProtokitePlaytestSubsystem* PlaytestNow = Playtest.Get())
	{
		PlaytestNow->OnRecordingUploadFinished.RemoveDynamic(Listener, &UProtokitePlaytestSelfTestListener::HandleRecordingUploadFinished);
	}
	Listener->OnUploadFinished = nullptr;
	if (UObjectInitialized())
	{
		Listener->RemoveFromRoot();
	}
	Listener = nullptr;
}

TMap<FString, FString> FProtokitePlaytestSelfTest::Headers() const
{
	// The key and version Flock initialized with, the same ones every playtest request sends.
	const UFlockSubsystem* FlockNow = Flock.Get();
	return FlockNow != nullptr ? FlockNow->GetRequestHeaders() : TMap<FString, FString>();
}

FString FProtokitePlaytestSelfTest::ProtokiteApiUrl() const
{
	return GetDefault<UProtokitePlaytestSettings>()->ProtokiteApiUrl;
}

FProtokitePlaytestFormSubmission FProtokitePlaytestSelfTest::MakeFormSubmission(const FString& PlaytestSessionId, const FString& AnswersJson) const
{
	const UProtokitePlaytestSubsystem* PlaytestNow = Playtest.Get();
	FProtokitePlaytestFormSubmission Submission;
	Submission.PlaytestSessionId = PlaytestSessionId;
	Submission.AnswersJson = AnswersJson;
	Submission.ProtokiteApiUrl = ProtokiteApiUrl();
	if (PlaytestNow != nullptr)
	{
		Submission.Identity = PlaytestNow->GetPlaytestIdentity();
		Submission.FlockGameVersionId = PlaytestNow->GetPlaytestConfig().FlockGameVersionId;
	}
	return Submission;
}

void FProtokitePlaytestSelfTest::CheckPlaytestLoaded()
{
	// Waited for only while the status can still become Ready: the Flock SDK still starting, the config on its way, or a
	// fetch that failed, which is asked again when the next Flock session starts. Any other status is final for the launch.
	const TWeakObjectPtr<UProtokitePlaytestSubsystem> WeakPlaytest = Playtest;
	WaitUntil([WeakPlaytest]()
		{
			if (!WeakPlaytest.IsValid())
			{
				return true;
			}
			const EProtokitePlaytestStatus Status = WeakPlaytest->GetStatus();
			return Status != EProtokitePlaytestStatus::WaitingForFlock && Status != EProtokitePlaytestStatus::FetchingPlaytestConfig
				&& Status != EProtokitePlaytestStatus::PlaytestConfigUnavailable;
		},
		Options.WaitSeconds, [this](bool)
		{
			const UProtokitePlaytestSubsystem* PlaytestNow = Playtest.Get();
			if (PlaytestNow == nullptr)
			{
				FinishBecauseTheGameShutDown();
				return;
			}
			if (PlaytestNow->GetStatus() == EProtokitePlaytestStatus::WaitingForPlayerConsent
				|| PlaytestNow->GetStatus() == EProtokitePlaytestStatus::PlayerRefusedPlaytest)
			{
				// Not a failure: this build's playtest loaded, which is what the step checks. Nobody has allowed it to
				// collect anything, and a run with no one at the keyboard cannot answer a question drawn over the game.
				Finish(EProtokitePlaytestSelfTestOutcome::Skipped, FString::Printf(TEXT("%s Answer it with "
					"'ProtokitePlaytest.AnswerConsent video_and_play_data' before this self-test, or turn off Ask The Player "
					"For Playtest Consent in Project Settings > Plugins > Protokite Playtest Settings."),
					*ProtokitePlaytestConsent::Describe(PlaytestNow->GetPlaytestConsent())));
				return;
			}
			if (PlaytestNow->GetStatus() != EProtokitePlaytestStatus::Ready)
			{
				Finish(EProtokitePlaytestSelfTestOutcome::Failed, FString::Printf(TEXT("the playtest status is %s; the log above says "
					"why. Enable Playtesting and Protokite API URL are in Project Settings > Plugins > Protokite Playtest Settings."),
					*DescribeSelfTestPlaytestStatus(PlaytestNow->GetStatus())));
				return;
			}

			bPlaytestLoaded = true;
			const FProtokitePlaytestConfig& Config = PlaytestNow->GetPlaytestConfig();
			const auto OnOrOff = [PlaytestNow](const TCHAR* Feature) { return PlaytestNow->IsPlaytestFeatureEnabled(Feature) ? TEXT("on") : TEXT("off"); };
			Finish(EProtokitePlaytestSelfTestOutcome::Passed, FString::Printf(TEXT("playtest %s for version %s; video recording %s, "
				"heavy analytics %s, exception capturing %s; %s"), *Config.TestId, *Config.FlockGameVersionId,
				OnOrOff(ProtokitePlaytestFeatures::VideoRecording), OnOrOff(ProtokitePlaytestFeatures::HeavyAnalytics),
				OnOrOff(ProtokitePlaytestFeatures::ExceptionCapturing),
				Config.HasForm() ? *FString::Printf(TEXT("a feedback form of %d questions"), Config.Form.Fields.Num()) : TEXT("no feedback form")));
		});
}

void FProtokitePlaytestSelfTest::CheckWrongApiKeyRefused()
{
	TMap<FString, FString> SentHeaders = Headers();
	SentHeaders.Add(TEXT("X-Flock-API-Key"), WrongApiKey);
	const TWeakPtr<FProtokitePlaytestSelfTest> WeakSelf = AsShared();
	Client->FetchPlaytestConfig(ProtokiteApiUrl(), SentHeaders, [WeakSelf](TFlockResult<FProtokitePlaytestConfig> Result)
		{
			if (const TSharedPtr<FProtokitePlaytestSelfTest> Self = WeakSelf.Pin())
			{
				Self->FinishWithRefusal(Result.bSuccess, Result.Error, 401);
			}
		});
}

void FProtokitePlaytestSelfTest::CheckMissingApiKeyRefused()
{
	TMap<FString, FString> SentHeaders = Headers();
	SentHeaders.Remove(TEXT("X-Flock-API-Key"));
	const TWeakPtr<FProtokitePlaytestSelfTest> WeakSelf = AsShared();
	Client->FetchPlaytestConfig(ProtokiteApiUrl(), SentHeaders, [WeakSelf](TFlockResult<FProtokitePlaytestConfig> Result)
		{
			if (const TSharedPtr<FProtokitePlaytestSelfTest> Self = WeakSelf.Pin())
			{
				Self->FinishWithRefusal(Result.bSuccess, Result.Error, 422);
			}
		});
}

void FProtokitePlaytestSelfTest::CheckVersionWithNoPlaytestRefused()
{
	TMap<FString, FString> SentHeaders = Headers();
	SentHeaders.Add(TEXT("X-Game-Version-ID"), GameVersionIdWithNoPlaytest);
	const TWeakPtr<FProtokitePlaytestSelfTest> WeakSelf = AsShared();
	Client->FetchPlaytestConfig(ProtokiteApiUrl(), SentHeaders, [WeakSelf](TFlockResult<FProtokitePlaytestConfig> Result)
		{
			if (const TSharedPtr<FProtokitePlaytestSelfTest> Self = WeakSelf.Pin())
			{
				Self->FinishWithRefusal(Result.bSuccess, Result.Error, 404);
			}
		});
}

void FProtokitePlaytestSelfTest::CheckSessionStarted()
{
	const TWeakObjectPtr<UProtokitePlaytestSubsystem> WeakPlaytest = Playtest;
	WaitUntil([WeakPlaytest]()
		{
			if (!WeakPlaytest.IsValid())
			{
				return true;
			}
			const EProtokitePlaytestSessionState State = WeakPlaytest->GetPlaytestSessionState();
			return State != EProtokitePlaytestSessionState::NotStarted && State != EProtokitePlaytestSessionState::Starting;
		},
		Options.WaitSeconds, [this](bool)
		{
			const UProtokitePlaytestSubsystem* PlaytestNow = Playtest.Get();
			if (PlaytestNow == nullptr)
			{
				FinishBecauseTheGameShutDown();
				return;
			}
			switch (PlaytestNow->GetPlaytestSessionState())
			{
			case EProtokitePlaytestSessionState::Started:
				bSessionStarted = true;
				Finish(EProtokitePlaytestSelfTestOutcome::Passed, FString::Printf(TEXT("session %s, for the player's %s"),
					*PlaytestNow->GetPlaytestSessionId(),
					PlaytestNow->GetPlaytestIdentity().SteamId.IsEmpty() ? TEXT("device id") : TEXT("Steam id")));
				return;
			case EProtokitePlaytestSessionState::NotStarted:
				Finish(EProtokitePlaytestSelfTestOutcome::Failed, FString::Printf(TEXT("no session started within %.0f seconds. A playtest "
					"never signs a player in: the session starts once the game signs one in and their Flock session reaches the "
					"server, so sign in first -- with Flock.LoginWithDevice from a harness."), Options.WaitSeconds));
				return;
			case EProtokitePlaytestSessionState::Starting:
				Finish(EProtokitePlaytestSelfTestOutcome::Failed, FString::Printf(TEXT("the session start was sent and not answered "
					"within %.0f seconds"), Options.WaitSeconds));
				return;
			case EProtokitePlaytestSessionState::Ended:
				Finish(EProtokitePlaytestSelfTestOutcome::Failed, TEXT("this launch's session had already ended, so there is none to "
					"check. A launch has one session: start the game again to run the self-test."));
				return;
			case EProtokitePlaytestSessionState::NoPlayerIdentity:
				Finish(EProtokitePlaytestSelfTestOutcome::Failed, TEXT("no session was started, because there was neither a Steam id "
					"nor a device id to send; the log above says why"));
				return;
			default:
				Finish(EProtokitePlaytestSelfTestOutcome::Failed, TEXT("Protokite did not create this launch's session; the log above "
					"says why"));
				return;
			}
		});
}

void FProtokitePlaytestSelfTest::CheckSessionWithNoPlayerRefused()
{
	// Nobody to tie the session to: no Steam id and no device id.
	FProtokitePlaytestSessionStartRequest Request;
	const TMap<FString, FString> SentHeaders = Headers();
	const TWeakPtr<FProtokitePlaytestSelfTest> WeakSelf = AsShared();
	Client->StartPlaytestSession(ProtokiteApiUrl(), SentHeaders, Request,
		[WeakSelf, SentHeaders](TFlockResult<FProtokitePlaytestSessionStartResult> Result)
		{
			const TSharedPtr<FProtokitePlaytestSelfTest> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (Result.bSuccess)
			{
				Self->EndSessionThatShouldNotExist(SentHeaders, Result.Value.SessionId);
			}
			Self->FinishWithRefusal(Result.bSuccess, Result.Error, 422);
		});
}

void FProtokitePlaytestSelfTest::CheckClosedPlaytestRefused()
{
	if (Options.ClosedPlaytestGameVersionId.IsEmpty())
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Skipped, TEXT("no closed playtest was named. Pass the Game Version ID of a closed "
			"playtest of this game to check it: ProtokitePlaytest.SelfTest <Game Version ID>."));
		return;
	}

	FProtokitePlaytestSessionStartRequest Request;
	Request.Identity = Playtest->GetPlaytestIdentity();
	TMap<FString, FString> SentHeaders = Headers();
	SentHeaders.Add(TEXT("X-Game-Version-ID"), Options.ClosedPlaytestGameVersionId);
	const TWeakPtr<FProtokitePlaytestSelfTest> WeakSelf = AsShared();
	Client->StartPlaytestSession(ProtokiteApiUrl(), SentHeaders, Request,
		[WeakSelf, SentHeaders](TFlockResult<FProtokitePlaytestSessionStartResult> Result)
		{
			const TSharedPtr<FProtokitePlaytestSelfTest> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (Result.bSuccess)
			{
				// The playtest is still collecting, or the version is not a closed playtest's.
				Self->EndSessionThatShouldNotExist(SentHeaders, Result.Value.SessionId);
			}
			Self->FinishWithRefusal(Result.bSuccess, Result.Error, 400);
		});
}

void FProtokitePlaytestSelfTest::CheckExceptionReported()
{
	UProtokitePlaytestSubsystem* PlaytestNow = Playtest.Get();
	UFlockSubsystem* FlockNow = Flock.Get();
	if (!PlaytestNow->IsPlaytestFeatureEnabled(ProtokitePlaytestFeatures::ExceptionCapturing))
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Skipped, TEXT("this playtest does not turn exception capturing on"));
		return;
	}
	if (!FlockNow->GetExceptionCaptureCoverage().bEnabled)
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Failed, TEXT("this playtest turns exception capturing on, but the Flock SDK is not "
			"capturing exceptions. Turn on Analytics Enabled and Analytics Capture Exceptions: Project Settings > Plugins > "
			"Flock SDK Settings."));
		return;
	}
	FFlockAnalyticsProvider* Analytics = FlockNow->GetAnalyticsProvider();
	if (Analytics == nullptr || !Analytics->HasConsent())
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Failed, TEXT("the Flock SDK is not collecting: analytics is off, or the player's "
			"consent is withheld, so no fault is reported"));
		return;
	}

	if (!FlockKeepsReportsUntilSent())
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Skipped, ReportsNotWatchableReason);
		return;
	}

	const int32 CountBefore = Analytics->GetPendingEventCount();
	if (!RaiseTheSameBlueprintFaultTwice(PlaytestNow, RunId))
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Failed, TEXT("no Blueprint fault could be raised"));
		return;
	}

	const TWeakObjectPtr<UFlockSubsystem> WeakFlock = Flock;
	const auto PendingCount = [WeakFlock]() -> int32
	{
		const FFlockAnalyticsProvider* AnalyticsNow = WeakFlock.IsValid() ? WeakFlock->GetAnalyticsProvider() : nullptr;
		return AnalyticsNow != nullptr ? AnalyticsNow->GetPendingEventCount() : 0;
	};
	WaitUntil([PendingCount, CountBefore]() { return PendingCount() > CountBefore; }, SecondsForAFaultToBeQueued,
		[this, PendingCount, CountBefore](bool bQueued)
		{
			const int32 Queued = PendingCount() - CountBefore;
			if (!bQueued)
			{
				Finish(EProtokitePlaytestSelfTestOutcome::Failed, TEXT("the same Blueprint fault was raised twice and nothing was queued"));
				return;
			}
			if (Queued != 1)
			{
				// The counter-case: a repeat queued as a report of its own means repeats are not being counted.
				Finish(EProtokitePlaytestSelfTestOutcome::Failed, FString::Printf(TEXT("the same fault raised twice queued %d reports, "
					"where its repeat should have been counted into the first"), Queued));
				return;
			}
			FlushAndWaitUntilSent(PendingCount, CountBefore + 1, TEXT("the fault, raised twice, queued one report"));
		});
}

void FProtokitePlaytestSelfTest::CheckPlaytestEventRecorded()
{
	UProtokitePlaytestSubsystem* PlaytestNow = Playtest.Get();
	UFlockSubsystem* FlockNow = Flock.Get();
	if (!PlaytestNow->IsPlaytestFeatureEnabled(ProtokitePlaytestFeatures::HeavyAnalytics))
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Skipped, TEXT("this playtest does not turn heavy analytics on"));
		return;
	}
	const FFlockAnalyticsProvider* Analytics = FlockNow->GetAnalyticsProvider();
	if (Analytics == nullptr)
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Failed, TEXT("this playtest turns heavy analytics on, but the Flock SDK's analytics "
			"is off. Turn on Analytics Enabled: Project Settings > Plugins > Flock SDK Settings."));
		return;
	}

	// Read on either side of the calls in one frame, so no performance window can land between them.
	const int32 CountBefore = Analytics->GetPendingAnalyticsEventCount();
	FFlockCommandData Properties;
	Properties.Set(TEXT("sent_by"), TEXT("ProtokitePlaytest.SelfTest"));
	const bool bRecorded = PlaytestNow->RecordPlaytestEvent(SelfTestEventName, Properties);
	const int32 CountAfterRecording = Analytics->GetPendingAnalyticsEventCount();
	// The counter-case: a game may not send an event under the plugin's own name, which would read as the plugin's.
	const bool bOwnNameAccepted = PlaytestNow->RecordPlaytestEvent(ProtokitePlaytestEvents::PerformanceWindow);

	if (!bRecorded)
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Failed, FString::Printf(TEXT("%s was refused; the log above says why"), SelfTestEventName));
		return;
	}
	if (bOwnNameAccepted)
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Failed, FString::Printf(TEXT("the game was allowed to record %s, which is the "
			"plugin's own event"), ProtokitePlaytestEvents::PerformanceWindow));
		return;
	}
	if (!FlockKeepsReportsUntilSent())
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Skipped, FString::Printf(TEXT("%s was recorded and %s refused, but %s"),
			SelfTestEventName, ProtokitePlaytestEvents::PerformanceWindow, ReportsNotWatchableReason));
		return;
	}
	if (CountAfterRecording != CountBefore + 1)
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Failed, FString::Printf(TEXT("%s was accepted, and %d events were queued for it"),
			SelfTestEventName, CountAfterRecording - CountBefore));
		return;
	}

	const TWeakObjectPtr<UFlockSubsystem> WeakFlock = Flock;
	FlushAndWaitUntilSent([WeakFlock]() -> int32
		{
			const FFlockAnalyticsProvider* AnalyticsNow = WeakFlock.IsValid() ? WeakFlock->GetAnalyticsProvider() : nullptr;
			return AnalyticsNow != nullptr ? AnalyticsNow->GetPendingAnalyticsEventCount() : 0;
		},
		CountAfterRecording, FString::Printf(TEXT("%s was queued, and %s refused"), SelfTestEventName, ProtokitePlaytestEvents::PerformanceWindow));
}

void FProtokitePlaytestSelfTest::FlushAndWaitUntilSent(TFunction<int32()> PendingCount, int32 CountBeforeTheFlush, const FString& WhatWasQueued)
{
	FFlockAnalyticsProvider* Analytics = Flock.IsValid() ? Flock->GetAnalyticsProvider() : nullptr;
	if (Analytics == nullptr)
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Failed, TEXT("the Flock SDK's analytics went away before the flush"));
		return;
	}

	const TWeakPtr<FProtokitePlaytestSelfTest> WeakSelf = AsShared();
	Analytics->Flush([WeakSelf, PendingCount, CountBeforeTheFlush, WhatWasQueued](TFlockResult<FFlockAnalyticsAck> Result)
		{
			const TSharedPtr<FProtokitePlaytestSelfTest> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (!Result.bSuccess)
			{
				Self->Finish(EProtokitePlaytestSelfTestOutcome::Failed, FString::Printf(TEXT("%s, and the flush failed: %s"),
					*WhatWasQueued, *Result.Error.ToDisplayText()));
				return;
			}
			// A flush that found another already running succeeds without sending, so what counts is the queue going down.
			Self->WaitUntil([PendingCount, CountBeforeTheFlush]() { return PendingCount() < CountBeforeTheFlush; },
				Self->Options.WaitSeconds, [RawSelf = Self.Get(), WhatWasQueued](bool bSent)
				{
					RawSelf->Finish(bSent ? EProtokitePlaytestSelfTestOutcome::Passed : EProtokitePlaytestSelfTestOutcome::Failed,
						bSent ? FString::Printf(TEXT("%s, and it was sent"), *WhatWasQueued)
							: FString::Printf(TEXT("%s, and it was still queued %.0f seconds after the flush"), *WhatWasQueued, RawSelf->Options.WaitSeconds));
				});
		});
}

void FProtokitePlaytestSelfTest::SendFormExpectingRefusal(const FProtokitePlaytestFormSubmission& Submission, int32 ExpectedStatus,
	const FString& QuestionItMustName)
{
	const TWeakPtr<FProtokitePlaytestSelfTest> WeakSelf = AsShared();
	Client->SubmitFeedbackForm(Headers(), Submission,
		[WeakSelf, ExpectedStatus, QuestionItMustName](TFlockResult<FProtokitePlaytestFormSubmitResult> Result)
		{
			if (const TSharedPtr<FProtokitePlaytestSelfTest> Self = WeakSelf.Pin())
			{
				Self->FinishWithRefusal(Result.bSuccess, Result.Error, ExpectedStatus, QuestionItMustName);
			}
		});
}

void FProtokitePlaytestSelfTest::CheckFormMissingANeededAnswerRefused()
{
	const UProtokitePlaytestSubsystem* PlaytestNow = Playtest.Get();
	if (!PlaytestNow->CanOpenFeedbackForm())
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Skipped, TEXT("this playtest publishes no feedback form"));
		return;
	}
	const FProtokitePlaytestForm& Form = PlaytestNow->GetPlaytestConfig().Form;
	const FProtokitePlaytestFormField* Needed = Form.Fields.FindByPredicate([](const FProtokitePlaytestFormField& Field) { return Field.Required; });
	if (Needed == nullptr)
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Skipped, TEXT("no question on this playtest's form needs an answer"));
		return;
	}

	// Every other question answered, so the only thing the server can object to is the one left out.
	FProtokitePlaytestForm FormWithoutIt = Form;
	const FString NeededId = Needed->Id;
	FormWithoutIt.Fields.RemoveAll([&NeededId](const FProtokitePlaytestFormField& Field) { return Field.Id.Equals(NeededId, ESearchCase::CaseSensitive); });
	const FProtokitePlaytestFormAnswers Answers = ProtokitePlaytestAnswerEveryQuestion(FormWithoutIt, SelfTestAnswerText);
	SendFormExpectingRefusal(MakeFormSubmission(PlaytestNow->GetPlaytestSessionId(), SerializeSelfTestAnswers(Answers, Form)),
		422, NeededId);
}

void FProtokitePlaytestSelfTest::CheckFormWithAnOptionNotOnTheListRefused()
{
	const UProtokitePlaytestSubsystem* PlaytestNow = Playtest.Get();
	if (!PlaytestNow->CanOpenFeedbackForm())
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Skipped, TEXT("this playtest publishes no feedback form"));
		return;
	}
	const FProtokitePlaytestForm& Form = PlaytestNow->GetPlaytestConfig().Form;
	const FProtokitePlaytestFormField* Select = Form.Fields.FindByPredicate([](const FProtokitePlaytestFormField& Field)
		{
			return Field.IsOfKind(ProtokitePlaytestFormFieldTypes::Select) && Field.Options.Num() > 0;
		});
	if (Select == nullptr)
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Skipped, TEXT("this playtest's form has no question with a list of options"));
		return;
	}

	FProtokitePlaytestFormAnswers Answers = ProtokitePlaytestAnswerEveryQuestion(Form, SelfTestAnswerText);
	Answers.SetChosenOption(Select->Id, MakeOptionNotOnTheList(Select->Options));
	SendFormExpectingRefusal(MakeFormSubmission(PlaytestNow->GetPlaytestSessionId(), SerializeSelfTestAnswers(Answers, Form)),
		422, Select->Id);
}

void FProtokitePlaytestSelfTest::CheckFormForASessionThatDoesNotExistRefused()
{
	const UProtokitePlaytestSubsystem* PlaytestNow = Playtest.Get();
	if (!PlaytestNow->CanOpenFeedbackForm())
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Skipped, TEXT("this playtest publishes no feedback form"));
		return;
	}
	const FProtokitePlaytestForm& Form = PlaytestNow->GetPlaytestConfig().Form;
	SendFormExpectingRefusal(MakeFormSubmission(SessionIdThatDoesNotExist,
		SerializeSelfTestAnswers(ProtokitePlaytestAnswerEveryQuestion(Form, SelfTestAnswerText), Form)), 404, FString());
}

void FProtokitePlaytestSelfTest::CheckFormTaken()
{
	const UProtokitePlaytestSubsystem* PlaytestNow = Playtest.Get();
	if (!PlaytestNow->CanOpenFeedbackForm())
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Skipped, TEXT("this playtest publishes no feedback form"));
		return;
	}
	const FProtokitePlaytestForm& Form = PlaytestNow->GetPlaytestConfig().Form;
	const FProtokitePlaytestFormAnswers Answers = ProtokitePlaytestAnswerEveryQuestion(Form, SelfTestAnswerText);
	const TArray<FProtokitePlaytestFormProblem> Problems = Answers.FindProblems(Form);
	if (Problems.Num() > 0)
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Failed, FString::Printf(TEXT("the self-test's own answers would be refused: question "
			"'%s'. A question kind this build does not answer the way the server expects?"), *Problems[0].FieldId));
		return;
	}

	const FString SessionId = PlaytestNow->GetPlaytestSessionId();
	const TWeakPtr<FProtokitePlaytestSelfTest> WeakSelf = AsShared();
	Client->SubmitFeedbackForm(Headers(), MakeFormSubmission(SessionId, SerializeSelfTestAnswers(Answers, Form)),
		[WeakSelf, SessionId, QuestionCount = Form.Fields.Num()](TFlockResult<FProtokitePlaytestFormSubmitResult> Result)
		{
			const TSharedPtr<FProtokitePlaytestSelfTest> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (Result.bSuccess)
			{
				Self->Finish(EProtokitePlaytestSelfTestOutcome::Passed, FString::Printf(TEXT("%d answers taken for session %s"),
					QuestionCount, *SessionId));
			}
			else
			{
				Self->Finish(EProtokitePlaytestSelfTestOutcome::Failed, Result.Error.ToDisplayText());
			}
		});
}

void FProtokitePlaytestSelfTest::CheckUploadLinkForASessionThatDoesNotExistRefused()
{
	const TWeakPtr<FProtokitePlaytestSelfTest> WeakSelf = AsShared();
	Client->RequestRecordingUploadLink(ProtokiteApiUrl(), Headers(), SessionIdThatDoesNotExist, ProtokitePlaytestRecordingContentTypes::WebM,
		[WeakSelf](TFlockResult<FProtokitePlaytestRecordingUploadLink> Result)
		{
			if (const TSharedPtr<FProtokitePlaytestSelfTest> Self = WeakSelf.Pin())
			{
				Self->FinishWithRefusal(Result.bSuccess, Result.Error, 404);
			}
		});
}

void FProtokitePlaytestSelfTest::CheckRecordingUploaded()
{
	UProtokitePlaytestSubsystem* PlaytestNow = Playtest.Get();
	if (!PlaytestNow->IsPlaytestFeatureEnabled(ProtokitePlaytestFeatures::VideoRecording))
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Skipped, TEXT("this playtest does not record video"));
		return;
	}
	// Every platform but 64-bit Windows records nothing: a limit of this build, not a fault of the launch.
	if (!FProtokitePlaytestVideoEncoder::IsBuiltWithVideo())
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Skipped,
			TEXT("this build has no video encoder: video recording is built for 64-bit Windows only"));
		return;
	}
	if (!PlaytestNow->CanSendTheRecording())
	{
		if (!FApp::CanEverRender())
		{
			Finish(EProtokitePlaytestSelfTestOutcome::Skipped, TEXT("this launch cannot render (-nullrhi), so nothing is recorded. Run "
				"it with a window to check the recording."));
		}
		else
		{
			Finish(EProtokitePlaytestSelfTestOutcome::Failed, TEXT("this playtest records video, but this launch has no recording it "
				"can send; the log above says why"));
		}
		return;
	}

	Listener = NewObject<UProtokitePlaytestSelfTestListener>();
	Listener->AddToRoot();
	bUploadFinished = false;
	const TWeakPtr<FProtokitePlaytestSelfTest> WeakSelf = AsShared();
	Listener->OnUploadFinished = [WeakSelf](bool bInUploaded, const FString& WhyNot)
	{
		if (const TSharedPtr<FProtokitePlaytestSelfTest> Self = WeakSelf.Pin())
		{
			Self->bUploadFinished = true;
			Self->bUploaded = bInUploaded;
			Self->UploadWhyNot = WhyNot;
		}
	};
	PlaytestNow->OnRecordingUploadFinished.AddDynamic(Listener, &UProtokitePlaytestSelfTestListener::HandleRecordingUploadFinished);

	if (!PlaytestNow->StopVideoRecordingAndUploadIt())
	{
		StopListeningForTheUpload();
		Finish(EProtokitePlaytestSelfTestOutcome::Failed, TEXT("the recording could not be stopped to be sent"));
		return;
	}

	// The upload finishes on a later frame, or here already when it could not begin.
	WaitUntil([this]() { return bUploadFinished; }, Options.UploadWaitSeconds, [this](bool bHeard)
		{
			StopListeningForTheUpload();
			if (!bHeard)
			{
				Finish(EProtokitePlaytestSelfTestOutcome::Failed, FString::Printf(TEXT("the upload had not finished after %.0f seconds"),
					Options.UploadWaitSeconds));
				return;
			}
			Finish(bUploaded ? EProtokitePlaytestSelfTestOutcome::Passed : EProtokitePlaytestSelfTestOutcome::Failed,
				bUploaded ? FString(TEXT("uploaded to this launch's session, and no longer kept on disk")) : UploadWhyNot);
		});
}

void FProtokitePlaytestSelfTest::CheckEndForASessionThatDoesNotExistRefused()
{
	const TWeakPtr<FProtokitePlaytestSelfTest> WeakSelf = AsShared();
	Client->EndPlaytestSession(ProtokiteApiUrl(), Headers(), SessionIdThatDoesNotExist,
		[WeakSelf](TFlockResult<FProtokitePlaytestSessionEndResult> Result)
		{
			if (const TSharedPtr<FProtokitePlaytestSelfTest> Self = WeakSelf.Pin())
			{
				Self->FinishWithRefusal(Result.bSuccess, Result.Error, 404);
			}
		});
}

void FProtokitePlaytestSelfTest::CheckSessionEnded()
{
	UProtokitePlaytestSubsystem* PlaytestNow = Playtest.Get();
	const FString SessionId = PlaytestNow->GetPlaytestSessionId();
	const TWeakPtr<FProtokitePlaytestSelfTest> WeakSelf = AsShared();
	const TWeakObjectPtr<UProtokitePlaytestSubsystem> WeakPlaytest = Playtest;
	const bool bSent = PlaytestNow->EndPlaytestSession([WeakSelf, WeakPlaytest, SessionId](bool bEnded, const FString& WhyNot)
		{
			const TSharedPtr<FProtokitePlaytestSelfTest> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			const bool bStateEnded = WeakPlaytest.IsValid() && WeakPlaytest->GetPlaytestSessionState() == EProtokitePlaytestSessionState::Ended;
			if (bEnded && bStateEnded)
			{
				Self->Finish(EProtokitePlaytestSelfTestOutcome::Passed, FString::Printf(TEXT("session %s ended"), *SessionId));
			}
			else
			{
				Self->Finish(EProtokitePlaytestSelfTestOutcome::Failed, bEnded
					? FString(TEXT("Protokite took the end, but the playtest does not show the session as ended"))
					: FString::Printf(TEXT("session %s could not be ended, so Protokite shows it as still in progress: %s"), *SessionId, *WhyNot));
			}
		});
	if (!bSent)
	{
		Finish(EProtokitePlaytestSelfTestOutcome::Failed, TEXT("no end was sent: the playtest has no session to end"));
	}
}

void FProtokitePlaytestSelfTest::EndSessionThatShouldNotExist(const TMap<FString, FString>& SessionHeaders, const FString& PlaytestSessionId)
{
	Client->EndPlaytestSession(ProtokiteApiUrl(), SessionHeaders, PlaytestSessionId,
		[PlaytestSessionId](TFlockResult<FProtokitePlaytestSessionEndResult> Result)
		{
			if (Result.bSuccess)
			{
				UE_LOG(LogProtokitePlaytest, Display, TEXT("Playtest self-test: ended session %s, which Protokite should have refused to create."),
					*PlaytestSessionId);
			}
			else
			{
				UE_LOG(LogProtokitePlaytest, Warning, TEXT("Playtest self-test: session %s, which Protokite should have refused to create, "
					"could not be ended and shows as in progress: %s"), *PlaytestSessionId, *Result.Error.ToDisplayText());
			}
		});
}

namespace
{
	/** The run the console command started, kept alive until it finishes and let go of before the engine exits. */
	TSharedPtr<FProtokitePlaytestSelfTest> GRunningPlaytestSelfTest;
	FDelegateHandle GLetGoOfPlaytestSelfTestAtExit;

	FAutoConsoleCommandWithWorldArgsAndOutputDevice PlaytestSelfTestCommand(
		TEXT("ProtokitePlaytest.SelfTest"),
		TEXT("Checks everything this playtest build does against the real Protokite, each step beside its counter-case, and "
			"logs a count of what passed: ProtokitePlaytest.SelfTest [Game Version ID of a closed playtest of this game]. It signs "
			"nobody in, so a harness runs Flock.LoginWithDevice first. It ends this launch's Protokite session as its last step. "
			"Development builds only."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Output)
		{
			if (GRunningPlaytestSelfTest.IsValid() && !GRunningPlaytestSelfTest->IsFinished())
			{
				Output.Log(TEXT("A playtest self-test is already running."));
				return;
			}
			if (Args.Num() > 1)
			{
				Output.Log(TEXT("Usage: ProtokitePlaytest.SelfTest [Game Version ID of a closed playtest of this game]."));
				return;
			}
			UGameInstance* GameInstance = World != nullptr ? World->GetGameInstance() : nullptr;
			UProtokitePlaytestSubsystem* Playtest = GameInstance != nullptr ? GameInstance->GetSubsystem<UProtokitePlaytestSubsystem>() : nullptr;
			UFlockSubsystem* Flock = GameInstance != nullptr ? GameInstance->GetSubsystem<UFlockSubsystem>() : nullptr;
			if (Playtest == nullptr || Flock == nullptr)
			{
				Output.Log(TEXT("No game instance with the Flock SDK and the Protokite Playtest plugin runs in this world."));
				return;
			}

			if (!GLetGoOfPlaytestSelfTestAtExit.IsValid())
			{
				// Let go of while the object system still exists, since a run holds a rooted listener.
				GLetGoOfPlaytestSelfTestAtExit = FCoreDelegates::OnPreExit.AddLambda([]() { GRunningPlaytestSelfTest.Reset(); });
			}

			FProtokitePlaytestSelfTest::FOptions Options;
			Options.ClosedPlaytestGameVersionId = Args.Num() > 0 ? Args[0] : FString();
			GRunningPlaytestSelfTest = FProtokitePlaytestSelfTest::Start(Playtest, Flock, Options);
		}));
}

#endif // !UE_BUILD_SHIPPING
