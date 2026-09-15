// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockPlaytestSubsystem.h"

#include "Async/Async.h"
#include "Config/FlockConfig.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FlockEvents.h"
#include "FlockPlaytestLocalSettings.h"
#include "FlockPlaytestLog.h"
#include "FlockPlaytestLogger.h"
#include "FlockPlaytestSettings.h"
#include "FlockPlaytestVideoFrameSource.h"
#include "FlockPlaytestVideoRecording.h"
#include "FlockProtokiteClient.h"
#include "FlockSubsystem.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Http/FlockHttpClient.h"
#include "Misc/Paths.h"
#include "Providers/FlockAnalyticsProvider.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	/** Sends the end for one Protokite session and logs what became of it. The one place an end is sent. */
	void SendPlaytestSessionEnd(const TSharedRef<FFlockProtokiteClient>& Client, const FString& ProtokiteApiUrl,
		const TMap<FString, FString>& RequestHeaders, const FString& PlaytestSessionId)
	{
		Client->EndPlaytestSession(ProtokiteApiUrl, RequestHeaders, PlaytestSessionId,
			[PlaytestSessionId](TFlockResult<FFlockPlaytestSessionEndResult> Result)
			{
				if (Result.bSuccess)
				{
					UE_LOG(LogFlockPlaytest, Log, TEXT("Protokite session %s ended."), *PlaytestSessionId);
				}
				else
				{
					UE_LOG(LogFlockPlaytest, Warning, TEXT("Protokite session %s could not be ended, so Protokite shows it as still in progress. %s"),
						*PlaytestSessionId, *Result.Error.ToDisplayText());
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

void UFlockPlaytestSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// The Flock subsystem initializes first, so with Auto-Initialize On Load it is already up by the time
	// this starts following it.
	FollowFlockLifecycle(Collection.InitializeDependency<UFlockSubsystem>());
}

void UFlockPlaytestSubsystem::Deinitialize()
{
	// The player is leaving, so the launch's session ends while the network is still there. A start still on its way
	// is ended when its answer arrives.
	if (SessionState == EFlockPlaytestSessionState::Started)
	{
		EndPlaytestSession();
	}

	// The Flock subsystem can outlive this one during teardown, and its shut-down event would otherwise
	// still reach a subsystem that has finished.
	if (UFlockSubsystem* Followed = Flock.Get())
	{
		UFlockEvents* Events = Followed->GetEvents();
		Events->OnInitialized.RemoveDynamic(this, &UFlockPlaytestSubsystem::HandleFlockLifecycleChanged);
		Events->OnShutdown.RemoveDynamic(this, &UFlockPlaytestSubsystem::HandleFlockLifecycleChanged);
		Events->OnSessionStarted.RemoveDynamic(this, &UFlockPlaytestSubsystem::HandleFlockSessionStarted);
		Events->OnSessionRegistered.RemoveDynamic(this, &UFlockPlaytestSubsystem::HandleFlockSessionRegistered);
	}
	Flock.Reset();

	// A reply still on its way must not reach a subsystem that has finished.
	ForgetPlaytestConfig();

	// Whatever the last status was, no playtest work may run on a subsystem that has shut down.
	ApplyStatus(EFlockPlaytestStatus::Stopped, FString(), FString());
	UpdatePerformanceTimeline();

	// The file is finished before the subsystem goes, so what was recorded is kept.
	FinishVideoRecordingNow(EFlockPlaytestVideoStopReason::GameInstanceShutDown);

	Super::Deinitialize();
}

bool UFlockPlaytestSubsystem::IsPlaytestFeatureEnabled(const FString& FeatureName) const
{
	return Status == EFlockPlaytestStatus::Ready && PlaytestConfig.IsFeatureEnabled(FeatureName);
}

bool UFlockPlaytestSubsystem::EndPlaytestSession()
{
	if (SessionState != EFlockPlaytestSessionState::Started && SessionState != EFlockPlaytestSessionState::Ended)
	{
		return false;
	}
	SessionState = EFlockPlaytestSessionState::Ended;
	SendPlaytestSessionEnd(GetOrCreateProtokiteClient(), PlaytestSessionApiUrl, PlaytestSessionHeaders, PlaytestSessionId);
	return true;
}

UFlockSubsystem* UFlockPlaytestSubsystem::GetFollowedFlockForTesting() const
{
	return Flock.Get();
}

void UFlockPlaytestSubsystem::FollowFlockLifecycle(UFlockSubsystem* InFlock)
{
	Flock = InFlock;
	if (InFlock)
	{
		UFlockEvents* Events = InFlock->GetEvents();
		Events->OnInitialized.AddUniqueDynamic(this, &UFlockPlaytestSubsystem::HandleFlockLifecycleChanged);
		Events->OnShutdown.AddUniqueDynamic(this, &UFlockPlaytestSubsystem::HandleFlockLifecycleChanged);
		Events->OnSessionStarted.AddUniqueDynamic(this, &UFlockPlaytestSubsystem::HandleFlockSessionStarted);
		Events->OnSessionRegistered.AddUniqueDynamic(this, &UFlockPlaytestSubsystem::HandleFlockSessionRegistered);
	}
	RefreshStatus();
}

void UFlockPlaytestSubsystem::HandleFlockLifecycleChanged()
{
	RefreshStatus();
}

void UFlockPlaytestSubsystem::HandleFlockSessionStarted(const FString& SessionId)
{
	// Only a failure to reach Protokite is worth asking again: a session starting is when playtest work would
	// begin, and the game carries on whatever the answer is.
	if (ConfigState == EFlockPlaytestConfigState::Unavailable)
	{
		ForgetPlaytestConfig();
		RefreshStatus();
	}
}

void UFlockPlaytestSubsystem::HandleFlockSessionRegistered(const FString& SessionId, const FString& ServerSessionId)
{
	// The first one names the Protokite session. A later one comes from a new Flock session after time away or after
	// a sign-in, and changes nothing: the launch keeps the session it has.
	if (FirstFlockServerSessionId.IsEmpty())
	{
		FirstFlockServerSessionId = ServerSessionId;
	}
	StartPlaytestSessionWhenAllowed();
}

void UFlockPlaytestSubsystem::RefreshStatus()
{
	// A config belongs to the Flock initialization it was fetched under. Once the Flock SDK is down, the next
	// initialization may carry another key or version, so the config and any reply still on its way are stale. So is
	// its first session: a session start sends the next initialization's headers, and has to name one of its sessions.
	const bool bFlockInitialized = Flock.IsValid() && Flock->IsInitialized();
	if (!bFlockInitialized)
	{
		if (ConfigState != EFlockPlaytestConfigState::NotFetched)
		{
			ForgetPlaytestConfig();
		}
		FirstFlockServerSessionId.Empty();
	}

	// Decided again after starting the fetch: a transport that answers straight away finishes the fetch, and
	// applies its own status, inside StartPlaytestConfigFetch.
	if (DecideCurrentStatus() == EFlockPlaytestStatus::FetchingPlaytestConfig
		&& ConfigState == EFlockPlaytestConfigState::NotFetched)
	{
		StartPlaytestConfigFetch();
	}

	ApplyStatus(DecideCurrentStatus(), GetDefault<UFlockPlaytestSettings>()->ProtokiteApiUrl,
		Flock.IsValid() ? Flock->GetGameVersionId() : FString());

	// After the status is applied, because all three need Ready.
	UpdatePerformanceTimeline();
	UpdateVideoRecording();
	StartPlaytestSessionWhenAllowed();
}

EFlockPlaytestStatus UFlockPlaytestSubsystem::DecideCurrentStatus() const
{
	const UFlockPlaytestSettings* Settings = GetDefault<UFlockPlaytestSettings>();

	FFlockPlaytestStatusInputs Inputs;
	Inputs.bPlaytestingEnabled = Settings->bPlaytestingEnabled;
	Inputs.ProtokiteApiUrl = Settings->ProtokiteApiUrl;
	Inputs.bFlockInitialized = Flock.IsValid() && Flock->IsInitialized();
	Inputs.ConfigState = ConfigState;
	Inputs.bPlaytestNoLongerCollecting = bPlaytestNoLongerCollecting;
	return DecidePlaytestStatus(Inputs);
}

TSharedRef<FFlockProtokiteClient> UFlockPlaytestSubsystem::GetOrCreateProtokiteClient()
{
	if (!ProtokiteClient.IsValid())
	{
		// The same timeout and retry settings the Flock SDK uses for its own requests.
		const UFlockConfig* FlockSettings = GetDefault<UFlockConfig>();
		const TSharedRef<IFlockLogger> Logger = MakeShared<FFlockPlaytestLogger>();
		const TSharedRef<FFlockHttpClient> HttpClient = TestHttpAdapter.IsValid()
			? MakeShared<FFlockHttpClient>(TestHttpAdapter.ToSharedRef(), Logger, FlockSettings->HttpTimeoutSeconds)
			: FFlockHttpClient::CreateDefault(FlockSettings->HttpTimeoutSeconds, Logger);

		FFlockRetryPolicy Policy;
		Policy.MaxRetries = FlockSettings->RetryMaxRetries;
		Policy.bUseJitter = FlockSettings->bRetryUseJitter;
		ProtokiteClient = MakeShared<FFlockProtokiteClient>(HttpClient, TestRetryPolicy.Get(Policy), Logger);
	}
	return ProtokiteClient.ToSharedRef();
}

void UFlockPlaytestSubsystem::StartPlaytestConfigFetch()
{
	const TSharedRef<FFlockProtokiteClient> Client = GetOrCreateProtokiteClient();

	// Exactly the key and version the Flock SDK initialized with, so the playtest found is this build's.
	const TMap<FString, FString> RequestHeaders = Flock->GetRequestHeaders();
	const FString SentGameVersionId = RequestHeaders.FindRef(TEXT("X-Game-Version-ID"));

	ConfigState = EFlockPlaytestConfigState::Fetching;
	const int32 TimesForgottenWhenSent = TimesConfigForgotten;
	const TWeakObjectPtr<UFlockPlaytestSubsystem> WeakThis(this);
	ConfigFetchRequest = Client->FetchPlaytestConfig(GetDefault<UFlockPlaytestSettings>()->ProtokiteApiUrl,
		RequestHeaders, [WeakThis, TimesForgottenWhenSent, SentGameVersionId](TFlockResult<FFlockPlaytestConfig> Result)
		{
			UFlockPlaytestSubsystem* Self = WeakThis.Get();
			// Stale: the Flock SDK shut down, or this subsystem finished, while the request was out.
			if (Self == nullptr || Self->TimesConfigForgotten != TimesForgottenWhenSent)
			{
				return;
			}

			Self->ConfigState = DecidePlaytestConfigState(Result, SentGameVersionId);
			const bool bLoaded = Self->ConfigState == EFlockPlaytestConfigState::Loaded;
			Self->PlaytestConfig = bLoaded ? Result.Value : FFlockPlaytestConfig();
			if (bLoaded)
			{
				Self->ConfigFailureMessage.Empty();
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

void UFlockPlaytestSubsystem::ForgetPlaytestConfig()
{
	++TimesConfigForgotten;
	ConfigState = EFlockPlaytestConfigState::NotFetched;
	PlaytestConfig = FFlockPlaytestConfig();
	ConfigFailureMessage.Empty();

	// Its retries would otherwise keep sending an ended initialization's key to Protokite. Stopped after the count
	// has moved, so whatever the stopped request still answers is already stale.
	ConfigFetchRequest.Cancel();
	ConfigFetchRequest = FFlockRequestHandle();
}

void UFlockPlaytestSubsystem::StartPlaytestSessionWhenAllowed()
{
	// One start per launch, whatever became of it.
	if (SessionState != EFlockPlaytestSessionState::NotStarted || Status != EFlockPlaytestStatus::Ready)
	{
		return;
	}
	if (FirstFlockServerSessionId.IsEmpty())
	{
		if (!bLoggedWaitingForFlockSession)
		{
			bLoggedWaitingForFlockSession = true;
			UE_LOG(LogFlockPlaytest, Log, TEXT("The Protokite session starts once a Flock session reaches the server. A Flock session starts when a player signs in, with Analytics Enabled and Analytics Auto Start Session on (or a Start Session call), and consent granted when Analytics Require Explicit Consent is on: Project Settings > Plugins > Flock SDK Settings."));
		}
		return;
	}

	FString WhyNoIdentity;
	PlaytestIdentity = ResolvePlaytestIdentity(WhyNoIdentity);
	if (PlaytestIdentity.IsEmpty())
	{
		SessionState = EFlockPlaytestSessionState::NoPlayerIdentity;
		UE_LOG(LogFlockPlaytest, Warning, TEXT("No Protokite session is started this launch, and nothing is sent: %s"), *WhyNoIdentity);
		return;
	}

	FFlockPlaytestSessionStartRequest Request;
	Request.Identity = PlaytestIdentity;
	if (IsUsablePlaytestId(FirstFlockServerSessionId, FlockPlaytestSessionLimits::FlockSessionIdLength))
	{
		Request.FlockSessionId = FirstFlockServerSessionId;
	}
	else
	{
		UE_LOG(LogFlockPlaytest, Warning, TEXT("Flock session id '%s' cannot be sent to Protokite (longer than %d characters, or containing whitespace), so the Protokite session starts without naming its Flock session."),
			*FirstFlockServerSessionId, FlockPlaytestSessionLimits::FlockSessionIdLength);
	}
	const UGameInstance* GameInstance = GetGameInstance();
	Request.DebugInfo = MakePlaytestSessionDebugInfo(MapNameOf(GameInstance != nullptr ? GameInstance->GetWorld() : nullptr));

	// Kept for the end, which goes to the same place with the same headers even after the Flock SDK has shut down.
	PlaytestSessionApiUrl = GetDefault<UFlockPlaytestSettings>()->ProtokiteApiUrl;
	PlaytestSessionHeaders = Flock->GetRequestHeaders();
	SessionState = EFlockPlaytestSessionState::Starting;

	const TSharedRef<FFlockProtokiteClient> Client = GetOrCreateProtokiteClient();
	const TWeakObjectPtr<UFlockPlaytestSubsystem> WeakThis(this);
	const FString ApiUrl = PlaytestSessionApiUrl;
	const TMap<FString, FString> Headers = PlaytestSessionHeaders;
	const bool bSentSteamId = !PlaytestIdentity.SteamId.IsEmpty();
	const FString SentFlockSessionId = Request.FlockSessionId;
	Client->StartPlaytestSession(ApiUrl, Headers, Request,
		[WeakThis, Client, ApiUrl, Headers, bSentSteamId, SentFlockSessionId](TFlockResult<FFlockPlaytestSessionStartResult> Result)
		{
			UFlockPlaytestSubsystem* Self = WeakThis.Get();
			if (Self == nullptr || Self->Status == EFlockPlaytestStatus::Stopped)
			{
				// The game instance shut down while the start was on its way. The launch is over, so a session that was
				// created is ended straight away rather than left in progress.
				if (Self != nullptr)
				{
					Self->SessionState = Result.bSuccess ? EFlockPlaytestSessionState::Ended : EFlockPlaytestSessionState::StartFailed;
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
				Self->SessionState = EFlockPlaytestSessionState::Started;
				Self->PlaytestSessionId = Result.Value.SessionId;
				UE_LOG(LogFlockPlaytest, Log, TEXT("Protokite session %s started for this launch, with the player's %s and Flock session %s."),
					*Result.Value.SessionId, bSentSteamId ? TEXT("Steam id") : TEXT("device id"),
					SentFlockSessionId.IsEmpty() ? TEXT("(none)") : *SentFlockSessionId);
				return;
			}

			Self->SessionState = EFlockPlaytestSessionState::StartFailed;
			if (Result.Error.StatusCode == 400)
			{
				// The playtest has closed. The status reports it, and turns playtesting off for the rest of the launch.
				Self->bPlaytestNoLongerCollecting = true;
				Self->RefreshStatus();
				return;
			}
			UE_LOG(LogFlockPlaytest, Warning, TEXT("No Protokite session was started, and none is tried again this launch, because a start that reached Protokite may already have created one. %s"),
				*Result.Error.ToDisplayText());
		});
}

FFlockPlaytestIdentity UFlockPlaytestSubsystem::ResolvePlaytestIdentity(FString& OutWhyNone) const
{
	FFlockPlaytestIdentity Identity;

	const FFlockRunningSteamAccount Steam = TestSteamAccountReader ? TestSteamAccountReader() : ReadRunningSteamAccount();
	if (IsUsablePlaytestId(Steam.Id, FlockPlaytestIdentityLimits::SteamIdLength))
	{
		Identity.SteamId = Steam.Id;
		// A name is shown to people, so spaces are fine. One too long for Protokite is left out rather than cut short.
		if (Steam.Nickname.Len() <= FlockPlaytestIdentityLimits::PlayerNameLength)
		{
			Identity.PlayerName = Steam.Nickname;
		}
		return Identity;
	}
	if (!Steam.Id.IsEmpty())
	{
		UE_LOG(LogFlockPlaytest, Warning, TEXT("The running Steam subsystem gave a Steam id Protokite cannot take (longer than %d characters, or containing whitespace): '%s'. This install's device id is sent instead."),
			FlockPlaytestIdentityLimits::SteamIdLength, *Steam.Id);
	}

	const FFlockPlaytestDeviceIdFile File(TestDeviceIdFilePath.IsEmpty() ? FFlockPlaytestDeviceIdFile::GetDefaultPath() : TestDeviceIdFilePath);
	switch (File.ReadOrCreate(Identity.DeviceId))
	{
	case EFlockDeviceIdFileResult::Read:
	case EFlockDeviceIdFileResult::Created:
		break;
	case EFlockDeviceIdFileResult::Replaced:
		UE_LOG(LogFlockPlaytest, Warning, TEXT("The device id file %s did not hold a device id, so a new one replaced it. Protokite sees this install as a new player from now on."),
			*File.GetPath());
		break;
	case EFlockDeviceIdFileResult::Unreadable:
		OutWhyNone = FString::Printf(TEXT("no Steam subsystem gave a Steam id, and the device id file %s exists but could not be read, so it was left alone."),
			*File.GetPath());
		break;
	case EFlockDeviceIdFileResult::CouldNotSave:
		OutWhyNone = FString::Printf(TEXT("no Steam subsystem gave a Steam id, and no device id could be saved to %s. An id that changed on every launch would show one player as many."),
			*File.GetPath());
		break;
	}
	return Identity;
}

void UFlockPlaytestSubsystem::ApplyStatus(EFlockPlaytestStatus NewStatus, const FString& ProtokiteApiUrl,
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
	const FString Description = DescribePlaytestStatus(Status);
	switch (Status)
	{
	case EFlockPlaytestStatus::TurnedOff:
	case EFlockPlaytestStatus::Stopped:
		UE_LOG(LogFlockPlaytest, Verbose, TEXT("%s"), *Description);
		break;
	case EFlockPlaytestStatus::ProtokiteApiUrlMissing:
	case EFlockPlaytestStatus::PlaytestNoLongerCollecting:
		UE_LOG(LogFlockPlaytest, Warning, TEXT("%s"), *Description);
		break;
	case EFlockPlaytestStatus::ProtokiteApiUrlUnusable:
		// Quoted, so a leading or trailing space is visible in the log.
		UE_LOG(LogFlockPlaytest, Warning, TEXT("%s Current value: '%s'."), *Description, *ProtokiteApiUrl);
		break;
	case EFlockPlaytestStatus::FetchingPlaytestConfig:
	case EFlockPlaytestStatus::WaitingForFlock:
		UE_LOG(LogFlockPlaytest, Log, TEXT("%s"), *Description);
		break;
	case EFlockPlaytestStatus::PlaytestNotLinked:
		UE_LOG(LogFlockPlaytest, Warning, TEXT("%s Game Version ID: %s."), *Description, *GameVersionId);
		break;
	case EFlockPlaytestStatus::ProtokiteRefusedApiKey:
	case EFlockPlaytestStatus::PlaytestConfigUnavailable:
	case EFlockPlaytestStatus::PlaytestConfigForAnotherVersion:
		UE_LOG(LogFlockPlaytest, Warning, TEXT("%s %s"), *Description, *ConfigFailureMessage);
		break;
	case EFlockPlaytestStatus::Ready:
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
		UE_LOG(LogFlockPlaytest, Log, TEXT("%s Playtest: %s. Features on: %s. Protokite API URL: %s. Game Version ID: %s."),
			*Description, *PlaytestConfig.TestId, *FeatureList, *ProtokiteApiUrl, *GameVersionId);
		break;
	}
	}
}

bool UFlockPlaytestSubsystem::RecordPlaytestEvent(const FString& EventName, const FFlockCommandData& Properties)
{
	// The status and the Flock SDK are game-thread state. A game recording from a worker thread has done nothing wrong,
	// so the call is handed over rather than refused, which means it cannot be judged here.
	if (!IsInGameThread())
	{
		const TWeakObjectPtr<UFlockPlaytestSubsystem> WeakThis(this);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, EventName, Properties]()
		{
			if (UFlockPlaytestSubsystem* Self = WeakThis.Get())
			{
				Self->RecordPlaytestEvent(EventName, Properties);
			}
		});
		return true;
	}

	// One sender per name, so a chart built on the plugin's own events never counts one of the game's.
	if (EventName.Equals(FlockPlaytestEvents::PerformanceWindow, ESearchCase::CaseSensitive)
		|| EventName.Equals(FlockPlaytestEvents::LevelLoaded, ESearchCase::CaseSensitive))
	{
		UE_LOG(LogFlockPlaytest, Warning, TEXT("Playtest event '%s' refused: the plugin sends events with that name itself."), *EventName);
		return false;
	}
	return SendPlaytestEvent(EventName, Properties);
}

void UFlockPlaytestSubsystem::UpdatePerformanceTimeline()
{
	const bool bHeavyAnalyticsOn = IsPlaytestFeatureEnabled(FlockPlaytestFeatures::HeavyAnalytics);
	const bool bFlockAnalyticsOn = Flock.IsValid() && Flock->GetAnalyticsProvider() != nullptr;
	if (bHeavyAnalyticsOn && !bFlockAnalyticsOn && !bLoggedHeavyAnalyticsWithoutFlockAnalytics)
	{
		bLoggedHeavyAnalyticsWithoutFlockAnalytics = true;
		UE_LOG(LogFlockPlaytest, Warning, TEXT("This playtest turns heavy analytics on, but the Flock SDK's analytics is off, so no performance or playtest event is recorded. Turn on Analytics Enabled: Project Settings > Plugins > Flock SDK Settings."));
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

		PerformanceFrameHandle = PerformancePump.OnTick.AddUObject(this, &UFlockPlaytestSubsystem::HandlePerformanceFrame);
		BackgroundChangedHandle = PerformancePump.OnBackgroundChanged.AddUObject(this, &UFlockPlaytestSubsystem::HandleBackgroundChanged);
		PreLoadMapHandle = FCoreUObjectDelegates::PreLoadMapWithContext.AddUObject(this, &UFlockPlaytestSubsystem::HandlePreLoadMap);
		PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &UFlockPlaytestSubsystem::HandlePostLoadMap);
		PerformancePump.Start();

		UE_LOG(LogFlockPlaytest, Log, TEXT("Heavy analytics is on: a performance window for every %.0f seconds of play, and every level load, go to the Flock SDK as '%s' events."),
			FFlockPlaytestPerformanceTimeline::WindowSeconds, FlockPlaytestEvents::Category);
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
	UE_LOG(LogFlockPlaytest, Verbose, TEXT("Heavy analytics stopped, and the unfinished performance window was dropped."));
}

void UFlockPlaytestSubsystem::HandlePerformanceFrame(float FrameSeconds)
{
	FFlockPlaytestPerformanceWindow Window;
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
	SendPlaytestEvent(FlockPlaytestEvents::PerformanceWindow, Properties);
}

void UFlockPlaytestSubsystem::HandleBackgroundChanged(bool /*bBackgrounded*/)
{
	// The ticker passes on no frames while the game is in the background, so the first frame time after the return is the
	// one that can carry the time away. Marking it when the game leaves or when it comes back leaves out that same frame.
	PerformanceTimeline.LeaveOutNextFrame();
}

void UFlockPlaytestSubsystem::HandlePreLoadMap(const FWorldContext& WorldContext, const FString& MapName)
{
	NoteLevelLoadStarted(WorldContext.OwningGameInstance);
}

void UFlockPlaytestSubsystem::HandlePostLoadMap(UWorld* LoadedWorld)
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

void UFlockPlaytestSubsystem::NoteLevelLoadStarted(const UGameInstance* LoadingGameInstance)
{
	// The engine announces every game instance's loads; while playing in the editor, several run side by side.
	if (LoadingGameInstance == nullptr || LoadingGameInstance != GetGameInstance())
	{
		return;
	}
	LevelLoadStartSeconds = FPlatformTime::Seconds();
}

void UFlockPlaytestSubsystem::NoteLevelLoaded(const UGameInstance* LoadingGameInstance, const FString& MapName)
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
	SendPlaytestEvent(FlockPlaytestEvents::LevelLoaded, Properties);
}

bool UFlockPlaytestSubsystem::SendPlaytestEvent(const FString& EventName, const FFlockCommandData& Properties)
{
	if (!IsPlaytestFeatureEnabled(FlockPlaytestFeatures::HeavyAnalytics))
	{
		return false;
	}
	FFlockAnalyticsProvider* Analytics = Flock.IsValid() ? Flock->GetAnalyticsProvider() : nullptr;
	return Analytics != nullptr && Analytics->TrackEvent(EventName, Properties, FlockPlaytestEvents::Category);
}

uint64 UFlockPlaytestSubsystem::GetEngineFrameNumber() const
{
	return TestEngineFrameNumberReader ? TestEngineFrameNumberReader() : GFrameCounter;
}

bool UFlockPlaytestSubsystem::IsRecordingVideo() const
{
	return VideoRecording.IsValid() && VideoRecording->IsCapturing();
}

bool UFlockPlaytestSubsystem::StopVideoRecording()
{
	if (!IsRecordingVideo())
	{
		return false;
	}
	VideoRecording->StopCapturing(EFlockPlaytestVideoStopReason::StoppedByGame);
	return true;
}

bool UFlockPlaytestSubsystem::StartTestVideoRecording(double Seconds)
{
#if UE_BUILD_SHIPPING
	return false;
#else
	if (Seconds <= 0.0)
	{
		UE_LOG(LogFlockPlaytest, Warning, TEXT("No test video is recorded: it needs a length above 0 seconds, and %.2f was asked for."), Seconds);
		return false;
	}
	// The launch's one recording belongs to the playtest; a test video would take its place and cut it short.
	if (IsPlaytestFeatureEnabled(FlockPlaytestFeatures::VideoRecording))
	{
		UE_LOG(LogFlockPlaytest, Log, TEXT("No test video is recorded: this launch records video for its playtest."));
		return false;
	}
	if (VideoRecording.IsValid() || bVideoRecordingStartedThisLaunch)
	{
		UE_LOG(LogFlockPlaytest, Log, TEXT("No test video is recorded: %s, and a launch records one video."),
			IsRecordingVideo() ? TEXT("a video is already being recorded") : TEXT("this launch has already recorded one"));
		return false;
	}
	if (!VideoRecordingUnavailableReason.IsEmpty())
	{
		UE_LOG(LogFlockPlaytest, Log, TEXT("No test video is recorded: %s."), *VideoRecordingUnavailableReason);
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
		UE_LOG(LogFlockPlaytest, Log, TEXT("The test video starts once this game instance has a viewport to record."));
		return true;
	}
	return false;
#endif
}

void UFlockPlaytestSubsystem::WaitUntilVideoWrittenForTesting()
{
	if (VideoRecording.IsValid())
	{
		VideoRecording->WaitUntilWritten();
	}
}

bool UFlockPlaytestSubsystem::IsVideoRecordingWanted() const
{
	// A subsystem that has shut down records nothing, whatever asked for it.
	if (Status == EFlockPlaytestStatus::Stopped)
	{
		return false;
	}
	return IsPlaytestFeatureEnabled(FlockPlaytestFeatures::VideoRecording) || bTestVideoRequested || IsRecordVideoInPlayInEditorOn();
}

bool UFlockPlaytestSubsystem::IsRecordVideoInPlayInEditorOn() const
{
#if WITH_EDITOR
	const UGameInstance* GameInstance = GetGameInstance();
	if (!GIsEditor || GEngine == nullptr || GameInstance == nullptr || !GetDefault<UFlockPlaytestLocalSettings>()->bRecordVideoInPlayInEditor)
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

bool UFlockPlaytestSubsystem::IsWaitingToStartVideoRecording() const
{
	return IsVideoRecordingWanted() && !bVideoRecordingStartedThisLaunch && VideoRecordingUnavailableReason.IsEmpty();
}

void UFlockPlaytestSubsystem::UpdateVideoRecording()
{
	if (VideoRecording.IsValid())
	{
		// A recording that has already stopped ignores this.
		if (!IsVideoRecordingWanted())
		{
			VideoRecording->StopCapturing(EFlockPlaytestVideoStopReason::PlaytestStopped);
		}
	}
	else if (IsWaitingToStartVideoRecording())
	{
		StartVideoRecordingWhenPossible();
	}
	UpdateVideoPump();
}

void UFlockPlaytestSubsystem::StartVideoRecordingWhenPossible()
{
	FFlockPlaytestVideoSettings Settings = FFlockPlaytestVideoSettings::FromProjectSettings(*GetDefault<UFlockPlaytestSettings>());
	if (bTestVideoRequested)
	{
		Settings.MaxSeconds = FMath::Min(Settings.MaxSeconds, TestVideoSeconds);
	}

	FString WhyNot;
	const TSharedPtr<IFlockPlaytestVideoFrameSource> Source = TestVideoFrameSourceFactory
		? TestVideoFrameSourceFactory(Settings.MaxVideoSize, WhyNot)
		: FFlockPlaytestGameViewportFrameSource::Create(GetGameInstance(), Settings.MaxVideoSize, WhyNot);
	if (!Source.IsValid())
	{
		// No reason means the game viewport is not there yet, and the next frame asks again. A reason does not change
		// during a launch, so it is logged once and never asked again.
		if (!WhyNot.IsEmpty())
		{
			VideoRecordingUnavailableReason = WhyNot;
			UE_LOG(LogFlockPlaytest, Warning, TEXT("No video is recorded this launch: %s. Everything else in the playtest carries on."), *WhyNot);
		}
		return;
	}

	const FString WhyRecording = IsPlaytestFeatureEnabled(FlockPlaytestFeatures::VideoRecording) ? TEXT("for the playtest")
		: bTestVideoRequested ? TEXT("as a test video") : TEXT("because Record Video In Play In Editor is on");
	const FString FilePath = MakeVideoRecordingPath();

	// One attempt per launch, whether or not it starts: a second recording would replace the first when uploaded.
	bVideoRecordingStartedThisLaunch = true;
	FString StartError;
	VideoRecording = FFlockPlaytestVideoRecording::Start(Source.ToSharedRef(), Settings, FilePath, StartError, TestBeforeEachVideoEncode);
	if (!VideoRecording.IsValid())
	{
		UE_LOG(LogFlockPlaytest, Warning, TEXT("No video is recorded this launch: the recording could not start, because %s."), *StartError);
		return;
	}

	const FIntPoint FrameSize = Source->GetFrameSize();
	UE_LOG(LogFlockPlaytest, Log, TEXT("Recording video %s to %s, at %dx%d and %d frames a second. It stops for good after %.0f seconds of play, before the file passes %lld MB, or when the game stops it."),
		*WhyRecording, *FilePath, FrameSize.X, FrameSize.Y, Settings.FramesPerSecond, Settings.MaxSeconds, Settings.MaxBytes / (1024 * 1024));
}

FString UFlockPlaytestSubsystem::MakeVideoRecordingPath() const
{
	const FString Folder = TestVideoRecordingFolder.IsEmpty()
		? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FlockPlaytest"), TEXT("Recordings"))
		: TestVideoRecordingFolder;
	const FString Prefix = IsPlaytestFeatureEnabled(FlockPlaytestFeatures::VideoRecording) ? TEXT("recording") : TEXT("test-recording");
	const FString Name = FString::Printf(TEXT("%s-%s-%s.ivf"), *Prefix, *FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S")),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower());
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(Folder, Name));
}

void UFlockPlaytestSubsystem::UpdateVideoPump()
{
	// Ticks while a recording captures or its file is being written, and while one waits for the game viewport.
	const bool bShouldTick = VideoRecording.IsValid() || IsWaitingToStartVideoRecording();
	if (bShouldTick == VideoPump.IsRunning())
	{
		return;
	}
	if (bShouldTick)
	{
		VideoFrameHandle = VideoPump.OnTick.AddUObject(this, &UFlockPlaytestSubsystem::HandleVideoFrame);
		VideoBackgroundChangedHandle = VideoPump.OnBackgroundChanged.AddUObject(this, &UFlockPlaytestSubsystem::HandleVideoBackgroundChanged);
		VideoPump.Start();
		return;
	}
	VideoPump.Stop();
	VideoPump.OnTick.Remove(VideoFrameHandle);
	VideoPump.OnBackgroundChanged.Remove(VideoBackgroundChangedHandle);
	VideoFrameHandle.Reset();
	VideoBackgroundChangedHandle.Reset();
}

void UFlockPlaytestSubsystem::HandleVideoFrame(float FrameSeconds)
{
	if (!VideoRecording.IsValid())
	{
		UpdateVideoRecording();
		return;
	}
	// A recording that has stopped capturing takes no frame; it is only waited on.
	const TOptional<EFlockPlaytestVideoStopReason> StopReason = VideoRecording->AddFrame(FrameSeconds);
	if (StopReason.IsSet())
	{
		VideoRecording->StopCapturing(StopReason.GetValue());
	}
	if (VideoRecording.IsValid() && VideoRecording->HasFinishedWriting())
	{
		ApplyFinishedVideoRecording();
	}
}

void UFlockPlaytestSubsystem::HandleVideoBackgroundChanged(bool /*bBackgrounded*/)
{
	// The ticker passes on no frames while the game is away, so the first frame time after the return carries that time.
	if (VideoRecording.IsValid())
	{
		VideoRecording->LeaveOutNextFrame();
	}
}

void UFlockPlaytestSubsystem::ApplyFinishedVideoRecording()
{
	const FFlockPlaytestVideoRecordingSummary Summary = VideoRecording->GetSummary();
	VideoRecording.Reset();

	if (!Summary.Error.IsEmpty())
	{
		UE_LOG(LogFlockPlaytest, Warning, TEXT("The video recording could not be written, so no file was kept: %s. It had stopped because %s."),
			*Summary.Error, *DescribeVideoStopReason(Summary.StopReason));
	}
	else if (Summary.FilePath.IsEmpty())
	{
		UE_LOG(LogFlockPlaytest, Log, TEXT("The video recording stopped because %s before any frame was captured, so no file was kept."),
			*DescribeVideoStopReason(Summary.StopReason));
	}
	else
	{
		FinishedVideoRecordingPath = Summary.FilePath;
		UE_LOG(LogFlockPlaytest, Log, TEXT("Video saved to %s: %.1f seconds, %d frames, %.1f MB. It stopped because %s. Encoding took %.2f ms a frame on average and %.2f ms at most; a frame waited at most %.0f ms to be encoded, and one write took at most %.2f ms; %d frames were dropped because encoding fell behind, %d because writing the file fell behind, and %d capture times passed while earlier frames were still on their way. The file is VP9 video in the IVF format, which VLC plays."),
			*Summary.FilePath, Summary.VideoSeconds, Summary.FramesWritten, Summary.BytesWritten / (1024.0 * 1024.0),
			*DescribeVideoStopReason(Summary.StopReason), Summary.AverageEncodeMs, Summary.LongestEncodeMs,
			Summary.LongestWaitToEncodeMs, Summary.LongestWriteMs, Summary.FramesDroppedBecauseEncodingFellBehind,
			Summary.FramesDroppedBecauseWritingFellBehind, Summary.FramesNotReadyInTime);
	}
	UpdateVideoPump();
}

void UFlockPlaytestSubsystem::FinishVideoRecordingNow(EFlockPlaytestVideoStopReason Reason)
{
	if (VideoRecording.IsValid())
	{
		VideoRecording->StopCapturing(Reason);
		VideoRecording->WaitUntilWritten();
		ApplyFinishedVideoRecording();
	}
	UpdateVideoPump();
}

#if !UE_BUILD_SHIPPING
namespace
{
	UFlockPlaytestSubsystem* FindPlaytestSubsystemOfWorld(const UWorld* World)
	{
		const UGameInstance* GameInstance = World != nullptr ? World->GetGameInstance() : nullptr;
		return GameInstance != nullptr ? GameInstance->GetSubsystem<UFlockPlaytestSubsystem>() : nullptr;
	}

	FAutoConsoleCommandWithWorldArgsAndOutputDevice RecordTestVideoCommand(
		TEXT("FlockPlaytest.RecordTestVideo"),
		TEXT("Records this game instance's screen for the given number of seconds, with no playtest needed: FlockPlaytest.RecordTestVideo 60. The file goes to Saved/FlockPlaytest/Recordings and is never uploaded. One video per launch. Not in Shipping builds."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Output)
		{
			double Seconds = 0.0;
			if (Args.Num() != 1 || !LexTryParseString(Seconds, *Args[0]) || Seconds <= 0.0)
			{
				Output.Log(TEXT("Usage: FlockPlaytest.RecordTestVideo <seconds>, with seconds above 0."));
				return;
			}
			UFlockPlaytestSubsystem* Playtest = FindPlaytestSubsystemOfWorld(World);
			if (Playtest == nullptr)
			{
				Output.Log(TEXT("No Flock Playtest subsystem runs in this world's game instance."));
				return;
			}
			Playtest->StartTestVideoRecording(Seconds);
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice StopVideoRecordingCommand(
		TEXT("FlockPlaytest.StopVideoRecording"),
		TEXT("Stops this game instance's video recording for good, and saves the file."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Output)
		{
			UFlockPlaytestSubsystem* Playtest = FindPlaytestSubsystemOfWorld(World);
			if (Playtest == nullptr || !Playtest->StopVideoRecording())
			{
				Output.Log(TEXT("No video is being recorded."));
			}
		}));
}
#endif
