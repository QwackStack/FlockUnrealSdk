// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockPlaytestConfig.h"
#include "FlockPlaytestStatus.h"
#include "Http/FlockHttpAdapter.h"
#include "Http/FlockRetryPolicy.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "FlockPlaytestSubsystem.generated.h"

class FFlockProtokiteClient;
class UFlockSubsystem;

/**
 * The playtest plugin's runtime home, one per game instance.
 *
 * Keeps one answer current: whether this build may do playtest work (GetStatus). It decides when it starts,
 * and again whenever the Flock SDK initializes or shuts down. Once the settings allow it and the Flock SDK is
 * initialized, it fetches this build's playtest config from Protokite, once per Flock initialization, and
 * forgets it when the Flock SDK shuts down. It moves to Stopped when its game instance shuts down.
 *
 * Each change is logged once: a setting or a refusal that stops playtesting is a warning, waiting, fetching
 * and being ready are logged, and playtesting turned off stays quiet, because that is the chosen state of
 * every build that is not a playtest build.
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

	/** This build's playtest config. Empty unless it has been fetched for the current Flock initialization. */
	const FFlockPlaytestConfig& GetPlaytestConfig() const { return PlaytestConfig; }

	/**
	 * True only while GetStatus() is Ready and the playtest's config turns FeatureName on (see
	 * FlockPlaytestFeatures). A feature the config does not mention is off.
	 */
	bool IsPlaytestFeatureEnabled(const FString& FeatureName) const;

	/**
	 * Starts following a Flock subsystem exactly as Initialize does, for tests that build both subsystems by
	 * hand. Call it once per subsystem.
	 */
	void FollowFlockLifecycleForTesting(UFlockSubsystem* InFlock) { FollowFlockLifecycle(InFlock); }

	/** The Flock subsystem this one follows; null before it starts following one and after it shuts down. */
	UFlockSubsystem* GetFollowedFlockForTesting() const;

	/** Sends Protokite requests through the given transport instead of the engine's HTTP module. Call before following. */
	void SetHttpAdapterForTesting(const TSharedPtr<IFlockHttpAdapter>& InAdapter) { TestHttpAdapter = InAdapter; }

	/** Uses the given retry policy instead of the Flock SDK's HTTP settings. Call before following. */
	void SetRetryPolicyForTesting(const FFlockRetryPolicy& InPolicy) { TestRetryPolicy = InPolicy; }

private:
	/**
	 * Starts following the Flock SDK's initialize and shut-down events, and decides the status straight away,
	 * so a Flock SDK that initialized before this was called is seen without waiting for an event that has
	 * already fired.
	 */
	void FollowFlockLifecycle(UFlockSubsystem* InFlock);

	UFUNCTION()
	void HandleFlockLifecycleChanged();

	/**
	 * Asks Protokite again when a Flock session starts, but only when the last attempt could not reach it. A
	 * refusal would get the same answer twice, and a config already loaded or on its way needs nothing.
	 */
	UFUNCTION()
	void HandleFlockSessionStarted(const FString& SessionId);

	/**
	 * Forgets a config that belongs to a Flock initialization which has ended, starts a fetch when the status
	 * calls for one, and applies the status.
	 */
	void RefreshStatus();

	/** The status the current settings, Flock SDK and config state add up to. Changes nothing. */
	EFlockPlaytestStatus DecideCurrentStatus() const;

	/** Sends the playtest-config request for the current Flock initialization. */
	void StartPlaytestConfigFetch();

	/**
	 * Drops the config, stops a request that is still being sent or retried, and makes any reply still on its
	 * way stale. The one place a config is forgotten.
	 */
	void ForgetPlaytestConfig();

	/** The only writer of Status. Logs when the status changes. */
	void ApplyStatus(EFlockPlaytestStatus NewStatus, const FString& ProtokiteApiUrl, const FString& GameVersionId);

	TWeakObjectPtr<UFlockSubsystem> Flock;

	EFlockPlaytestStatus Status = EFlockPlaytestStatus::TurnedOff;

	/** False until the first decision, so that decision is logged even when it matches the initial value. */
	bool bStatusDecided = false;

	FFlockPlaytestConfig PlaytestConfig;
	EFlockPlaytestConfigState ConfigState = EFlockPlaytestConfigState::NotFetched;

	/** Why the last fetch did not load a config, for the warning that reports it; empty otherwise. */
	FString ConfigFailureMessage;

	/** How many times a config has been forgotten. A reply to a request sent before the latest time is ignored. */
	int32 TimesConfigForgotten = 0;

	/** The playtest-config request for the current Flock initialization, kept so forgetting the config can stop it. */
	FFlockRequestHandle ConfigFetchRequest;

	TSharedPtr<FFlockProtokiteClient> ProtokiteClient;
	TSharedPtr<IFlockHttpAdapter> TestHttpAdapter;
	TOptional<FFlockRetryPolicy> TestRetryPolicy;
};
