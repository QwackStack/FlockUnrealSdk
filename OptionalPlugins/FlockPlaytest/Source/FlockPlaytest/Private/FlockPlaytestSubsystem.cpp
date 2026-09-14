// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockPlaytestSubsystem.h"

#include "FlockEvents.h"
#include "FlockPlaytestLog.h"
#include "FlockPlaytestSettings.h"
#include "FlockSubsystem.h"

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
	}
	Flock.Reset();

	// Whatever the last status was, no playtest work may run on a subsystem that has shut down.
	ApplyStatus(EFlockPlaytestStatus::Stopped, FString(), FString());

	Super::Deinitialize();
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
	}
	RefreshStatus();
}

void UFlockPlaytestSubsystem::HandleFlockLifecycleChanged()
{
	RefreshStatus();
}

void UFlockPlaytestSubsystem::RefreshStatus()
{
	const UFlockPlaytestSettings* Settings = GetDefault<UFlockPlaytestSettings>();

	FFlockPlaytestStatusInputs Inputs;
	Inputs.bPlaytestingEnabled = Settings->bPlaytestingEnabled;
	Inputs.ProtokiteApiUrl = Settings->ProtokiteApiUrl;
	Inputs.bFlockInitialized = Flock.IsValid() && Flock->IsInitialized();

	ApplyStatus(DecidePlaytestStatus(Inputs), Inputs.ProtokiteApiUrl,
		Flock.IsValid() ? Flock->GetGameVersionId() : FString());
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

	// Loud only when playtesting is turned on: a setting that stops it is a warning, and waiting for the Flock
	// SDK or being ready is logged. Turned off is the chosen state of every build that is not a playtest
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
	case EFlockPlaytestStatus::WaitingForFlock:
		UE_LOG(LogFlockPlaytest, Log, TEXT("%s"), *Description);
		break;
	case EFlockPlaytestStatus::Ready:
		UE_LOG(LogFlockPlaytest, Log, TEXT("%s Protokite API URL: %s. Game Version ID: %s."), *Description,
			*ProtokiteApiUrl, *GameVersionId);
		break;
	}
}
