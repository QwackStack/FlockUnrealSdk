// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockPlaytestSubsystem.h"

#include "Config/FlockConfig.h"
#include "FlockEvents.h"
#include "FlockPlaytestLog.h"
#include "FlockPlaytestLogger.h"
#include "FlockPlaytestSettings.h"
#include "FlockProtokiteClient.h"
#include "FlockSubsystem.h"
#include "Http/FlockHttpClient.h"

void UFlockPlaytestSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// The Flock subsystem initializes first, so with Auto-Initialize On Load it is already up by the time
	// this starts following it.
	FollowFlockLifecycle(Collection.InitializeDependency<UFlockSubsystem>());
}

void UFlockPlaytestSubsystem::Deinitialize()
{
	// The Flock subsystem can outlive this one during teardown, and its shut-down event would otherwise
	// still reach a subsystem that has finished.
	if (UFlockSubsystem* Followed = Flock.Get())
	{
		UFlockEvents* Events = Followed->GetEvents();
		Events->OnInitialized.RemoveDynamic(this, &UFlockPlaytestSubsystem::HandleFlockLifecycleChanged);
		Events->OnShutdown.RemoveDynamic(this, &UFlockPlaytestSubsystem::HandleFlockLifecycleChanged);
		Events->OnSessionStarted.RemoveDynamic(this, &UFlockPlaytestSubsystem::HandleFlockSessionStarted);
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

void UFlockPlaytestSubsystem::RefreshStatus()
{
	// A config belongs to the Flock initialization it was fetched under. Once the Flock SDK is down, the next
	// initialization may carry another key or version, so the config and any reply still on its way are stale.
	const bool bFlockInitialized = Flock.IsValid() && Flock->IsInitialized();
	if (!bFlockInitialized && ConfigState != EFlockPlaytestConfigState::NotFetched)
	{
		ForgetPlaytestConfig();
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
}

EFlockPlaytestStatus UFlockPlaytestSubsystem::DecideCurrentStatus() const
{
	const UFlockPlaytestSettings* Settings = GetDefault<UFlockPlaytestSettings>();

	FFlockPlaytestStatusInputs Inputs;
	Inputs.bPlaytestingEnabled = Settings->bPlaytestingEnabled;
	Inputs.ProtokiteApiUrl = Settings->ProtokiteApiUrl;
	Inputs.bFlockInitialized = Flock.IsValid() && Flock->IsInitialized();
	Inputs.ConfigState = ConfigState;
	return DecidePlaytestStatus(Inputs);
}

void UFlockPlaytestSubsystem::StartPlaytestConfigFetch()
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

	// Exactly the key and version the Flock SDK initialized with, so the playtest found is this build's.
	const TMap<FString, FString> RequestHeaders = Flock->GetRequestHeaders();
	const FString SentGameVersionId = RequestHeaders.FindRef(TEXT("X-Game-Version-ID"));

	ConfigState = EFlockPlaytestConfigState::Fetching;
	const int32 TimesForgottenWhenSent = TimesConfigForgotten;
	const TWeakObjectPtr<UFlockPlaytestSubsystem> WeakThis(this);
	ConfigFetchRequest = ProtokiteClient->FetchPlaytestConfig(GetDefault<UFlockPlaytestSettings>()->ProtokiteApiUrl,
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
