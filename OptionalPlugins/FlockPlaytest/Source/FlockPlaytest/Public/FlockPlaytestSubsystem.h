// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockPlaytestStatus.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "FlockPlaytestSubsystem.generated.h"

class UFlockSubsystem;

/**
 * The playtest plugin's runtime home, one per game instance.
 *
 * Keeps one answer current: whether this build may do playtest work (GetStatus). It decides when it starts,
 * and again whenever the Flock SDK initializes or shuts down, and moves to Stopped when its game instance
 * shuts down. Each change is logged once: a setting that stops playtesting is a warning, waiting for the
 * Flock SDK and being ready are logged, and playtesting turned off stays quiet, because that is the chosen
 * state of every build that is not a playtest build.
 */
UCLASS()
class FLOCKPLAYTEST_API UFlockPlaytestSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem interface

	/** Whether playtest work may run right now, and if not, why. Reading it changes nothing. */
	EFlockPlaytestStatus GetStatus() const { return Status; }

	/**
	 * Starts following a Flock subsystem exactly as Initialize does, for tests that build both subsystems by
	 * hand. Call it once per subsystem.
	 */
	void FollowFlockLifecycleForTesting(UFlockSubsystem* InFlock) { FollowFlockLifecycle(InFlock); }

	/** The Flock subsystem this one follows; null before it starts following one and after it shuts down. */
	UFlockSubsystem* GetFollowedFlockForTesting() const;

private:
	/**
	 * Starts following the Flock SDK's initialize and shut-down events, and decides the status straight away,
	 * so a Flock SDK that initialized before this was called is seen without waiting for an event that has
	 * already fired.
	 */
	void FollowFlockLifecycle(UFlockSubsystem* InFlock);

	UFUNCTION()
	void HandleFlockLifecycleChanged();

	/** Decides the status from the settings and the followed Flock SDK, and applies it. */
	void RefreshStatus();

	/** The only writer of Status. Logs when the status changes. */
	void ApplyStatus(EFlockPlaytestStatus NewStatus, const FString& ProtokiteApiUrl, const FString& GameVersionId);

	TWeakObjectPtr<UFlockSubsystem> Flock;

	EFlockPlaytestStatus Status = EFlockPlaytestStatus::TurnedOff;

	/** False until the first decision, so that decision is logged even when it matches the initial value. */
	bool bStatusDecided = false;
};
