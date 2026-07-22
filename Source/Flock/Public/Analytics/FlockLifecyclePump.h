// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

/**
 * The per-frame and app-lifecycle signals the analytics feature runs on: the UE stand-in for the
 * MonoBehaviour a Unity SDK would attach to a DontDestroyOnLoad object.
 *
 * FTSTicker rather than an actor or world tick, because analytics must keep running with no world
 * loaded, in the editor, in commandlets, and under -nullrhi — which is also what lets the automation
 * tests drive it. Nothing here knows about sessions; the provider decides what a tick means.
 *
 * Quit fires at most once. UE can raise both ApplicationWillTerminate and OnPreExit for the same
 * shutdown (and on desktop typically raises only the latter), so both are subscribed and the first
 * one through wins — a session must not be ended twice.
 *
 * Game thread only: the ticker and every core delegate here run on it, so the session state the
 * provider mutates in response needs no locking.
 */
class FLOCK_API FFlockLifecyclePump
{
public:
	DECLARE_MULTICAST_DELEGATE_OneParam(FFlockOnPumpTick, float /*DeltaSeconds*/);
	DECLARE_MULTICAST_DELEGATE_OneParam(FFlockOnPumpBackgroundChanged, bool /*bBackgrounded*/);
	DECLARE_MULTICAST_DELEGATE(FFlockOnPumpQuit);

	FFlockLifecyclePump() = default;
	~FFlockLifecyclePump();

	FFlockLifecyclePump(const FFlockLifecyclePump&) = delete;
	FFlockLifecyclePump& operator=(const FFlockLifecyclePump&) = delete;

	/** Idempotent. */
	void Start();
	/** Idempotent, and safe to call from the destructor path. */
	void Stop();
	bool IsRunning() const { return bRunning; }

	/** Fires every frame while running and not backgrounded. */
	FFlockOnPumpTick OnTick;
	/** True when the app went to the background, false when it came back. */
	FFlockOnPumpBackgroundChanged OnBackgroundChanged;
	/** Fires at most once per process. */
	FFlockOnPumpQuit OnQuit;

	// ── Test seams: drive the pump without the engine raising the real signals ──

	/** Routed through the real handler so background gating is covered, not bypassed. */
	void TickForTesting(float DeltaSeconds) { HandleTick(DeltaSeconds); }
	void SetBackgroundedForTesting(bool bInBackgrounded) { HandleBackgroundChanged(bInBackgrounded); }
	void QuitForTesting() { HandleQuit(); }

private:
	bool HandleTick(float DeltaSeconds);
	void HandleBackgroundChanged(bool bBackgrounded);
	void HandleQuit();

	FTSTicker::FDelegateHandle TickerHandle;
	FDelegateHandle BackgroundHandle;
	FDelegateHandle ForegroundHandle;
	FDelegateHandle TerminateHandle;
	FDelegateHandle PreExitHandle;

	bool bRunning = false;
	bool bBackgrounded = false;
	bool bQuitBroadcast = false;
};
