// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockPlaytestSubsystem.h"

#include "Config/FlockConfig.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FlockEvents.h"
#include "FlockPlaytestLog.h"
#include "FlockPlaytestLogger.h"
#include "FlockPlaytestSettings.h"
#include "FlockProtokiteClient.h"
#include "FlockSubsystem.h"
#include "Http/FlockHttpClient.h"

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

	// After the status is applied, because a start needs Ready.
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
	const UWorld* World = GameInstance != nullptr ? GameInstance->GetWorld() : nullptr;
	Request.DebugInfo = MakePlaytestSessionDebugInfo(World != nullptr ? UWorld::RemovePIEPrefix(World->GetMapName()) : FString());

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
