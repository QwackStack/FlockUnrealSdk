// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"

/** Work the relay runs on a thread of its own: a relay connection's, or one of a bridge's loopback sockets. */
class IFlockRelayThreadWork : public FRunnable
{
public:
	/** True once Run has returned, so its thread can be cleaned up at once. Any thread. */
	virtual bool HasEnded() const = 0;

	/** Flock's module is going away: Run returns within one wait, sending nothing more. Any thread. */
	virtual void EndNow() = 0;
};

/**
 * Every thread the relay runs, so the game thread cleans each up once it ended (the core ticker, once a second) and Flock's
 * module waits for those still running when it goes away. A thread keeps its work alive until it is cleaned up, so work its
 * owner dropped still finishes what it must (a relay connection gives its address back).
 */
class FFlockRelayThreads
{
public:
	/**
	 * Starts Work on a thread of its own; false when no thread can be started, as in a launch with -nothreading, and while
	 * EndEveryThread runs. Any thread.
	 */
	static bool Start(const TSharedRef<IFlockRelayThreadWork, ESPMode::ThreadSafe>& Work, const TCHAR* Name, uint32 StackBytes);

	/** Tells every thread still running to end at once and waits for each: Flock's module is going away. */
	static void EndEveryThread();

	/** Threads not yet ended and cleaned up. */
	static int32 CountForTesting();

	/** True while the core ticker cleans up ended threads once a second: only while any thread is listed. */
	static bool IsCleaningUpForTesting();

	/** Makes Start answer false, as in a launch without threads. */
	static bool bRefusedForTesting;
};
