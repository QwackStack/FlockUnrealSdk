// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/ThreadSafeCounter.h"
#include "Misc/OutputDevice.h"

/** One captured log line, in the shape the provider turns into an `exception` log event. */
struct FFlockCapturedLog
{
	FString Message;
	FName Category;
	bool bFatal = false;
	FDateTime TimestampUtc = FDateTime::MinValue();

	/**
	 * Callstack walked at the moment of capture. Empty only when the platform could not produce one
	 * — an exception report without it is close to useless, which is the whole reason capture is
	 * worth its cost.
	 */
	FString StackTrace;
};

/**
 * Automatic exception capture: the UE stand-in for Unity's Application.logMessageReceived.
 *
 * UE has no managed exception stream, so the closest equivalent is tapping the log. This registers
 * an FOutputDevice on GLog and takes Error and Fatal lines, plus FCoreDelegates::OnHandleSystemError
 * for the hard crashes that never reach the log at all.
 *
 * Two things make this safe rather than a footgun:
 *
 * 1. **The SDK's own categories are excluded.** FFlockLogger emits UE_LOG(LogFlock, Error, ...), so
 *    without the filter a failed analytics upload would log an error, which would be captured as an
 *    exception, which would be uploaded, which would fail... A feedback loop that only shows up when
 *    the network is already broken.
 * 2. **A thread-local re-entrancy guard.** Anything this does while handling a line could itself log.
 *
 * Threading: Serialize is called from any thread, but the analytics core is game-thread only. Errors
 * are therefore pushed onto a lock-free queue and drained by the provider on tick — never delivered
 * on the calling thread. The queue is capped so an error storm costs a bounded amount of memory.
 *
 * Fatals are the exception to that: there is no next tick when the process is dying, so they are
 * broadcast synchronously on the crashing thread. A handler for OnFatal must do nothing but write to
 * disk — no network, no allocation it can avoid.
 */
class FFlockLogSink : public FOutputDevice
{
public:
	DECLARE_MULTICAST_DELEGATE_OneParam(FFlockOnFatalCapture, const FFlockCapturedLog&);

	/** Beyond this many undrained entries, new ones are dropped and counted. */
	static constexpr int32 DefaultMaxQueued = 256;

	explicit FFlockLogSink(int32 InMaxQueued = DefaultMaxQueued);
	virtual ~FFlockLogSink() override;

	FFlockLogSink(const FFlockLogSink&) = delete;
	FFlockLogSink& operator=(const FFlockLogSink&) = delete;

	/** Idempotent. Registers on GLog and subscribes to the crash delegates. */
	void Start();
	/** Idempotent, and called by the destructor. */
	void Stop();
	bool IsRunning() const { return bRunning; }

	/** Fired synchronously on the crashing thread. Disk writes only. */
	FFlockOnFatalCapture OnFatal;

	/** Drains one entry. The provider calls this on tick until it returns false. */
	bool Dequeue(FFlockCapturedLog& OutCaptured);

	/** Reads the head without consuming it — lets a diagnostic show what is about to be reported. */
	bool Peek(FFlockCapturedLog& OutCaptured) const;

	int32 PendingCount() const { return QueuedCount.GetValue(); }
	/** Entries lost to the cap — worth reporting rather than hiding. */
	int32 GetDroppedCount() const { return DroppedCount.GetValue(); }

	/** Categories to ignore. LogFlock and LogFlockEditor are excluded from construction. */
	void AddExcludedCategory(FName Category);
	bool IsExcluded(FName Category) const { return ExcludedCategories.Contains(Category); }

	// FOutputDevice
	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override;
	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

	/** Test seam: drive the crash path without crashing. */
	void SimulateSystemErrorForTesting() { HandleSystemError(); }

private:
	void HandleSystemError();

	int32 MaxQueued = DefaultMaxQueued;
	TQueue<FFlockCapturedLog, EQueueMode::Mpsc> Queue;
	FThreadSafeCounter QueuedCount;
	FThreadSafeCounter DroppedCount;

	TSet<FName> ExcludedCategories;

	FDelegateHandle SystemErrorHandle;
	FDelegateHandle ShutdownAfterErrorHandle;
	bool bRunning = false;
};
