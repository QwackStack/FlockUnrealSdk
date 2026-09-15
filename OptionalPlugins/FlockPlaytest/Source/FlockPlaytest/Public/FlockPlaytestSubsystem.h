// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockPlaytestConfig.h"
#include "FlockPlaytestIdentity.h"
#include "FlockPlaytestSession.h"
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
 * It also runs the launch's one Protokite session. The session starts once the playtest config is loaded and the
 * current Flock initialization's first session has reached the server, whichever of the two happens last, and it
 * names that Flock session. A Flock session only exists after a player signs in, with the Flock SDK's analytics on,
 * Analytics Auto Start Session on (or a Start Session call) and consent granted when Analytics Require Explicit
 * Consent is on. A later Flock session, a sign-out or the Flock SDK shutting down neither ends nor restarts it, and
 * neither does the Flock SDK initializing again with another API key or Game Version ID: the session keeps the
 * address and headers it started with. It ends when the game instance shuts down, or when EndPlaytestSession is
 * called.
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

	/** Where this launch's Protokite session is. Reading it changes nothing. */
	EFlockPlaytestSessionState GetPlaytestSessionState() const { return SessionState; }

	/** The id Protokite gave this launch's session. Empty until the session has started, and kept once it has ended. */
	const FString& GetPlaytestSessionId() const { return PlaytestSessionId; }

	/** The identity this launch's session start sent. Empty until a start has resolved it. */
	const FFlockPlaytestIdentity& GetPlaytestIdentity() const { return PlaytestIdentity; }

	/**
	 * Ends this launch's Protokite session now, for a game that quits on its own schedule. Protokite ignores an end
	 * for a session that has already ended, so calling this twice sends two ends. Returns false, and sends nothing,
	 * when no session has started. The session also ends by itself when the game instance shuts down.
	 */
	bool EndPlaytestSession();

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

	/** Reads the Steam account through Reader instead of the engine's Steam subsystem. Call before following. */
	void SetSteamAccountReaderForTesting(TFunction<FFlockRunningSteamAccount()> Reader) { TestSteamAccountReader = MoveTemp(Reader); }

	/** Keeps the device id in the given file instead of the default one under Saved. Call before following. */
	void SetDeviceIdFilePathForTesting(const FString& Path) { TestDeviceIdFilePath = Path; }

private:
	/**
	 * Starts following the Flock SDK's initialize, shut-down and session events, and decides the status straight
	 * away, so a Flock SDK that initialized before this was called is seen without waiting for an event that has
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

	/** Remembers the current Flock initialization's first session to reach the server, and starts the Protokite session when it can. */
	UFUNCTION()
	void HandleFlockSessionRegistered(const FString& SessionId, const FString& ServerSessionId);

	/**
	 * Forgets a config that belongs to a Flock initialization which has ended, starts a fetch when the status
	 * calls for one, applies the status, and starts the Protokite session when it can.
	 */
	void RefreshStatus();

	/** The status the current settings, Flock SDK and config state add up to. Changes nothing. */
	EFlockPlaytestStatus DecideCurrentStatus() const;

	/** The Protokite client, created on first use with the Flock SDK's timeout and retry settings. */
	TSharedRef<FFlockProtokiteClient> GetOrCreateProtokiteClient();

	/** Sends the playtest-config request for the current Flock initialization. */
	void StartPlaytestConfigFetch();

	/**
	 * Drops the config, stops a request that is still being sent or retried, and makes any reply still on its
	 * way stale. The one place a config is forgotten.
	 */
	void ForgetPlaytestConfig();

	/**
	 * Sends this launch's session start once the status is Ready and a Flock session has reached the server, and
	 * does nothing otherwise. The one place a start is sent, and it sends one at most per launch.
	 */
	void StartPlaytestSessionWhenAllowed();

	/**
	 * The Steam id when a Steam subsystem is running, otherwise this install's device id. When neither can be had,
	 * the identity is empty and OutWhyNone says why.
	 */
	FFlockPlaytestIdentity ResolvePlaytestIdentity(FString& OutWhyNone) const;

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

	/** Set when Protokite refused the session because the playtest has closed. Keeps playtesting off for the launch. */
	bool bPlaytestNoLongerCollecting = false;

	/** The server id of the current Flock initialization's first session to reach the server; empty until one has. */
	FString FirstFlockServerSessionId;

	EFlockPlaytestSessionState SessionState = EFlockPlaytestSessionState::NotStarted;
	FString PlaytestSessionId;
	FFlockPlaytestIdentity PlaytestIdentity;

	/** The Protokite API URL and headers the session start used. The end is sent with the same ones. */
	FString PlaytestSessionApiUrl;
	TMap<FString, FString> PlaytestSessionHeaders;

	/** Whether waiting for a Flock session has been logged this launch. */
	bool bLoggedWaitingForFlockSession = false;

	TSharedPtr<FFlockProtokiteClient> ProtokiteClient;
	TSharedPtr<IFlockHttpAdapter> TestHttpAdapter;
	TOptional<FFlockRetryPolicy> TestRetryPolicy;
	TFunction<FFlockRunningSteamAccount()> TestSteamAccountReader;
	FString TestDeviceIdFilePath;
};
