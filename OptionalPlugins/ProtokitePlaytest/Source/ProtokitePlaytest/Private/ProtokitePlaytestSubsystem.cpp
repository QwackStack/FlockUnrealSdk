// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "ProtokitePlaytestSubsystem.h"

#include "Async/Async.h"
#include "Config/FlockConfig.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FlockEvents.h"
#include "ProtokitePlaytestLocalSettings.h"
#include "ProtokitePlaytestLog.h"
#include "ProtokitePlaytestLogger.h"
#include "ProtokitePlaytestSettings.h"
#include "Async/Async.h"
#include "ProtokitePlaytestRecordingsFolder.h"
#include "ProtokitePlaytestVideoFrameSource.h"
#include "ProtokitePlaytestVideoRecording.h"
#include "ProtokitePlaytestRecordingUploads.h"
#include "ProtokitePlaytestSampleAnswers.h"
#include "ProtokitePlaytestFormSpool.h"
#include "SProtokitePlaytestConsentWidget.h"
#include "SProtokitePlaytestFormWidget.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "ProtokiteClient.h"
#include "Http/FlockFileUploader.h"
#include "FlockSubsystem.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockHttpShutdown.h"
#include "Misc/Paths.h"
#include "Providers/FlockAnalyticsProvider.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	/** Sends the end for one Protokite session and logs what became of it. The one place an end is sent. */
	void SendPlaytestSessionEnd(const TSharedRef<FProtokiteClient>& Client, const FString& ProtokiteApiUrl,
		const TMap<FString, FString>& RequestHeaders, const FString& PlaytestSessionId,
		TFunction<void(bool bEnded, const FString& WhyNot)> OnEnded = nullptr)
	{
		Client->EndPlaytestSession(ProtokiteApiUrl, RequestHeaders, PlaytestSessionId,
			[PlaytestSessionId, OnEnded](TFlockResult<FProtokitePlaytestSessionEndResult> Result)
			{
				if (Result.bSuccess)
				{
					UE_LOG(LogProtokitePlaytest, Log, TEXT("Protokite session %s ended."), *PlaytestSessionId);
				}
				else
				{
					UE_LOG(LogProtokitePlaytest, Warning, TEXT("Protokite session %s could not be ended, so Protokite shows it as still in progress. %s"),
						*PlaytestSessionId, *Result.Error.ToDisplayText());
				}
				if (OnEnded)
				{
					OnEnded(Result.bSuccess, Result.bSuccess ? FString() : Result.Error.ToDisplayText());
				}
			});
	}

	double RoundToHundredths(double Value)
	{
		return FMath::RoundToDouble(Value * 100.0) / 100.0;
	}

	/** A world's map name, without the prefix the editor adds while playing in the editor; empty without a world. */
	FString MapNameOf(const UWorld* World)
	{
		return World != nullptr ? UWorld::RemovePIEPrefix(World->GetMapName()) : FString();
	}
}

void UProtokitePlaytestSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// The Flock subsystem initializes first, so with Auto-Initialize On Load it is already up by the time
	// this starts following it.
	FollowFlockLifecycle(Collection.InitializeDependency<UFlockSubsystem>());
}

void UProtokitePlaytestSubsystem::Deinitialize()
{
	// The player is leaving, so the launch's session ends while the network is still there. A start still on its way
	// is ended when its answer arrives.
	if (SessionState == EProtokitePlaytestSessionState::Started)
	{
		EndPlaytestSession();
		// Sending the end is not the same as it arriving. The game tears down around the request, and the engine's
		// HTTP module then unbinds it part-way -- measured 2026-09-16, which left sessions showing in Protokite as
		// still in progress with no duration and nothing logged, because the completion that would have logged it was
		// the thing being unbound. Protokite takes no end time after the fact (BN-9), so this is the only chance it gets.
		FlockFinishRequestsBeforeShutdown();
	}

	// The Flock subsystem can outlive this one during teardown, and its shut-down event would otherwise
	// still reach a subsystem that has finished.
	if (UFlockSubsystem* Followed = Flock.Get())
	{
		UFlockEvents* Events = Followed->GetEvents();
		Events->OnInitialized.RemoveDynamic(this, &UProtokitePlaytestSubsystem::HandleFlockLifecycleChanged);
		Events->OnShutdown.RemoveDynamic(this, &UProtokitePlaytestSubsystem::HandleFlockLifecycleChanged);
		Events->OnSessionStarted.RemoveDynamic(this, &UProtokitePlaytestSubsystem::HandleFlockSessionStarted);
		Events->OnSessionRegistered.RemoveDynamic(this, &UProtokitePlaytestSubsystem::HandleFlockSessionRegistered);
	}
	Flock.Reset();

	// Read by the upload paths: from here on a finished recording is a later launch's to push, never this one's to start
	// sending into a shutdown.
	bDeinitializing = true;

	// Nothing this subsystem put on screen, or had listening to the keyboard, outlives it.
	CloseFeedbackForm();
	StopWaitingForAViewportToAskIn();
	CloseConsentQuestion();
	if (WaitingToUploadEarlierRecordings.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(WaitingToUploadEarlierRecordings);
		WaitingToUploadEarlierRecordings.Reset();
	}

	// A reply still on its way must not reach a subsystem that has finished.
	ForgetPlaytestConfig();

	// Whatever the last status was, no playtest work may run on a subsystem that has shut down.
	ApplyStatus(EProtokitePlaytestStatus::Stopped, FString(), FString());
	UpdatePerformanceTimeline();

	// The file is finished before the subsystem goes, so what was recorded is kept.
	FinishVideoRecordingNow(EProtokitePlaytestVideoStopReason::GameInstanceShutDown);

	// The folder is let go of only now, not when the recording finished: until the game instance shuts down, a finished
	// recording is still this launch's. From here on it belongs to a later launch.
	VideoRecordingRun.Reset();

	// Nothing this subsystem started outlives it, going through earlier launches' recordings included. It holds up shutting
	// down only when the game quits while that is still going.
	if (RecordingsFolderFinished.IsValid())
	{
		RecordingsFolderFinished.Wait();
	}

	Super::Deinitialize();
}

bool UProtokitePlaytestSubsystem::IsPlaytestFeatureEnabled(const FString& FeatureName) const
{
	// Three answers, all of which have to be yes: the build is in a state to do playtest work, the player allowed this
	// half of it, and the playtest asked for it.
	return Status == EProtokitePlaytestStatus::Ready
		&& ProtokitePlaytestConsent::AllowsFeature(GetPlaytestConsent(), FeatureName)
		&& PlaytestConfig.IsFeatureEnabled(FeatureName);
}

EProtokitePlaytestConsentChoice UProtokitePlaytestSubsystem::GetPlaytestConsent() const
{
	const EProtokitePlaytestConsentChoice Answer = GetPlayersConsentAnswer();
	if (ProtokitePlaytestConsent::IsAnswered(Answer))
	{
		return Answer;
	}
	// Nobody has answered. A build that asks collects nothing until someone does; one that does not ask collects what
	// the playtest turns on, and its sessions say that nobody was asked.
	return DoesThisBuildAskForPlaytestConsent()
		? EProtokitePlaytestConsentChoice::NotAnswered
		: EProtokitePlaytestConsentChoice::VideoAndPlayData;
}

EProtokitePlaytestConsentChoice UProtokitePlaytestSubsystem::GetPlayersConsentAnswer() const
{
	// Read once and kept: every feature check asks for it, and a file read per frame would be silly.
	if (!SavedConsentChoice.IsSet())
	{
		SavedConsentChoice = FProtokitePlaytestConsentFile(GetConsentFilePath()).Read();
	}
	return SavedConsentChoice.GetValue();
}

bool UProtokitePlaytestSubsystem::DoesThisBuildAskForPlaytestConsent() const
{
	return GetDefault<UProtokitePlaytestSettings>()->bAskThePlayerForPlaytestConsent;
}

FString UProtokitePlaytestSubsystem::GetConsentFilePath() const
{
	return TestConsentFilePath.IsEmpty() ? FProtokitePlaytestConsentFile::GetDefaultPath() : TestConsentFilePath;
}

bool UProtokitePlaytestSubsystem::SetPlaytestConsent(EProtokitePlaytestConsentChoice Choice)
{
	const FProtokitePlaytestConsentFile File(GetConsentFilePath());
	if (!File.Save(Choice))
	{
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("The player's answer about what this playtest may collect could not be saved to %s, so they are asked again next launch. Nothing is collected until it can be saved."),
			*File.GetPath());
		SavedConsentChoice = EProtokitePlaytestConsentChoice::NotAnswered;
		RefreshStatus();
		return false;
	}

	SavedConsentChoice = Choice;
	// The question has been answered however it was put, so nothing is waiting for it any more.
	bAskedToChangeConsent = false;
	UE_LOG(LogProtokitePlaytest, Log, TEXT("%s The answer is kept in %s."), *ProtokitePlaytestConsent::Describe(Choice), *File.GetPath());

	// Applies it at once: the status is decided again, which starts or stops the recording, the performance windows and
	// the launch's session.
	RefreshStatus();
	return true;
}

bool UProtokitePlaytestSubsystem::AskForPlaytestConsent()
{
	if (!IsThePlaytestLoaded())
	{
		UE_LOG(LogProtokitePlaytest, Log, TEXT("There is nothing to ask the player about: no playtest is loaded. %s"),
			*DescribePlaytestStatus(Status));
		return false;
	}
	if (FormWidget.IsValid())
	{
		// One panel at a time, and nothing is remembered to be done afterwards: a game that means to ask asks again
		// once its player has finished with the form.
		UE_LOG(LogProtokitePlaytest, Log, TEXT("The playtest's consent question cannot be put while the feedback form is open."));
		return false;
	}
	bAskedToChangeConsent = true;
	UpdateConsentQuestion();
	return IsConsentQuestionOpen();
}

bool UProtokitePlaytestSubsystem::EndPlaytestSession(TFunction<void(bool bEnded, const FString& WhyNot)> OnEnded)
{
	if (SessionState != EProtokitePlaytestSessionState::Started && SessionState != EProtokitePlaytestSessionState::Ended)
	{
		return false;
	}
	SessionState = EProtokitePlaytestSessionState::Ended;
	SendPlaytestSessionEnd(GetOrCreateProtokiteClient(), PlaytestSessionApiUrl, PlaytestSessionHeaders, PlaytestSessionId,
		MoveTemp(OnEnded));
	return true;
}

UFlockSubsystem* UProtokitePlaytestSubsystem::GetFollowedFlockForTesting() const
{
	return Flock.Get();
}

void UProtokitePlaytestSubsystem::FollowFlockLifecycle(UFlockSubsystem* InFlock)
{
	// Before anything can record.
	FinishWhatEndedRunsLeftInRecordingsFolder();

	Flock = InFlock;
	if (InFlock)
	{
		UFlockEvents* Events = InFlock->GetEvents();
		Events->OnInitialized.AddUniqueDynamic(this, &UProtokitePlaytestSubsystem::HandleFlockLifecycleChanged);
		Events->OnShutdown.AddUniqueDynamic(this, &UProtokitePlaytestSubsystem::HandleFlockLifecycleChanged);
		Events->OnSessionStarted.AddUniqueDynamic(this, &UProtokitePlaytestSubsystem::HandleFlockSessionStarted);
		Events->OnSessionRegistered.AddUniqueDynamic(this, &UProtokitePlaytestSubsystem::HandleFlockSessionRegistered);
	}
	RefreshStatus();

	// Whether or not playtesting is on this launch: what an earlier launch could not send is still sent.
	StartUploadingWhatEarlierLaunchesLeft();
}

void UProtokitePlaytestSubsystem::HandleFlockLifecycleChanged()
{
	RefreshStatus();
}

void UProtokitePlaytestSubsystem::HandleFlockSessionStarted(const FString& SessionId)
{
	// Only a failure to reach Protokite is worth asking again: a session starting is when playtest work would
	// begin, and the game carries on whatever the answer is.
	if (ConfigState == EProtokitePlaytestConfigState::Unavailable)
	{
		ForgetPlaytestConfig();
		RefreshStatus();
	}
}

void UProtokitePlaytestSubsystem::HandleFlockSessionRegistered(const FString& SessionId, const FString& ServerSessionId)
{
	// The first one names the Protokite session. A later one comes from a new Flock session after time away or after
	// a sign-in, and changes nothing: the launch keeps the session it has.
	if (FirstFlockServerSessionId.IsEmpty())
	{
		FirstFlockServerSessionId = ServerSessionId;
	}
	StartPlaytestSessionWhenAllowed();
}

void UProtokitePlaytestSubsystem::RefreshStatus()
{
	// A config belongs to the Flock initialization it was fetched under. Once the Flock SDK is down, the next
	// initialization may carry another key or version, so the config and any reply still on its way are stale. So is
	// its first session: a session start sends the next initialization's headers, and has to name one of its sessions.
	const bool bFlockInitialized = Flock.IsValid() && Flock->IsInitialized();
	if (!bFlockInitialized)
	{
		if (ConfigState != EProtokitePlaytestConfigState::NotFetched)
		{
			ForgetPlaytestConfig();
		}
		FirstFlockServerSessionId.Empty();
	}

	// Decided again after starting the fetch: a transport that answers straight away finishes the fetch, and
	// applies its own status, inside StartPlaytestConfigFetch.
	if (DecideCurrentStatus() == EProtokitePlaytestStatus::FetchingPlaytestConfig
		&& ConfigState == EProtokitePlaytestConfigState::NotFetched)
	{
		StartPlaytestConfigFetch();
	}

	ApplyStatus(DecideCurrentStatus(), GetDefault<UProtokitePlaytestSettings>()->ProtokiteApiUrl,
		Flock.IsValid() ? Flock->GetGameVersionId() : FString());

	// After the status is applied, because all four read it. The question comes first: while it is unanswered the other
	// three have nothing to start, and the moment it is answered they are the ones that act on it.
	UpdateConsentQuestion();
	UpdatePerformanceTimeline();
	UpdateVideoRecording();
	StartPlaytestSessionWhenAllowed();
}

EProtokitePlaytestStatus UProtokitePlaytestSubsystem::DecideCurrentStatus() const
{
	const UProtokitePlaytestSettings* Settings = GetDefault<UProtokitePlaytestSettings>();

	FProtokitePlaytestStatusInputs Inputs;
	Inputs.bPlaytestingEnabled = Settings->bPlaytestingEnabled;
	Inputs.ProtokiteApiUrl = Settings->ProtokiteApiUrl;
	Inputs.bFlockInitialized = Flock.IsValid() && Flock->IsInitialized();
	Inputs.ConfigState = ConfigState;
	Inputs.bPlaytestNoLongerCollecting = bPlaytestNoLongerCollecting;
	Inputs.PlayerConsent = GetPlaytestConsent();
	return DecidePlaytestStatus(Inputs);
}

TSharedRef<FProtokiteClient> UProtokitePlaytestSubsystem::GetOrCreateProtokiteClient()
{
	if (!ProtokiteClient.IsValid())
	{
		// The same timeout and retry settings the Flock SDK uses for its own requests.
		const UFlockConfig* FlockSettings = GetDefault<UFlockConfig>();
		const TSharedRef<IFlockLogger> Logger = MakeShared<FProtokitePlaytestLogger>();
		const TSharedRef<FFlockHttpClient> HttpClient = TestHttpAdapter.IsValid()
			? MakeShared<FFlockHttpClient>(TestHttpAdapter.ToSharedRef(), Logger, FlockSettings->HttpTimeoutSeconds)
			: FFlockHttpClient::CreateDefault(FlockSettings->HttpTimeoutSeconds, Logger);

		FFlockRetryPolicy Policy;
		Policy.MaxRetries = FlockSettings->RetryMaxRetries;
		Policy.bUseJitter = FlockSettings->bRetryUseJitter;
		ProtokiteClient = MakeShared<FProtokiteClient>(HttpClient, TestRetryPolicy.Get(Policy), Logger);
	}
	return ProtokiteClient.ToSharedRef();
}

void UProtokitePlaytestSubsystem::StartPlaytestConfigFetch()
{
	const TSharedRef<FProtokiteClient> Client = GetOrCreateProtokiteClient();

	// Exactly the key and version the Flock SDK initialized with, so the playtest found is this build's.
	const TMap<FString, FString> RequestHeaders = Flock->GetRequestHeaders();
	const FString SentGameVersionId = RequestHeaders.FindRef(TEXT("X-Game-Version-ID"));

	ConfigState = EProtokitePlaytestConfigState::Fetching;
	const int32 TimesForgottenWhenSent = TimesConfigForgotten;
	const TWeakObjectPtr<UProtokitePlaytestSubsystem> WeakThis(this);
	ConfigFetchRequest = Client->FetchPlaytestConfig(GetDefault<UProtokitePlaytestSettings>()->ProtokiteApiUrl,
		RequestHeaders, [WeakThis, TimesForgottenWhenSent, SentGameVersionId](TFlockResult<FProtokitePlaytestConfig> Result)
		{
			UProtokitePlaytestSubsystem* Self = WeakThis.Get();
			// Stale: the Flock SDK shut down, or this subsystem finished, while the request was out.
			if (Self == nullptr || Self->TimesConfigForgotten != TimesForgottenWhenSent)
			{
				return;
			}

			Self->ConfigState = DecidePlaytestConfigState(Result, SentGameVersionId);
			const bool bLoaded = Self->ConfigState == EProtokitePlaytestConfigState::Loaded;
			Self->PlaytestConfig = bLoaded ? Result.Value : FProtokitePlaytestConfig();
			if (bLoaded)
			{
				Self->ConfigFailureMessage.Empty();
				const FString QuestionsItCannotTellApart = FProtokitePlaytestFormAnswers::DescribeQuestionsItCannotTellApart(Self->PlaytestConfig.Form);
				if (!QuestionsItCannotTellApart.IsEmpty())
				{
					UE_LOG(LogProtokitePlaytest, Warning, TEXT("The playtest's feedback form has questions whose ids differ only in letter case (%s). This plugin cannot keep their answers apart, so one answer would be sent for both: rename one of them on the dashboard."),
						*QuestionsItCannotTellApart);
				}
			}
			else if (Result.bSuccess)
			{
				Self->ConfigFailureMessage = FString::Printf(TEXT("It named Game Version ID %s; this build sent %s."),
					*Result.Value.FlockGameVersionId, *SentGameVersionId);
			}
			else
			{
				Self->ConfigFailureMessage = Result.Error.ToDisplayText();
			}
			Self->RefreshStatus();
		});
}

void UProtokitePlaytestSubsystem::ForgetPlaytestConfig()
{
	++TimesConfigForgotten;
	ConfigState = EProtokitePlaytestConfigState::NotFetched;
	PlaytestConfig = FProtokitePlaytestConfig();
	ConfigFailureMessage.Empty();

	// Its retries would otherwise keep sending an ended initialization's key to Protokite. Stopped after the count
	// has moved, so whatever the stopped request still answers is already stale.
	ConfigFetchRequest.Cancel();
	ConfigFetchRequest = FFlockRequestHandle();
}

void UProtokitePlaytestSubsystem::StartPlaytestSessionWhenAllowed()
{
	// One start per launch, whatever became of it.
	if (SessionState != EProtokitePlaytestSessionState::NotStarted || Status != EProtokitePlaytestStatus::Ready)
	{
		return;
	}
	if (FirstFlockServerSessionId.IsEmpty())
	{
		if (!bLoggedWaitingForFlockSession)
		{
			bLoggedWaitingForFlockSession = true;
			UE_LOG(LogProtokitePlaytest, Log, TEXT("The Protokite session starts once a Flock session reaches the server. A Flock session starts when a player signs in, with Analytics Enabled and Analytics Auto Start Session on (or a Start Session call), and consent granted when Analytics Require Explicit Consent is on: Project Settings > Plugins > Flock SDK Settings."));
		}
		return;
	}

	FString WhyNoIdentity;
	PlaytestIdentity = ResolvePlaytestIdentity(WhyNoIdentity);
	if (PlaytestIdentity.IsEmpty())
	{
		SessionState = EProtokitePlaytestSessionState::NoPlayerIdentity;
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("No Protokite session is started this launch, and nothing is sent: %s"), *WhyNoIdentity);
		return;
	}

	FProtokitePlaytestSessionStartRequest Request;
	Request.Identity = PlaytestIdentity;
	if (IsUsablePlaytestId(FirstFlockServerSessionId, ProtokitePlaytestSessionLimits::FlockSessionIdLength))
	{
		Request.FlockSessionId = FirstFlockServerSessionId;
	}
	else
	{
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("Flock session id '%s' cannot be sent to Protokite (longer than %d characters, or containing whitespace), so the Protokite session starts without naming its Flock session."),
			*FirstFlockServerSessionId, ProtokitePlaytestSessionLimits::FlockSessionIdLength);
	}
	const UGameInstance* GameInstance = GetGameInstance();
	// What the player allowed rides along as extra parameters, so the session says what it was collected under: a
	// session with no recording is then a player who asked for none rather than a build that went wrong.
	Request.DebugInfo = MakePlaytestSessionDebugInfo(MapNameOf(GameInstance != nullptr ? GameInstance->GetWorld() : nullptr),
		GetPlaytestConsent(), DoesThisBuildAskForPlaytestConsent());

	WarnIfExceptionsAreNotCaptured();

	// Kept for the end, which goes to the same place with the same headers even after the Flock SDK has shut down.
	PlaytestSessionApiUrl = GetDefault<UProtokitePlaytestSettings>()->ProtokiteApiUrl;
	PlaytestSessionHeaders = Flock->GetRequestHeaders();
	SessionState = EProtokitePlaytestSessionState::Starting;

	const TSharedRef<FProtokiteClient> Client = GetOrCreateProtokiteClient();
	const TWeakObjectPtr<UProtokitePlaytestSubsystem> WeakThis(this);
	const FString ApiUrl = PlaytestSessionApiUrl;
	const TMap<FString, FString> Headers = PlaytestSessionHeaders;
	const bool bSentSteamId = !PlaytestIdentity.SteamId.IsEmpty();
	const FString SentFlockSessionId = Request.FlockSessionId;
	Client->StartPlaytestSession(ApiUrl, Headers, Request,
		[WeakThis, Client, ApiUrl, Headers, bSentSteamId, SentFlockSessionId](TFlockResult<FProtokitePlaytestSessionStartResult> Result)
		{
			UProtokitePlaytestSubsystem* Self = WeakThis.Get();
			if (Self == nullptr || Self->Status == EProtokitePlaytestStatus::Stopped)
			{
				// The game instance shut down while the start was on its way. The launch is over, so a session that was
				// created is ended straight away rather than left in progress.
				if (Self != nullptr)
				{
					Self->SessionState = Result.bSuccess ? EProtokitePlaytestSessionState::Ended : EProtokitePlaytestSessionState::StartFailed;
					Self->PlaytestSessionId = Result.bSuccess ? Result.Value.SessionId : FString();
				}
				if (Result.bSuccess)
				{
					SendPlaytestSessionEnd(Client, ApiUrl, Headers, Result.Value.SessionId);
				}
				return;
			}

			if (Result.bSuccess)
			{
				Self->SessionState = EProtokitePlaytestSessionState::Started;
				Self->PlaytestSessionId = Result.Value.SessionId;
				UE_LOG(LogProtokitePlaytest, Log, TEXT("Protokite session %s started for this launch, with the player's %s and Flock session %s."),
					*Result.Value.SessionId, bSentSteamId ? TEXT("Steam id") : TEXT("device id"),
					SentFlockSessionId.IsEmpty() ? TEXT("(none)") : *SentFlockSessionId);
				Self->SaveVideoRecordingSession();
				return;
			}

			Self->SessionState = EProtokitePlaytestSessionState::StartFailed;
			if (Result.Error.StatusCode == 400)
			{
				// The playtest has closed. The status reports it, and turns playtesting off for the rest of the launch.
				Self->bPlaytestNoLongerCollecting = true;
				Self->RefreshStatus();
				return;
			}
			UE_LOG(LogProtokitePlaytest, Warning, TEXT("No Protokite session was started, and none is tried again this launch, because a start that reached Protokite may already have created one. %s"),
				*Result.Error.ToDisplayText());
		});
}

FProtokitePlaytestIdentity UProtokitePlaytestSubsystem::ResolvePlaytestIdentity(FString& OutWhyNone) const
{
	FProtokitePlaytestIdentity Identity;

	const FProtokitePlaytestRunningSteamAccount Steam = TestSteamAccountReader ? TestSteamAccountReader() : ReadRunningSteamAccount();
	if (IsUsablePlaytestId(Steam.Id, ProtokitePlaytestIdentityLimits::SteamIdLength))
	{
		Identity.SteamId = Steam.Id;
		// A name is shown to people, so spaces are fine. One too long for Protokite is left out rather than cut short.
		if (Steam.Nickname.Len() <= ProtokitePlaytestIdentityLimits::PlayerNameLength)
		{
			Identity.PlayerName = Steam.Nickname;
		}
		return Identity;
	}
	if (!Steam.Id.IsEmpty())
	{
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("The running Steam subsystem gave a Steam id Protokite cannot take (longer than %d characters, or containing whitespace): '%s'. This install's device id is sent instead."),
			ProtokitePlaytestIdentityLimits::SteamIdLength, *Steam.Id);
	}

	const FProtokitePlaytestDeviceIdFile File(TestDeviceIdFilePath.IsEmpty() ? FProtokitePlaytestDeviceIdFile::GetDefaultPath() : TestDeviceIdFilePath);
	switch (File.ReadOrCreate(Identity.DeviceId))
	{
	case EProtokitePlaytestDeviceIdFileResult::Read:
	case EProtokitePlaytestDeviceIdFileResult::Created:
		break;
	case EProtokitePlaytestDeviceIdFileResult::Replaced:
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("The device id file %s did not hold a device id, so a new one replaced it. Protokite sees this install as a new player from now on."),
			*File.GetPath());
		break;
	case EProtokitePlaytestDeviceIdFileResult::Unreadable:
		OutWhyNone = FString::Printf(TEXT("no Steam subsystem gave a Steam id, and the device id file %s exists but could not be read, so it was left alone."),
			*File.GetPath());
		break;
	case EProtokitePlaytestDeviceIdFileResult::CouldNotSave:
		OutWhyNone = FString::Printf(TEXT("no Steam subsystem gave a Steam id, and no device id could be saved to %s. An id that changed on every launch would show one player as many."),
			*File.GetPath());
		break;
	}
	return Identity;
}

void UProtokitePlaytestSubsystem::ApplyStatus(EProtokitePlaytestStatus NewStatus, const FString& ProtokiteApiUrl,
	const FString& GameVersionId)
{
	if (bStatusDecided && NewStatus == Status)
	{
		return;
	}
	Status = NewStatus;
	bStatusDecided = true;

	// Loud only when playtesting is turned on: a setting or a refusal that stops it is a warning, and waiting,
	// fetching or being ready is logged. Turned off is the chosen state of every build that is not a playtest
	// build, so it stays at Verbose, and so does shutting down with the game instance.
	// The key that opens the form only listens while there is a form to open.
	UpdateFeedbackFormKeyWatcher();

	const FString Description = DescribePlaytestStatus(Status);
	switch (Status)
	{
	case EProtokitePlaytestStatus::TurnedOff:
	case EProtokitePlaytestStatus::Stopped:
		UE_LOG(LogProtokitePlaytest, Verbose, TEXT("%s"), *Description);
		break;
	case EProtokitePlaytestStatus::ProtokiteApiUrlMissing:
	case EProtokitePlaytestStatus::PlaytestNoLongerCollecting:
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("%s"), *Description);
		break;
	case EProtokitePlaytestStatus::ProtokiteApiUrlUnusable:
		// Quoted, so a leading or trailing space is visible in the log.
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("%s Current value: '%s'."), *Description, *ProtokiteApiUrl);
		break;
	case EProtokitePlaytestStatus::FetchingPlaytestConfig:
	case EProtokitePlaytestStatus::WaitingForFlock:
	case EProtokitePlaytestStatus::WaitingForPlayerConsent:
		UE_LOG(LogProtokitePlaytest, Log, TEXT("%s"), *Description);
		break;
	case EProtokitePlaytestStatus::PlayerRefusedPlaytest:
		// Not a warning: nothing is wrong, and the answer is the player's to give.
		UE_LOG(LogProtokitePlaytest, Log, TEXT("%s"), *Description);
		break;
	case EProtokitePlaytestStatus::PlaytestNotLinked:
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("%s Game Version ID: %s."), *Description, *GameVersionId);
		break;
	case EProtokitePlaytestStatus::ProtokiteRefusedApiKey:
	case EProtokitePlaytestStatus::PlaytestConfigUnavailable:
	case EProtokitePlaytestStatus::PlaytestConfigForAnotherVersion:
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("%s %s"), *Description, *ConfigFailureMessage);
		break;
	case EProtokitePlaytestStatus::Ready:
	{
		TArray<FString> FeaturesOn;
		for (const TPair<FString, bool>& Feature : PlaytestConfig.Features)
		{
			if (Feature.Value)
			{
				FeaturesOn.Add(Feature.Key);
			}
		}
		FeaturesOn.Sort();
		const FString FeatureList = FeaturesOn.Num() > 0 ? FString::Join(FeaturesOn, TEXT(", ")) : FString(TEXT("none"));
		// What the playtest asks for and what the player allowed are both named: a feature listed here still collects
		// nothing when their answer leaves it out, and that is the first thing to look at when it does not.
		UE_LOG(LogProtokitePlaytest, Log, TEXT("%s Playtest: %s. Features on: %s. Player consent: %s%s. Protokite API URL: %s. Game Version ID: %s."),
			*Description, *PlaytestConfig.TestId, *FeatureList, *ProtokitePlaytestConsent::ToWire(GetPlaytestConsent()),
			DoesThisBuildAskForPlaytestConsent() ? TEXT("") : TEXT(" (the player was not asked)"), *ProtokiteApiUrl, *GameVersionId);
		break;
	}
	}
}

bool UProtokitePlaytestSubsystem::RecordPlaytestEvent(const FString& EventName, const FFlockCommandData& Properties)
{
	// The status and the Flock SDK are game-thread state. A game recording from a worker thread has done nothing wrong,
	// so the call is handed over rather than refused, which means it cannot be judged here.
	if (!IsInGameThread())
	{
		const TWeakObjectPtr<UProtokitePlaytestSubsystem> WeakThis(this);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, EventName, Properties]()
		{
			if (UProtokitePlaytestSubsystem* Self = WeakThis.Get())
			{
				Self->RecordPlaytestEvent(EventName, Properties);
			}
		});
		return true;
	}

	// One sender per name, so a chart built on the plugin's own events never counts one of the game's.
	if (EventName.Equals(ProtokitePlaytestEvents::PerformanceWindow, ESearchCase::CaseSensitive)
		|| EventName.Equals(ProtokitePlaytestEvents::LevelLoaded, ESearchCase::CaseSensitive))
	{
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("Playtest event '%s' refused: the plugin sends events with that name itself."), *EventName);
		return false;
	}
	return SendPlaytestEvent(EventName, Properties);
}

void UProtokitePlaytestSubsystem::WarnIfExceptionsAreNotCaptured() const
{
	if (IsPlaytestFeatureEnabled(ProtokitePlaytestFeatures::ExceptionCapturing) && Flock.IsValid()
		&& !Flock->GetExceptionCaptureCoverage().bEnabled)
	{
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("This playtest turns exception capturing on, but the Flock SDK is not capturing exceptions, so no fault from this launch is reported. Turn on Analytics Capture Exceptions: Project Settings > Plugins > Flock SDK Settings."));
	}
}

void UProtokitePlaytestSubsystem::UpdatePerformanceTimeline()
{
	const bool bHeavyAnalyticsOn = IsPlaytestFeatureEnabled(ProtokitePlaytestFeatures::HeavyAnalytics);
	const bool bFlockAnalyticsOn = Flock.IsValid() && Flock->GetAnalyticsProvider() != nullptr;
	if (bHeavyAnalyticsOn && !bFlockAnalyticsOn && !bLoggedHeavyAnalyticsWithoutFlockAnalytics)
	{
		bLoggedHeavyAnalyticsWithoutFlockAnalytics = true;
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("This playtest turns heavy analytics on, but the Flock SDK's analytics is off, so no performance or playtest event is recorded. Turn on Analytics Enabled: Project Settings > Plugins > Flock SDK Settings."));
	}

	const bool bShouldMeasure = bHeavyAnalyticsOn && bFlockAnalyticsOn;
	if (bShouldMeasure == PerformancePump.IsRunning())
	{
		return;
	}

	if (bShouldMeasure)
	{
		// The timeline and the level-load start are already clear: every stop clears them, and nothing adds to them
		// while stopped.
		const UGameInstance* GameInstance = GetGameInstance();
		CurrentMapName = MapNameOf(GameInstance != nullptr ? GameInstance->GetWorld() : nullptr);

		PerformanceFrameHandle = PerformancePump.OnTick.AddUObject(this, &UProtokitePlaytestSubsystem::HandlePerformanceFrame);
		BackgroundChangedHandle = PerformancePump.OnBackgroundChanged.AddUObject(this, &UProtokitePlaytestSubsystem::HandleBackgroundChanged);
		PreLoadMapHandle = FCoreUObjectDelegates::PreLoadMapWithContext.AddUObject(this, &UProtokitePlaytestSubsystem::HandlePreLoadMap);
		PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &UProtokitePlaytestSubsystem::HandlePostLoadMap);
		PerformancePump.Start();

		UE_LOG(LogProtokitePlaytest, Log, TEXT("Heavy analytics is on: a performance window for every %.0f seconds of play, and every level load, go to the Flock SDK as '%s' events."),
			FProtokitePlaytestPerformanceTimeline::WindowSeconds, ProtokitePlaytestEvents::Category);
		return;
	}

	PerformancePump.Stop();
	PerformancePump.OnTick.Remove(PerformanceFrameHandle);
	PerformancePump.OnBackgroundChanged.Remove(BackgroundChangedHandle);
	FCoreUObjectDelegates::PreLoadMapWithContext.Remove(PreLoadMapHandle);
	FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapHandle);
	PerformanceFrameHandle.Reset();
	BackgroundChangedHandle.Reset();
	PreLoadMapHandle.Reset();
	PostLoadMapHandle.Reset();

	// A window cut short would read as a short stretch of play, so it is dropped rather than sent.
	PerformanceTimeline.Reset();
	LevelLoadStartSeconds = -1.0;
	UE_LOG(LogProtokitePlaytest, Verbose, TEXT("Heavy analytics stopped, and the unfinished performance window was dropped."));
}

void UProtokitePlaytestSubsystem::HandlePerformanceFrame(float FrameSeconds)
{
	FProtokitePlaytestPerformanceWindow Window;
	if (!PerformanceTimeline.AddFrame(FrameSeconds, GetEngineFrameNumber(), Window))
	{
		return;
	}

	// Frame times and memory only. The session's length, pauses and frame rate are the Flock SDK's, and are never sent twice.
	FFlockCommandData Properties;
	Properties.Set(TEXT("window_seconds"), RoundToHundredths(Window.Seconds))
		.Set(TEXT("frames"), Window.Frames)
		.Set(TEXT("median_frame_time_ms"), RoundToHundredths(Window.MedianFrameTimeMs))
		.Set(TEXT("frame_time_95th_percentile_ms"), RoundToHundredths(Window.FrameTime95thPercentileMs))
		.Set(TEXT("frame_time_99th_percentile_ms"), RoundToHundredths(Window.FrameTime99thPercentileMs))
		.Set(TEXT("hitches"), Window.Hitches)
		.Set(TEXT("hitch_threshold_ms"), RoundToHundredths(Window.HitchThresholdMs))
		.Set(TEXT("memory_used_mb"), Window.MemoryUsedMb)
		.Set(TEXT("memory_peak_mb"), Window.MemoryPeakMb);
	if (!CurrentMapName.IsEmpty())
	{
		Properties.Set(TEXT("map"), CurrentMapName);
	}
	SendPlaytestEvent(ProtokitePlaytestEvents::PerformanceWindow, Properties);
}

void UProtokitePlaytestSubsystem::HandleBackgroundChanged(bool /*bBackgrounded*/)
{
	// The ticker passes on no frames while the game is in the background, so the first frame time after the return is the
	// one that can carry the time away. Marking it when the game leaves or when it comes back leaves out that same frame.
	PerformanceTimeline.LeaveOutNextFrame();
}

void UProtokitePlaytestSubsystem::HandlePreLoadMap(const FWorldContext& WorldContext, const FString& MapName)
{
	NoteLevelLoadStarted(WorldContext.OwningGameInstance);
}

void UProtokitePlaytestSubsystem::HandlePostLoadMap(UWorld* LoadedWorld)
{
	if (LoadedWorld == nullptr)
	{
		// A load that failed names no world. The frame time it stretched is still not play.
		if (LevelLoadStartSeconds >= 0.0)
		{
			LevelLoadStartSeconds = -1.0;
			PerformanceTimeline.LeaveOutFirstFrameAfter(GetEngineFrameNumber());
		}
		return;
	}
	NoteLevelLoaded(LoadedWorld->GetGameInstance(), MapNameOf(LoadedWorld));
}

void UProtokitePlaytestSubsystem::NoteLevelLoadStarted(const UGameInstance* LoadingGameInstance)
{
	// The engine announces every game instance's loads; while playing in the editor, several run side by side.
	if (LoadingGameInstance == nullptr || LoadingGameInstance != GetGameInstance())
	{
		return;
	}
	LevelLoadStartSeconds = FPlatformTime::Seconds();
}

void UProtokitePlaytestSubsystem::NoteLevelLoaded(const UGameInstance* LoadingGameInstance, const FString& MapName)
{
	if (LoadingGameInstance == nullptr || LoadingGameInstance != GetGameInstance())
	{
		return;
	}

	FFlockCommandData Properties;
	Properties.Set(TEXT("map"), MapName);
	if (!CurrentMapName.IsEmpty())
	{
		Properties.Set(TEXT("previous_map"), CurrentMapName);
	}
	// A load that held the game up reports how long it took, and the frame time that carries it is left out. The load
	// runs inside the engine's update, before this engine frame hands over a frame time measured before the load began,
	// so the next engine frame's time is the stretched one. A seamless travel keeps frames coming while it loads, so it
	// has neither.
	if (LevelLoadStartSeconds >= 0.0)
	{
		Properties.Set(TEXT("load_seconds"), RoundToHundredths(FPlatformTime::Seconds() - LevelLoadStartSeconds));
		LevelLoadStartSeconds = -1.0;
		PerformanceTimeline.LeaveOutFirstFrameAfter(GetEngineFrameNumber());
	}
	CurrentMapName = MapName;
	SendPlaytestEvent(ProtokitePlaytestEvents::LevelLoaded, Properties);
}

bool UProtokitePlaytestSubsystem::SendPlaytestEvent(const FString& EventName, const FFlockCommandData& Properties)
{
	if (!IsPlaytestFeatureEnabled(ProtokitePlaytestFeatures::HeavyAnalytics))
	{
		return false;
	}
	FFlockAnalyticsProvider* Analytics = Flock.IsValid() ? Flock->GetAnalyticsProvider() : nullptr;
	return Analytics != nullptr && Analytics->TrackEvent(EventName, Properties, ProtokitePlaytestEvents::Category);
}

uint64 UProtokitePlaytestSubsystem::GetEngineFrameNumber() const
{
	return TestEngineFrameNumberReader ? TestEngineFrameNumberReader() : GFrameCounter;
}

bool UProtokitePlaytestSubsystem::IsRecordingVideo() const
{
	return VideoRecording.IsValid() && VideoRecording->IsCapturing();
}

bool UProtokitePlaytestSubsystem::StopVideoRecording()
{
	if (!IsRecordingVideo())
	{
		return false;
	}
	VideoRecording->StopCapturing(EProtokitePlaytestVideoStopReason::StoppedByGame);
	return true;
}

bool UProtokitePlaytestSubsystem::CanSendTheRecording() const
{
	// The same conditions SaveVideoRecordingSession writes a session.json under, because that file is the only thing
	// that gives a finished recording somewhere to go. Asking anything weaker offers a player a button that stops
	// their recording and sends nothing.
	return IsRecordingVideo()
		&& VideoRecordingRun.IsValid()
		&& VideoRecordingRun->GetKind() == EProtokitePlaytestRecordingKind::Playtest
		&& !PlaytestSessionId.IsEmpty();
}

bool UProtokitePlaytestSubsystem::StopVideoRecordingAndUploadIt()
{
	if (!IsRecordingVideo())
	{
		return false;
	}
	// Finished here and now rather than left to the writer thread, because the upload needs the whole file: a link is
	// asked for only once there is a finished recording to send.
	FinishVideoRecordingNow(EProtokitePlaytestVideoStopReason::StoppedByGame);
	return true;
}

bool UProtokitePlaytestSubsystem::StartTestVideoRecording(double Seconds)
{
#if UE_BUILD_SHIPPING
	return false;
#else
	if (Seconds <= 0.0)
	{
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("No test video is recorded: it needs a length above 0 seconds, and %.2f was asked for."), Seconds);
		return false;
	}
	// The launch's one recording belongs to the playtest; a test video would take its place and cut it short.
	if (IsPlaytestFeatureEnabled(ProtokitePlaytestFeatures::VideoRecording))
	{
		UE_LOG(LogProtokitePlaytest, Log, TEXT("No test video is recorded: this launch records video for its playtest."));
		return false;
	}
	if (VideoRecording.IsValid() || bVideoRecordingStartedThisLaunch)
	{
		UE_LOG(LogProtokitePlaytest, Log, TEXT("No test video is recorded: %s, and a launch records one video."),
			IsRecordingVideo() ? TEXT("a video is already being recorded") : TEXT("this launch has already recorded one"));
		return false;
	}
	if (!VideoRecordingUnavailableReason.IsEmpty())
	{
		UE_LOG(LogProtokitePlaytest, Log, TEXT("No test video is recorded: %s."), *VideoRecordingUnavailableReason);
		return false;
	}

	bTestVideoRequested = true;
	TestVideoSeconds = Seconds;
	UpdateVideoRecording();
	if (VideoRecording.IsValid())
	{
		return true;
	}
	if (IsWaitingToStartVideoRecording())
	{
		UE_LOG(LogProtokitePlaytest, Log, TEXT("The test video starts once this game instance has a viewport to record."));
		return true;
	}
	return false;
#endif
}

void UProtokitePlaytestSubsystem::WaitUntilVideoWrittenForTesting()
{
	if (VideoRecording.IsValid())
	{
		VideoRecording->WaitUntilWritten();
	}
}

bool UProtokitePlaytestSubsystem::IsVideoRecordingWanted() const
{
	// A subsystem that has shut down records nothing, whatever asked for it.
	if (Status == EProtokitePlaytestStatus::Stopped)
	{
		return false;
	}
	return IsPlaytestFeatureEnabled(ProtokitePlaytestFeatures::VideoRecording) || bTestVideoRequested || IsRecordVideoInPlayInEditorOn();
}

bool UProtokitePlaytestSubsystem::IsRecordVideoInPlayInEditorOn() const
{
#if WITH_EDITOR
	const UGameInstance* GameInstance = GetGameInstance();
	if (!GIsEditor || GEngine == nullptr || GameInstance == nullptr || !GetDefault<UProtokitePlaytestLocalSettings>()->bRecordVideoInPlayInEditor)
	{
		return false;
	}
	// Only this game instance's own world counts: a game run from the editor binary with -game is not playing in the editor.
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.OwningGameInstance == GameInstance)
		{
			return Context.WorldType == EWorldType::PIE;
		}
	}
#endif
	return false;
}

bool UProtokitePlaytestSubsystem::IsWaitingToStartVideoRecording() const
{
	return IsVideoRecordingWanted() && !bVideoRecordingStartedThisLaunch && VideoRecordingUnavailableReason.IsEmpty();
}

void UProtokitePlaytestSubsystem::UpdateVideoRecording()
{
	if (VideoRecording.IsValid())
	{
		// A recording that has already stopped ignores this.
		if (!IsVideoRecordingWanted())
		{
			VideoRecording->StopCapturing(EProtokitePlaytestVideoStopReason::PlaytestStopped);
		}
	}
	else if (IsWaitingToStartVideoRecording())
	{
		StartVideoRecordingWhenPossible();
	}
	UpdateVideoPump();
}

void UProtokitePlaytestSubsystem::StartVideoRecordingWhenPossible()
{
	FProtokitePlaytestVideoSettings Settings = FProtokitePlaytestVideoSettings::FromProjectSettings(*GetDefault<UProtokitePlaytestSettings>());
	if (bTestVideoRequested)
	{
		Settings.MaxSeconds = FMath::Min(Settings.MaxSeconds, TestVideoSeconds);
	}

	FString WhyNot;
	const TSharedPtr<IProtokitePlaytestVideoFrameSource> Source = TestVideoFrameSourceFactory
		? TestVideoFrameSourceFactory(Settings.MaxVideoSize, WhyNot)
		: FProtokitePlaytestGameViewportFrameSource::Create(GetGameInstance(), Settings.MaxVideoSize, WhyNot);
	if (!Source.IsValid())
	{
		// No reason means the game viewport is not there yet, and the next frame asks again. A reason does not change
		// during a launch, so it is logged once and never asked again.
		if (!WhyNot.IsEmpty())
		{
			VideoRecordingUnavailableReason = WhyNot;
			UE_LOG(LogProtokitePlaytest, Warning, TEXT("No video is recorded this launch: %s. Everything else in the playtest carries on."), *WhyNot);
		}
		return;
	}

	const bool bForThePlaytest = IsPlaytestFeatureEnabled(ProtokitePlaytestFeatures::VideoRecording);
	const FString WhyRecording = bForThePlaytest ? TEXT("for the playtest")
		: bTestVideoRequested ? TEXT("as a test video") : TEXT("because Record Video In Play In Editor is on");
	const FString RecordingsFolder = GetVideoRecordingsFolder();

	// One attempt per launch, whether or not it starts: a second recording would replace the first when uploaded.
	bVideoRecordingStartedThisLaunch = true;

	const FProtokitePlaytestRecordingsRoom Room = FProtokitePlaytestRecordingsFolder::MakeRoom(RecordingsFolder, Settings.DiskBudgetBytes, Settings.BytesToMakeRoomFor());
	const int64 BudgetMb = Settings.DiskBudgetBytes / (1024 * 1024);
	for (const FString& Deleted : Room.PlaytestRecordingsDeleted)
	{
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("Deleted %s, the oldest playtest recording not yet uploaded, to make room for this launch's recording inside Recordings Disk Budget (MB), %lld MB."),
			*Deleted, BudgetMb);
	}
	for (const FString& Deleted : Room.TestVideosDeleted)
	{
		UE_LOG(LogProtokitePlaytest, Log, TEXT("Deleted the test video %s, the oldest kept, to make room for this launch's recording inside Recordings Disk Budget (MB), %lld MB."),
			*Deleted, BudgetMb);
	}
	if (Room.BytesLeftInBudget < FProtokitePlaytestRecordingsFolder::SmallestRoomForARecording)
	{
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("No video is recorded this launch: the recordings in %s take %.1f MB of Recordings Disk Budget (MB), %lld MB, and none of them can be deleted now, because their games are still running or another program is using them. The budget is in Project Settings > Plugins > Protokite Playtest Settings."),
			*RecordingsFolder, Room.BytesUsed / (1024.0 * 1024.0), BudgetMb);
		return;
	}

	// The recording may grow as far as the budget has left, up to its size limit, and its folder says how far, so a launch
	// making room meanwhile counts it at that size.
	const int64 SizeLimitBytes = Settings.MaxBytes;
	Settings.MaxBytes = FMath::Min(Settings.MaxBytes, Room.BytesLeftInBudget);
	FString StartError;
	VideoRecordingRun = FProtokitePlaytestRecordingRun::Create(RecordingsFolder,
		bForThePlaytest ? EProtokitePlaytestRecordingKind::Playtest : EProtokitePlaytestRecordingKind::TestVideo, Settings.MaxBytes, StartError);
	if (VideoRecordingRun.IsValid())
	{
		VideoRecording = FProtokitePlaytestVideoRecording::Start(Source.ToSharedRef(), Settings, VideoRecordingRun->GetVideoFilePath(), StartError,
			TestBeforeEachVideoEncode, TestBeforeEachVideoWrite);
	}
	if (!VideoRecording.IsValid())
	{
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("No video is recorded this launch: the recording could not start, because %s."), *StartError);
		return;
	}

	const FIntPoint FrameSize = Source->GetFrameSize();
	const FString CutShorter = Settings.MaxBytes < SizeLimitBytes
		? FString::Printf(TEXT(" (Recording Size Limit (MB) is %lld MB, but Recordings Disk Budget (MB) has only this much left)"), SizeLimitBytes / (1024 * 1024))
		: FString();
	UE_LOG(LogProtokitePlaytest, Log, TEXT("Recording video %s to %s, at %dx%d and %d frames a second. It stops for good after %.0f seconds of play, before the file passes %.1f MB%s, or when the game stops it."),
		*WhyRecording, *VideoRecordingRun->GetVideoFilePath(), FrameSize.X, FrameSize.Y, Settings.FramesPerSecond, Settings.MaxSeconds,
		Settings.MaxBytes / (1024.0 * 1024.0), *CutShorter);

	// The Protokite session may have started while the recording waited for the game viewport.
	SaveVideoRecordingSession();
}

FString UProtokitePlaytestSubsystem::GetVideoRecordingsFolder() const
{
	return TestVideoRecordingFolder.IsEmpty() ? FProtokitePlaytestRecordingsFolder::GetDefaultPath() : TestVideoRecordingFolder;
}

void UProtokitePlaytestSubsystem::FinishWhatEndedRunsLeftInRecordingsFolder()
{
	// Finishing a recording a crash cut off reads every frame's header: measured, 0.4 to 2.1 s for an hour's recording. So
	// it runs on a thread of its own and never holds up the game's start. A run it works on is held by it meanwhile, so a
	// recording that starts before it is done counts that run and never deletes it.
	const FString RecordingsFolder = GetVideoRecordingsFolder();
	RecordingsFolderFinished = Async(EAsyncExecution::Thread, [RecordingsFolder, BeforeFinishing = TestBeforeFinishingWhatEndedRunsLeft]()
	{
		if (BeforeFinishing)
		{
			BeforeFinishing();
		}
		const FProtokitePlaytestWhatEndedRunsLeft Result = FProtokitePlaytestRecordingsFolder::FinishWhatEndedRunsLeft(RecordingsFolder);
		if (Result.RecordingsWaitingToUpload > 0 || Result.InterruptedRecordingsFinished > 0 || Result.RecordingsWithoutASessionDeleted > 0)
		{
			UE_LOG(LogProtokitePlaytest, Log, TEXT("Earlier launches left recordings in %s: %d playtest recordings not yet uploaded are kept, waiting to be uploaded; %d recordings cut off when their game ended were finished with every whole frame they held; and %d playtest recordings were deleted, because no Protokite session started for them to be uploaded to."),
				*RecordingsFolder, Result.RecordingsWaitingToUpload, Result.InterruptedRecordingsFinished, Result.RecordingsWithoutASessionDeleted);
		}
		if (Result.FilesLeftForTheNextLaunch.Num() > 0)
		{
			UE_LOG(LogProtokitePlaytest, Warning, TEXT("%d recording files that earlier launches left could not be finished or deleted, and the next launch tries again (is another program using them?): %s."),
				Result.FilesLeftForTheNextLaunch.Num(), *FString::Join(Result.FilesLeftForTheNextLaunch, TEXT(", ")));
		}
	});
}

void UProtokitePlaytestSubsystem::WaitUntilRecordingsFolderFinishedForTesting()
{
	if (RecordingsFolderFinished.IsValid())
	{
		RecordingsFolderFinished.Wait();
	}
}

void UProtokitePlaytestSubsystem::SaveVideoRecordingSession()
{
	// Only a playtest recording is uploaded, and only to a session that was started; one the game has ended since still counts.
	const bool bSessionWasStarted = SessionState == EProtokitePlaytestSessionState::Started || SessionState == EProtokitePlaytestSessionState::Ended;
	if (!VideoRecordingRun.IsValid() || VideoRecordingRun->GetKind() != EProtokitePlaytestRecordingKind::Playtest || !bSessionWasStarted
		|| PlaytestSessionId.IsEmpty())
	{
		return;
	}

	// The address and Game Version ID the session started with, whatever the Flock SDK was initialized with since: Protokite
	// finds the session's playtest from that version. The API key is never saved; a later launch sends its own.
	FProtokitePlaytestRecordingSession Session;
	Session.ProtokiteSessionId = PlaytestSessionId;
	Session.ProtokiteApiUrl = PlaytestSessionApiUrl;
	Session.FlockGameVersionId = PlaytestSessionHeaders.FindRef(TEXT("X-Game-Version-ID"));
	FString Error;
	if (!VideoRecordingRun->SaveSession(Session, Error))
	{
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("The Protokite session this launch's recording belongs to could not be saved beside it, so a later launch cannot upload the recording: %s."),
			*Error);
	}
}

void UProtokitePlaytestSubsystem::UpdateVideoPump()
{
	// Ticks while a recording captures or its file is being written, and while one waits for the game viewport.
	const bool bShouldTick = VideoRecording.IsValid() || IsWaitingToStartVideoRecording();
	if (bShouldTick == VideoPump.IsRunning())
	{
		return;
	}
	if (bShouldTick)
	{
		VideoFrameHandle = VideoPump.OnTick.AddUObject(this, &UProtokitePlaytestSubsystem::HandleVideoFrame);
		VideoBackgroundChangedHandle = VideoPump.OnBackgroundChanged.AddUObject(this, &UProtokitePlaytestSubsystem::HandleVideoBackgroundChanged);
		VideoPump.Start();
		return;
	}
	VideoPump.Stop();
	VideoPump.OnTick.Remove(VideoFrameHandle);
	VideoPump.OnBackgroundChanged.Remove(VideoBackgroundChangedHandle);
	VideoFrameHandle.Reset();
	VideoBackgroundChangedHandle.Reset();
}

void UProtokitePlaytestSubsystem::HandleVideoFrame(float FrameSeconds)
{
	if (!VideoRecording.IsValid())
	{
		UpdateVideoRecording();
		return;
	}
	// A recording that has stopped capturing takes no frame; it is only waited on.
	const TOptional<EProtokitePlaytestVideoStopReason> StopReason = VideoRecording->AddFrame(FrameSeconds);
	if (StopReason.IsSet())
	{
		VideoRecording->StopCapturing(StopReason.GetValue());
	}
	if (VideoRecording.IsValid() && VideoRecording->HasFinishedWriting())
	{
		ApplyFinishedVideoRecording();
	}
}

void UProtokitePlaytestSubsystem::HandleVideoBackgroundChanged(bool /*bBackgrounded*/)
{
	// The ticker passes on no frames while the game is away, so the first frame time after the return carries that time.
	if (VideoRecording.IsValid())
	{
		VideoRecording->LeaveOutNextFrame();
	}
}

void UProtokitePlaytestSubsystem::ApplyFinishedVideoRecording()
{
	const FProtokitePlaytestVideoRecordingSummary Summary = VideoRecording->GetSummary();
	VideoRecording.Reset();

	if (!Summary.Error.IsEmpty() && Summary.FilePath.IsEmpty())
	{
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("The video recording could not be written, so no finished file was kept: %s. It had stopped because %s."),
			*Summary.Error, *DescribeVideoStopReason(Summary.StopReason));
	}
	else if (!Summary.Error.IsEmpty())
	{
		FinishedVideoRecordingPath = Summary.FilePath;
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("The video recording could not be written to the end: %s. The %d frames written before, %.1f seconds, are kept in %s."),
			*Summary.Error, Summary.FramesWritten, Summary.VideoSeconds, *Summary.FilePath);
	}
	else if (Summary.FilePath.IsEmpty())
	{
		UE_LOG(LogProtokitePlaytest, Log, TEXT("The video recording stopped because %s before any frame was captured, so no file was kept."),
			*DescribeVideoStopReason(Summary.StopReason));
	}
	else
	{
		FinishedVideoRecordingPath = Summary.FilePath;
		UE_LOG(LogProtokitePlaytest, Log, TEXT("Video saved to %s: %.1f seconds, %d frames, %.1f MB. It stopped because %s. Encoding took %.2f ms a frame on average and %.2f ms at most; a frame waited at most %.0f ms to be encoded, and one write took at most %.2f ms; %d frames were dropped because encoding fell behind, %d because writing the file fell behind, and %d capture times passed while earlier frames were still on their way. The file is VP9 video in a WebM file, which a browser plays with nothing installed."),
			*Summary.FilePath, Summary.VideoSeconds, Summary.FramesWritten, Summary.BytesWritten / (1024.0 * 1024.0),
			*DescribeVideoStopReason(Summary.StopReason), Summary.AverageEncodeMs, Summary.LongestEncodeMs,
			Summary.LongestWaitToEncodeMs, Summary.LongestWriteMs, Summary.FramesDroppedBecauseEncodingFellBehind,
			Summary.FramesDroppedBecauseWritingFellBehind, Summary.FramesNotReadyInTime);
	}
	// The ticker is settled first, so whoever hears how the upload went finds the recording fully put away.
	UpdateVideoPump();
	UploadThisLaunchsRecording();
}

TSharedRef<FProtokitePlaytestRecordingUploads> UProtokitePlaytestSubsystem::GetOrCreateRecordingUploads()
{
	if (!RecordingUploads.IsValid())
	{
		const TSharedRef<IFlockLogger> Logger = MakeShared<FProtokitePlaytestLogger>();
		const TSharedRef<IFlockFileUploader> Uploader = TestFileUploader.IsValid()
			? TestFileUploader.ToSharedRef()
			: FlockCreateHttpFileUploader(Logger);
		RecordingUploads = MakeShared<FProtokitePlaytestRecordingUploads>(GetOrCreateProtokiteClient(), Uploader, Logger);
		// A whole recording is up to a gigabyte and a half, so it gets no request timeout: the ordinary thirty seconds
		// would end every upload that matters.
		RecordingUploads->UploadTimeoutSeconds = 0.f;
	}
	return RecordingUploads.ToSharedRef();
}

void UProtokitePlaytestSubsystem::UploadThisLaunchsRecording()
{
	// Every way out of here says why. They were all silent once, and a player who had asked for their recording was
	// told it was on its way while nothing happened and the log held no reason -- which is worse than either.
	if (!VideoRecordingRun.IsValid() || VideoRecordingRun->GetKind() != EProtokitePlaytestRecordingKind::Playtest)
	{
		// Not raised: there was no playtest recording, so nobody is waiting to hear about one.
		UE_LOG(LogProtokitePlaytest, Verbose, TEXT("There is no playtest recording to upload; a test video is never uploaded."));
		return;
	}

	// Each way out below also raises OnRecordingUploadFinished, so whoever is waiting on this recording hears the answer
	// instead of waiting for one that never comes.
	const auto NotUploadedNow = [this](const FString& WhyNot)
	{
		OnRecordingUploadFinished.Broadcast(false, WhyNot);
	};

	// Checked before the game is closing, not after: a recording kept through a shutdown is one a later launch sends,
	// and the session saved beside it is what tells that launch where. Only deleting it now honours the answer.
	if (!ProtokitePlaytestConsent::AllowsVideoRecording(GetPlaytestConsent()))
	{
		TArray<FString> FilesLeft;
		const bool bDeleted = VideoRecordingRun->DeleteEverything(FilesLeft);
		FinishedVideoRecordingPath.Empty();
		if (bDeleted)
		{
			VideoRecordingRun.Reset();
			UE_LOG(LogProtokitePlaytest, Log, TEXT("The player asked for the screen not to be recorded, so what had been recorded this launch was deleted instead of uploaded."));
		}
		else
		{
			UE_LOG(LogProtokitePlaytest, Warning, TEXT("The player asked for the screen not to be recorded, and what had been recorded this launch could not all be deleted: %s. It is not uploaded."),
				*FString::Join(FilesLeft, TEXT(", ")));
		}
		if (!bDeinitializing)
		{
			// Same rule as every other way out: not raised while the game is closing, because its handlers would run
			// against objects being torn down.
			NotUploadedNow(TEXT("The player asked for the screen not to be recorded, so the recording was deleted."));
		}
		return;
	}

	if (bDeinitializing)
	{
		// A whole recording cannot be sent inside a shutdown, and the run is about to be let go of. It stays on disk
		// and a later launch pushes it, which is what D10 is for.
		// Not raised: the game is closing, so nothing it would tell could act on the answer, and its handlers would run
		// against objects being torn down.
		UE_LOG(LogProtokitePlaytest, Log, TEXT("The recording is not being uploaded now, because the game is closing. A later launch sends it."));
		return;
	}
	if (FinishedVideoRecordingPath.IsEmpty())
	{
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("The recording cannot be uploaded: no finished file was kept."));
		NotUploadedNow(TEXT("No finished recording file was kept."));
		return;
	}

	const FProtokitePlaytestRecordingSession Session = VideoRecordingRun->LoadSession();
	if (Session.IsEmpty())
	{
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("The recording cannot be uploaded: no Protokite session started for it, so it has "
			"nowhere to go. A Protokite session starts once a player signs in and their Flock session reaches the server. The "
			"next launch deletes this recording."));
		NotUploadedNow(TEXT("No Protokite session started for the recording, so it has nowhere to go."));
		return;
	}
	if (!Flock.IsValid() || !Flock->IsInitialized())
	{
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("The recording cannot be uploaded while the Flock SDK is not running. It is kept "
			"for a later launch."));
		NotUploadedNow(TEXT("The Flock SDK is not running; the recording is kept for a later launch."));
		return;
	}

	UE_LOG(LogProtokitePlaytest, Log, TEXT("Uploading this launch's recording to Protokite session %s."), *Session.ProtokiteSessionId);
	const TWeakObjectPtr<UProtokitePlaytestSubsystem> WeakThis(this);
	GetOrCreateRecordingUploads()->UploadOne(VideoRecordingRun.ToSharedRef(), Session, Flock->GetRequestHeaders(),
		[WeakThis](FProtokitePlaytestRecordingUploadOutcome Outcome)
		{
			if (Outcome.bUploaded)
			{
				UE_LOG(LogProtokitePlaytest, Log, TEXT("The recording was uploaded and is no longer kept on disk."));
			}
			else
			{
				UE_LOG(LogProtokitePlaytest, Warning, TEXT("The recording was not uploaded, so it is kept for a later launch to push: %s"),
					*Outcome.Error);
			}
			if (UProtokitePlaytestSubsystem* Self = WeakThis.Get())
			{
				Self->OnRecordingUploadFinished.Broadcast(Outcome.bUploaded, Outcome.bUploaded ? FString() : Outcome.Error);
			}
		});
}

void UProtokitePlaytestSubsystem::StartUploadingWhatEarlierLaunchesLeft()
{
	if (bStartedUploadingWhatEarlierLaunchesLeft || WaitingToUploadEarlierRecordings.IsValid())
	{
		return;
	}

	// Two things have to be true first, and they finish in no fixed order: Flock has to have initialized, because the
	// API key comes from this launch and never from the saved session, and the launch pass has to have finished with
	// the folder, because it is what turns a cut-off recording into one worth sending.
	TWeakObjectPtr<UProtokitePlaytestSubsystem> WeakThis(this);
	WaitingToUploadEarlierRecordings = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[WeakThis](float) -> bool
		{
			UProtokitePlaytestSubsystem* Self = WeakThis.Get();
			if (Self == nullptr || Self->bDeinitializing)
			{
				return false;
			}
			if (!Self->Flock.IsValid() || !Self->Flock->IsInitialized())
			{
				return true;
			}
			if (Self->RecordingsFolderFinished.IsValid() && !Self->RecordingsFolderFinished.IsReady())
			{
				return true;
			}

			// Forms kept from an earlier launch go out first, and go whatever the player's answer is: a form was typed
			// and sent by the player themselves, which is what they are told when they answer -- the consent question
			// covers what the playtest collects on its own, never what somebody chose to send.
			Self->SendFormsKeptFromEarlierLaunches();

			// **Nothing of an earlier launch's goes out while this launch's question is still on screen.** The answer is
			// seconds away, and sending first would mean a player who then asks for nothing had their last session's
			// video uploaded while they were reading the question. Only that one status waits: a build with playtesting
			// switched off, or one that asks nobody, never reaches it, and stranding those recordings is exactly what
			// pushing them exists to prevent.
			if (Self->Status == EProtokitePlaytestStatus::WaitingForPlayerConsent)
			{
				return true;
			}

			// A player who has asked for nothing to be collected is not sent what earlier launches left, although those
			// launches recorded it with their permission. It keeps waiting rather than giving up for the launch, so a
			// player who changes their mind from their own menu has them sent then instead of a launch later.
			if (Self->GetPlaytestConsent() == EProtokitePlaytestConsentChoice::Nothing)
			{
				if (!Self->bLoggedNotPushingWhatEarlierLaunchesLeft)
				{
					Self->bLoggedNotPushingWhatEarlierLaunchesLeft = true;
					UE_LOG(LogProtokitePlaytest, Log, TEXT("Nothing an earlier launch left is being sent: the player has asked this playtest to collect nothing. What is waiting stays on disk, and goes only if they change that answer."));
				}
				return true;
			}

			Self->bStartedUploadingWhatEarlierLaunchesLeft = true;
			Self->WaitingToUploadEarlierRecordings.Reset();

			const FString RecordingsFolder = Self->GetVideoRecordingsFolder();
			Self->GetOrCreateRecordingUploads()->UploadEveryOneWaiting(RecordingsFolder, Self->Flock->GetRequestHeaders(),
				[](int32 UploadedCount, int32 LeftCount)
				{
					if (UploadedCount > 0 || LeftCount > 0)
					{
						UE_LOG(LogProtokitePlaytest, Log, TEXT("Recordings earlier launches left: %d uploaded, %d kept for a later launch."),
							UploadedCount, LeftCount);
					}
				});
			return false;
		}), 0.5f);
}

void UProtokitePlaytestSubsystem::FinishVideoRecordingNow(EProtokitePlaytestVideoStopReason Reason)
{
	if (VideoRecording.IsValid())
	{
		VideoRecording->StopCapturing(Reason);
		VideoRecording->WaitUntilWritten();
		ApplyFinishedVideoRecording();
	}
	UpdateVideoPump();
}

/**
 * Watches for the key that opens the feedback form, above the game's own input.
 *
 * A Slate input processor rather than a binding on a player controller: a playtest build should not need the game to
 * add an input action, and a game that swaps controllers or runs without one would lose the binding. It only ever
 * answers the one key it was given, so the game's own input is untouched.
 */
class FProtokitePlaytestFormKeyWatcher : public IInputProcessor
{
public:
	FProtokitePlaytestFormKeyWatcher(const TWeakObjectPtr<UProtokitePlaytestSubsystem>& InPlaytest, const FKey& InKey)
		: Playtest(InPlaytest)
		, Key(InKey)
	{
	}

	virtual void Tick(const float, FSlateApplication&, TSharedRef<ICursor>) override {}

	virtual bool HandleKeyDownEvent(FSlateApplication&, const FKeyEvent& Event) override
	{
		if (Event.GetKey() != Key || Event.IsRepeat())
		{
			return false;
		}
		UProtokitePlaytestSubsystem* Subsystem = Playtest.Get();
		if (Subsystem == nullptr)
		{
			return false;
		}
		// The same key closes it again, so a player who opened it by accident is not stuck in it.
		if (Subsystem->IsFeedbackFormOpen())
		{
			Subsystem->CloseFeedbackForm();
			return true;
		}
		return Subsystem->OpenFeedbackForm();
	}

private:
	TWeakObjectPtr<UProtokitePlaytestSubsystem> Playtest;
	FKey Key;
};

bool UProtokitePlaytestSubsystem::CanOpenFeedbackForm() const
{
	return GetStatus() == EProtokitePlaytestStatus::Ready && PlaytestConfig.HasForm();
}

bool UProtokitePlaytestSubsystem::OpenFeedbackForm()
{
	if (!CanOpenFeedbackForm() || FormWidget.IsValid())
	{
		return false;
	}
	if (ConsentWidget.IsValid())
	{
		// One panel at a time. The player is being asked what the playtest may collect, which is the question the form
		// itself depends on the answer to.
		UE_LOG(LogProtokitePlaytest, Log, TEXT("The feedback form cannot be opened while the playtest's consent question is on screen."));
		return false;
	}

	UGameInstance* GameInstance = GetGameInstance();
	UGameViewportClient* Viewport = GameInstance != nullptr ? GameInstance->GetGameViewportClient() : nullptr;
	if (Viewport == nullptr)
	{
		// A run that draws nothing -- a dedicated server, or a headless test -- has nowhere to put a form.
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("The feedback form cannot be opened: this game instance has no viewport."));
		return false;
	}

	const TSharedRef<SProtokitePlaytestFormWidget> Widget = SNew(SProtokitePlaytestFormWidget)
		.Form(PlaytestConfig.Form)
		.OnSubmitted_Lambda([this](const FProtokitePlaytestFormAnswers& Answers)
		{
			SendFilledInForm(Answers);
			// Closed straight away rather than held open waiting: a form that does not get through is kept and sent by
			// a later launch, so there is nothing for the player to wait for or to do again.
			CloseFeedbackForm();
		})
		.OnClosed_Lambda([this]() { CloseFeedbackForm(); })
		// Offered only when the recording has somewhere to go, never merely because one is running.
		.CanSendRecording_Lambda([this]() { return CanSendTheRecording(); })
		.OnSendRecording_Lambda([this]()
		{
			// The player's own choice, which is the only thing that stops a recording early: opening the form does not.
			UE_LOG(LogProtokitePlaytest, Log, TEXT("The player asked for their recording to be sent."));
			return StopVideoRecordingAndUploadIt();
		});

	FormWidget = Widget;
	ShowPanelOverTheGame(Widget);

	const UProtokitePlaytestSettings* Settings = GetDefault<UProtokitePlaytestSettings>();
	if (Settings->bPauseWhileFeedbackFormIsOpen && !UGameplayStatics::IsGamePaused(this))
	{
		bPausedForTheForm = UGameplayStatics::SetGamePaused(this, true);
	}

	UE_LOG(LogProtokitePlaytest, Log, TEXT("The playtest feedback form is open."));
	return true;
}

bool UProtokitePlaytestSubsystem::CloseFeedbackForm()
{
	if (!FormWidget.IsValid())
	{
		return false;
	}

	const TSharedRef<SProtokitePlaytestFormWidget> Widget = FormWidget.ToSharedRef();
	FormWidget.Reset();
	HidePanelOverTheGame(Widget);

	if (bPausedForTheForm)
	{
		// Only a pause of this subsystem's own is undone: a game paused for its own reasons stays paused.
		UGameplayStatics::SetGamePaused(this, false);
		bPausedForTheForm = false;
	}

	UE_LOG(LogProtokitePlaytest, Log, TEXT("The playtest feedback form is closed."));
	return true;
}

bool UProtokitePlaytestSubsystem::SendFilledInForm(const FProtokitePlaytestFormAnswers& Answers)
{
	if (!PlaytestConfig.HasForm())
	{
		// Said out loud: a game drawing its own form calls this, and a silent return would read as sent.
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("The feedback form cannot be sent: this playtest has no published feedback form loaded."));
		return false;
	}

	FProtokitePlaytestFormSubmission Submission;
	Submission.PlaytestSessionId = PlaytestSessionId;
	Submission.ProtokiteApiUrl = GetDefault<UProtokitePlaytestSettings>()->ProtokiteApiUrl;
	Submission.FlockGameVersionId = PlaytestConfig.FlockGameVersionId;

	// The same identity the session was started with, or one resolved now for a form filled in before any session.
	Submission.Identity = PlaytestIdentity;
	if (Submission.Identity.IsEmpty())
	{
		FString WhyNone;
		Submission.Identity = ResolvePlaytestIdentity(WhyNone);
		if (Submission.Identity.IsEmpty())
		{
			// The server refuses a form with nobody to attribute it to, so keeping it would only fail forever.
			UE_LOG(LogProtokitePlaytest, Warning, TEXT("The feedback form cannot be sent: %s"), *WhyNone);
			return false;
		}
	}

	FString AnswersJson;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&AnswersJson);
	FJsonSerializer::Serialize(Answers.ToWireObject(PlaytestConfig.Form), Writer);
	Submission.AnswersJson = AnswersJson;

	const FProtokitePlaytestFormSpool Spool(FormSpoolFolderForTesting.Get(FProtokitePlaytestFormSpool::GetDefaultFolder()));

	// Written down before it is sent, not after a failure: the game may be closed between the two, and a player's
	// answers are the one thing here that cannot be collected again.
	FString KeepError;
	const FString KeptAt = Spool.Keep(Submission, KeepError);
	if (KeptAt.IsEmpty())
	{
		UE_LOG(LogProtokitePlaytest, Warning, TEXT("The feedback form could not be kept while it is sent: %s"), *KeepError);
	}

	if (!Flock.IsValid() || !Flock->IsInitialized())
	{
		if (KeptAt.IsEmpty())
		{
			// Neither kept nor sent: the warning above already says why.
			return false;
		}
		UE_LOG(LogProtokitePlaytest, Log, TEXT("The feedback form is kept until the Flock SDK is running."));
		return true;
	}

	UE_LOG(LogProtokitePlaytest, Log, TEXT("Sending the feedback form."));
	const FString SpoolFolder = Spool.GetDefaultFolder();
	GetOrCreateProtokiteClient()->SubmitFeedbackForm(Flock->GetRequestHeaders(), Submission,
		[KeptAt, SpoolFolderCopy = FormSpoolFolderForTesting.Get(SpoolFolder)]
		(TFlockResult<FProtokitePlaytestFormSubmitResult> Result)
		{
			const FProtokitePlaytestFormSpool Done(SpoolFolderCopy);
			if (Result.IsSuccess())
			{
				if (!KeptAt.IsEmpty())
				{
					Done.Forget(KeptAt);
				}
				UE_LOG(LogProtokitePlaytest, Log, TEXT("The feedback form was sent."));
				return;
			}

			// A refusal the server will repeat is not worth keeping: the answers would be sent again at every launch to
			// be turned away again. Anything else -- no network, a server having a moment -- is kept.
			const bool bServerRefusedIt = Result.Error.StatusCode == 422 || Result.Error.StatusCode == 404;
			if (bServerRefusedIt && !KeptAt.IsEmpty())
			{
				Done.Forget(KeptAt);
			}
			// Protokite names the question it refused only inside the message, so it is pulled out and said plainly. A
			// player cannot act on it -- the form has closed -- but whoever edited the form can.
			const FString RefusedQuestion = ProtokitePlaytestFindFieldIdInComplaint(Result.Error.ToDisplayText());
			UE_LOG(LogProtokitePlaytest, Warning, TEXT("The feedback form was not sent%s%s: %s"),
				bServerRefusedIt ? TEXT("") : TEXT(", and is kept for a later launch"),
				RefusedQuestion.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (the question '%s' was refused)"), *RefusedQuestion),
				*Result.Error.ToDisplayText());
		});
	return true;
}

void UProtokitePlaytestSubsystem::SendFormsKeptFromEarlierLaunches()
{
	if (bStartedSendingKeptForms || !Flock.IsValid() || !Flock->IsInitialized())
	{
		return;
	}
	bStartedSendingKeptForms = true;

	const FString Folder = FormSpoolFolderForTesting.Get(FProtokitePlaytestFormSpool::GetDefaultFolder());
	const FProtokitePlaytestFormSpool Spool(Folder);
	const TArray<TPair<FString, FProtokitePlaytestFormSubmission>> Waiting = Spool.FindWaiting();
	if (Waiting.Num() == 0)
	{
		return;
	}

	UE_LOG(LogProtokitePlaytest, Log, TEXT("Sending %d feedback form(s) kept from an earlier launch."), Waiting.Num());
	const TMap<FString, FString> LaunchHeaders = Flock->GetRequestHeaders();

	for (const TPair<FString, FProtokitePlaytestFormSubmission>& Kept : Waiting)
	{
		// This launch's own key, with the Game Version ID the form's own session ran under -- the same swap a kept
		// recording needs, and for the same reason: Protokite finds the playtest from that version.
		TMap<FString, FString> Headers = LaunchHeaders;
		if (!Kept.Value.FlockGameVersionId.IsEmpty())
		{
			Headers.Add(TEXT("X-Game-Version-ID"), Kept.Value.FlockGameVersionId);
		}

		const FString Path = Kept.Key;
		GetOrCreateProtokiteClient()->SubmitFeedbackForm(Headers, Kept.Value,
			[Path, Folder](TFlockResult<FProtokitePlaytestFormSubmitResult> Result)
			{
				const FProtokitePlaytestFormSpool Done(Folder);
				const bool bServerRefusedIt = Result.Error.StatusCode == 422 || Result.Error.StatusCode == 404;
				if (Result.IsSuccess() || bServerRefusedIt)
				{
					// Taken, or refused in a way that will not change. Either way it stops waiting.
					Done.Forget(Path);
				}
				if (Result.IsSuccess())
				{
					UE_LOG(LogProtokitePlaytest, Log, TEXT("A kept feedback form was sent."));
					return;
				}
				const FString RefusedQuestion = ProtokitePlaytestFindFieldIdInComplaint(Result.Error.ToDisplayText());
				UE_LOG(LogProtokitePlaytest, Warning, TEXT("A kept feedback form was not sent%s: %s"),
					RefusedQuestion.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (the question '%s' was refused)"), *RefusedQuestion),
					*Result.Error.ToDisplayText());
			});
	}
}

bool UProtokitePlaytestSubsystem::IsThePlaytestLoaded() const
{
	// The three statuses a loaded playtest can be in: waiting for the answer, refused, or running.
	return Status == EProtokitePlaytestStatus::WaitingForPlayerConsent
		|| Status == EProtokitePlaytestStatus::PlayerRefusedPlaytest
		|| Status == EProtokitePlaytestStatus::Ready;
}

void UProtokitePlaytestSubsystem::UpdateConsentQuestion()
{
	// Two reasons to have it up: the playtest is waiting for a first answer, or a game asked for it to be put again.
	const bool bWanted = !bDeinitializing && IsThePlaytestLoaded()
		&& (Status == EProtokitePlaytestStatus::WaitingForPlayerConsent || bAskedToChangeConsent);
	if (!bWanted)
	{
		StopWaitingForAViewportToAskIn();
		CloseConsentQuestion();
		return;
	}
	if (ConsentWidget.IsValid())
	{
		return;
	}
	if (OpenConsentQuestion())
	{
		StopWaitingForAViewportToAskIn();
		return;
	}
	// There is nowhere to draw it yet. A game instance gets its viewport when it gets one, so this keeps trying rather
	// than deciding once that the player cannot be asked.
	WaitForAViewportToAskIn();
}

bool UProtokitePlaytestSubsystem::OpenConsentQuestion()
{
	// One panel at a time, and the form is the player's own doing, so it is never taken away from under them. Nothing
	// is said here: whoever asked has already been told.
	if (FormWidget.IsValid())
	{
		return false;
	}

	UGameInstance* GameInstance = GetGameInstance();
	UGameViewportClient* Viewport = GameInstance != nullptr ? GameInstance->GetGameViewportClient() : nullptr;
	if (Viewport == nullptr || !FSlateApplication::IsInitialized())
	{
		if (!bLoggedNowhereToAskForConsent)
		{
			bLoggedNowhereToAskForConsent = true;
			// Both reasons are named, because a run with a viewport and no Slate application would otherwise be sent
			// looking for a rendering fault that is not there.
			UE_LOG(LogProtokitePlaytest, Warning, TEXT("This build's playtest is loaded, but the consent question cannot be drawn yet: this game instance has %s. Nothing is collected until it is answered, and a run that draws nothing (a dedicated server, -nullrhi) never can answer it. Such a run can be given an answer with Protokite Set Playtest Consent or the console command ProtokitePlaytest.AnswerConsent, or can turn Ask The Player For Playtest Consent off."),
				Viewport == nullptr ? TEXT("no viewport") : TEXT("no Slate application to draw with"));
		}
		return false;
	}

	const TSharedRef<SProtokitePlaytestConsentWidget> Widget = SNew(SProtokitePlaytestConsentWidget)
		.OnChosen_Lambda([this](EProtokitePlaytestConsentChoice Choice)
		{
			// Saving decides the status again, which is what takes the question away and starts or stops everything.
			SetPlaytestConsent(Choice);
		});

	ConsentWidget = Widget;
	ShowPanelOverTheGame(Widget);
	UE_LOG(LogProtokitePlaytest, Log, TEXT("The playtest's consent question is on screen. Nothing is collected until the player answers it."));
	return true;
}

void UProtokitePlaytestSubsystem::CloseConsentQuestion()
{
	if (!ConsentWidget.IsValid())
	{
		return;
	}
	const TSharedRef<SProtokitePlaytestConsentWidget> Widget = ConsentWidget.ToSharedRef();
	ConsentWidget.Reset();
	HidePanelOverTheGame(Widget);
	UE_LOG(LogProtokitePlaytest, Verbose, TEXT("The playtest's consent question is closed."));
}

void UProtokitePlaytestSubsystem::WaitForAViewportToAskIn()
{
	if (WaitingToAskForConsent.IsValid())
	{
		return;
	}
	const TWeakObjectPtr<UProtokitePlaytestSubsystem> WeakThis(this);
	WaitingToAskForConsent = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[WeakThis](float) -> bool
		{
			UProtokitePlaytestSubsystem* Self = WeakThis.Get();
			if (Self == nullptr || Self->bDeinitializing)
			{
				return false;
			}
			Self->UpdateConsentQuestion();
			// The question being up, or no longer wanted, clears the handle: this then stops with it.
			return Self->WaitingToAskForConsent.IsValid();
		}), 0.5f);
}

void UProtokitePlaytestSubsystem::StopWaitingForAViewportToAskIn()
{
	if (!WaitingToAskForConsent.IsValid())
	{
		return;
	}
	FTSTicker::GetCoreTicker().RemoveTicker(WaitingToAskForConsent);
	WaitingToAskForConsent.Reset();
}

void UProtokitePlaytestSubsystem::ShowPanelOverTheGame(const TSharedRef<SWidget>& Panel)
{
	UGameInstance* GameInstance = GetGameInstance();
	UGameViewportClient* Viewport = GameInstance != nullptr ? GameInstance->GetGameViewportClient() : nullptr;
	if (Viewport == nullptr)
	{
		return;
	}

	// High enough to sit over the game's own HUD widgets.
	Viewport->AddViewportWidgetContent(Panel, /*ZOrder*/ 1000);

	if (APlayerController* Controller = GameInstance->GetFirstLocalPlayerController())
	{
		// Remembered rather than assumed: a game that already showed a cursor must still have one afterwards.
		bCursorWasShownBeforeThePanel = Controller->bShowMouseCursor;
		Controller->bShowMouseCursor = true;
		FInputModeUIOnly Mode;
		Mode.SetWidgetToFocus(Panel);
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Controller->SetInputMode(Mode);
	}

	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetKeyboardFocus(Panel);
	}
}

void UProtokitePlaytestSubsystem::HidePanelOverTheGame(const TSharedRef<SWidget>& Panel)
{
	UGameInstance* GameInstance = GetGameInstance();
	if (UGameViewportClient* Viewport = GameInstance != nullptr ? GameInstance->GetGameViewportClient() : nullptr)
	{
		Viewport->RemoveViewportWidgetContent(Panel);
	}

	if (APlayerController* Controller = GameInstance != nullptr ? GameInstance->GetFirstLocalPlayerController() : nullptr)
	{
		// Put back exactly what was there, or a game that never had a cursor keeps one and its input stays on the UI.
		Controller->bShowMouseCursor = bCursorWasShownBeforeThePanel;
		Controller->SetInputMode(FInputModeGameOnly());
	}
}

void UProtokitePlaytestSubsystem::UpdateFeedbackFormKeyWatcher()
{
	const UProtokitePlaytestSettings* Settings = GetDefault<UProtokitePlaytestSettings>();
	const bool bWanted = CanOpenFeedbackForm() && Settings->FeedbackFormKey.IsValid() && FSlateApplication::IsInitialized();

	if (bWanted == FormKeyWatcher.IsValid())
	{
		return;
	}
	if (bWanted)
	{
		FormKeyWatcher = MakeShared<FProtokitePlaytestFormKeyWatcher>(TWeakObjectPtr<UProtokitePlaytestSubsystem>(this), Settings->FeedbackFormKey);
		FSlateApplication::Get().RegisterInputPreProcessor(FormKeyWatcher);
	}
	else
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().UnregisterInputPreProcessor(FormKeyWatcher);
		}
		FormKeyWatcher.Reset();
	}
}

#if !UE_BUILD_SHIPPING
namespace
{
	UProtokitePlaytestSubsystem* FindPlaytestSubsystemOfWorld(const UWorld* World)
	{
		const UGameInstance* GameInstance = World != nullptr ? World->GetGameInstance() : nullptr;
		return GameInstance != nullptr ? GameInstance->GetSubsystem<UProtokitePlaytestSubsystem>() : nullptr;
	}

	FAutoConsoleCommandWithWorldArgsAndOutputDevice RecordTestVideoCommand(
		TEXT("ProtokitePlaytest.RecordTestVideo"),
		TEXT("Records this game instance's screen for the given number of seconds, with no playtest needed: ProtokitePlaytest.RecordTestVideo 60. The file goes to Saved/ProtokitePlaytest/Recordings/TestVideos, is never uploaded, and is kept until Recordings Disk Budget (MB) needs its room. One video per launch. Not in Shipping builds."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Output)
		{
			double Seconds = 0.0;
			if (Args.Num() != 1 || !LexTryParseString(Seconds, *Args[0]) || Seconds <= 0.0)
			{
				Output.Log(TEXT("Usage: ProtokitePlaytest.RecordTestVideo <seconds>, with seconds above 0."));
				return;
			}
			UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystemOfWorld(World);
			if (Playtest == nullptr)
			{
				Output.Log(TEXT("No Protokite Playtest subsystem runs in this world's game instance."));
				return;
			}
			Playtest->StartTestVideoRecording(Seconds);
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice AnswerConsentCommand(
		TEXT("ProtokitePlaytest.AnswerConsent"),
		TEXT("Answers the playtest's consent question as a player would, for a run with nobody at the keyboard: "
			"ProtokitePlaytest.AnswerConsent video_and_play_data | video_only | play_data_only | nothing | not_answered. "
			"The answer is kept for later launches, and not_answered forgets it so the question is put again. A build "
			"that should never ask turns off Ask The Player For Playtest Consent instead."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Output)
		{
			// Lower-cased first, so a console answer is not refused over letter case; the saved answer and what a
			// session sends are the spellings the table holds either way.
			const EProtokitePlaytestConsentChoice Choice = Args.Num() == 1
				? ProtokitePlaytestConsent::FromWire(Args[0].ToLower())
				: EProtokitePlaytestConsentChoice::NotAnswered;
			const bool bReadTheAnswer = Args.Num() == 1
				&& (ProtokitePlaytestConsent::IsAnswered(Choice) || Args[0].ToLower() == ProtokitePlaytestConsent::ToWire(EProtokitePlaytestConsentChoice::NotAnswered));
			if (!bReadTheAnswer)
			{
				Output.Log(TEXT("Usage: ProtokitePlaytest.AnswerConsent <video_and_play_data|video_only|play_data_only|nothing|not_answered>."));
				return;
			}
			UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystemOfWorld(World);
			if (Playtest == nullptr)
			{
				Output.Log(TEXT("No Protokite Playtest subsystem runs in this world's game instance."));
				return;
			}
			Playtest->SetPlaytestConsent(Choice);
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice StopVideoRecordingCommand(
		TEXT("ProtokitePlaytest.StopVideoRecording"),
		TEXT("Stops this game instance's video recording for good, and saves the file."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Output)
		{
			UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystemOfWorld(World);
			if (Playtest == nullptr || !Playtest->StopVideoRecording())
			{
				Output.Log(TEXT("No video is being recorded."));
			}
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice SendTestFeedbackCommand(
		TEXT("ProtokitePlaytest.SendTestFeedback"),
		TEXT("Fills the playtest's feedback form with a plausible answer to every question and sends it, for checking a "
			"form reaches the dashboard without typing it in by hand: ProtokitePlaytest.SendTestFeedback, or ... 10 to wait "
			"ten seconds first. Development builds only."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Output)
		{
			double Seconds = 0.0;
			if (Args.Num() > 0 && (!LexTryParseString(Seconds, *Args[0]) || Seconds < 0.0))
			{
				Output.Log(TEXT("Usage: ProtokitePlaytest.SendTestFeedback [seconds to wait first]."));
				return;
			}

			auto Send = [](UWorld* InWorld, FOutputDevice* Out)
			{
				UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystemOfWorld(InWorld);
				if (Playtest == nullptr || !Playtest->CanOpenFeedbackForm())
				{
					if (Out != nullptr)
					{
						Out->Log(TEXT("There is no feedback form to send."));
					}
					return;
				}

				// An answer of the right shape for each question, so the server takes it exactly as a player's would be.
				Playtest->SendFilledInForm(ProtokitePlaytestAnswerEveryQuestion(Playtest->GetPlaytestConfig().Form,
					TEXT("Sent by ProtokitePlaytest.SendTestFeedback.")));
			};

			if (Seconds <= 0.0)
			{
				Send(World, &Output);
				return;
			}
			TWeakObjectPtr<UWorld> WeakWorld(World);
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakWorld, Send](float) -> bool
			{
				Send(WeakWorld.Get(), nullptr);
				return false;
			}), static_cast<float>(Seconds));
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice OpenFeedbackFormCommand(
		TEXT("ProtokitePlaytest.OpenFeedbackForm"),
		TEXT("Opens the playtest's feedback form, the way the form key does: ProtokitePlaytest.OpenFeedbackForm, or ... 10 to "
			"open it ten seconds from now, which is how a harness reaches it at all."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Output)
		{
			// The wait exists for the same reason the one on the recording command does: every -ExecCmds command runs on
			// the first frame, before the playtest is ready and before there is a viewport to put a form in.
			double Seconds = 0.0;
			if (Args.Num() > 0 && (!LexTryParseString(Seconds, *Args[0]) || Seconds < 0.0))
			{
				Output.Log(TEXT("Usage: ProtokitePlaytest.OpenFeedbackForm [seconds to wait first]."));
				return;
			}
			if (Seconds <= 0.0)
			{
				UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystemOfWorld(World);
				if (Playtest == nullptr || !Playtest->OpenFeedbackForm())
				{
					Output.Log(TEXT("There is no feedback form to open."));
				}
				return;
			}
			TWeakObjectPtr<UWorld> WeakWorld(World);
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakWorld](float) -> bool
			{
				if (UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystemOfWorld(WeakWorld.Get()))
				{
					Playtest->OpenFeedbackForm();
				}
				return false;
			}), static_cast<float>(Seconds));
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice StopVideoRecordingAndUploadItCommand(
		TEXT("ProtokitePlaytest.StopVideoRecordingAndUploadIt"),
		TEXT("Stops this game instance's video recording and uploads it to its playtest session, the way a feedback form's "
			"upload button does: ProtokitePlaytest.StopVideoRecordingAndUploadIt, or ... 20 to let it record for 20 seconds "
			"first. Uploading is not waited for; what does not make it is kept for a later launch to push."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Output)
		{
			// The wait is what makes this drivable from a harness at all: every -ExecCmds command runs on the first
			// frame, which is before anything has been recorded, so with no wait there would be nothing to upload.
			double Seconds = 0.0;
			if (Args.Num() > 0 && (!LexTryParseString(Seconds, *Args[0]) || Seconds < 0.0))
			{
				Output.Log(TEXT("Usage: ProtokitePlaytest.StopVideoRecordingAndUploadIt [seconds to record first]."));
				return;
			}

			if (Seconds <= 0.0)
			{
				UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystemOfWorld(World);
				if (Playtest == nullptr || !Playtest->StopVideoRecordingAndUploadIt())
				{
					Output.Log(TEXT("No video is being recorded."));
				}
				return;
			}

			// The world is held weakly: the game may be gone by the time this comes round.
			TWeakObjectPtr<UWorld> WeakWorld(World);
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakWorld](float) -> bool
			{
				UProtokitePlaytestSubsystem* Playtest = FindPlaytestSubsystemOfWorld(WeakWorld.Get());
				if (Playtest != nullptr)
				{
					Playtest->StopVideoRecordingAndUploadIt();
				}
				return false;
			}), static_cast<float>(Seconds));
		}));
}
#endif
