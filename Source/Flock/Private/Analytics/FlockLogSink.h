// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/BlueprintExceptionInfo.h"
#include "Containers/Queue.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/ThreadSafeCounter.h"
#include "Misc/OutputDevice.h"

struct FFrame;

/** One captured fault, in the shape the provider turns into an `exception` log event. */
struct FFlockCapturedLog
{
	FString Message;
	FName Category;
	bool bFatal = false;
	FDateTime TimestampUtc = FDateTime::MinValue();

	/** Where it came from: `log` (an Error or Fatal line), `blueprint` (a script exception) or `crash`. */
	FString Source;

	/** For a Blueprint exception, its kind in wire spelling (`access_violation`, `infinite_loop`, ...). */
	FString ScriptExceptionType;

	/**
	 * Callstack walked at the moment of capture. Empty only when the platform could not produce one
	 * — an exception report without it is close to useless, which is the whole reason capture is
	 * worth its cost.
	 */
	FString StackTrace;
};

/**
 * Automatic exception capture, by tapping the engine log.
 *
 * UE has no managed exception stream, so the closest equivalent is tapping the log. This registers
 * an FOutputDevice on GLog and takes Error and Fatal lines, plus FCoreDelegates::OnHandleSystemError
 * for the hard crashes that never reach the log at all. A crash is reported once, however many times
 * the engine raises its crash delegates for it, and a Fatal line counts as that report.
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
 *
 * Blueprint script exceptions (Accessed None, a missing property, a runaway loop) arrive through
 * FBlueprintCoreDelegates::OnScriptException, not the log: the engine logs their description as a Warning,
 * which this sink deliberately ignores. They are queued like errors. Breakpoints and tracepoints travel the
 * same delegate and are the debugger's business, so they are skipped.
 *
 * Listening has a side effect worth undoing. The engine writes a script exception's stack trace to the log
 * only while nothing is bound to that delegate, so binding would quietly take that line away from the
 * developer. When this sink is the first listener it writes the line back.
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

	/** Test seam: drive the crash path without crashing, with what the engine wrote down about the error. */
	void SimulateSystemErrorForTesting(const TCHAR* EngineErrorRecord = TEXT("")) { HandleSystemError(EngineErrorRecord); }

	/**
	 * The message a crash report carries: the first line of what the engine wrote down about the error (an assertion's
	 * expression, file and line, or an unhandled exception's code), or "Unhandled system error" when it wrote nothing.
	 */
	static FString DescribeCrash(const TCHAR* EngineErrorRecord);

	/** Faults only. Breakpoints and tracepoints ride the same delegate for the debugger's sake. */
	static bool IsReportableScriptException(EBlueprintExceptionType::Type Type);

	/** The wire spelling recorded with a Blueprint exception. Empty for a type that is never reported. */
	static FString ScriptExceptionTypeToWire(EBlueprintExceptionType::Type Type);

	/** True while bound to Blueprint script exceptions. */
	bool IsListeningToScriptExceptions() const { return ScriptExceptionHandle.IsValid(); }

	/** Test seam: the Blueprint path without a script VM frame to raise it from. */
	void SimulateScriptExceptionForTesting(EBlueprintExceptionType::Type Type, const FString& Description,
		const FString& ScriptStack)
	{
		CaptureScriptException(Type, Description, ScriptStack);
	}

	/** Test seam: how many times the engine's own Blueprint stack-trace warning was written back. */
	int32 GetWrittenBackScriptWarningCountForTesting() const { return WrittenBackScriptWarnings; }

	/** Test seam: sets the first-listener decision Start() made, so both answers can be exercised in any process. */
	void SetWriteBackScriptWarningForTesting(bool bWriteBack) { bWriteBackScriptWarning = bWriteBack; }

private:
	void HandleSystemError(const TCHAR* EngineErrorRecord);

	/**
	 * True the first time only. A process crashes once, but the engine raises its crash delegates up to three times for
	 * that one crash (measured on Windows: OnHandleSystemError from the crash-reporting thread and again from the error
	 * handler, then OnShutdownAfterError), and a Fatal log line is followed by the same delegates.
	 */
	bool ClaimTheCrashReport() { return !bCrashReported.AtomicSet(true); }
	void HandleScriptException(const UObject* ActiveObject, const FFrame& StackFrame, const FBlueprintExceptionInfo& Info);
	void CaptureScriptException(EBlueprintExceptionType::Type Type, const FString& Description, const FString& ScriptStack);

	int32 MaxQueued = DefaultMaxQueued;
	TQueue<FFlockCapturedLog, EQueueMode::Mpsc> Queue;
	FThreadSafeCounter QueuedCount;
	FThreadSafeCounter DroppedCount;

	TSet<FName> ExcludedCategories;

	FDelegateHandle SystemErrorHandle;
	FDelegateHandle ShutdownAfterErrorHandle;
	FDelegateHandle ScriptExceptionHandle;

	/** Set at Start when nothing else was listening, which is exactly when the engine would have logged the stack. */
	bool bWriteBackScriptWarning = false;
	int32 WrittenBackScriptWarnings = 0;

	bool bRunning = false;

	/** Set once this process's crash has been reported. Read and written from whichever thread is crashing. */
	FThreadSafeBool bCrashReported;
};
