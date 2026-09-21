// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Analytics/FlockLogSink.h"

#include "Analytics/FlockStackTrace.h"
#include "CoreGlobals.h"
#include "Misc/CoreDelegates.h"
#include "Misc/OutputDeviceRedirector.h"
#include "UObject/Script.h"
#include "UObject/Stack.h"

namespace
{
	/** Script exceptions are filed under the category the engine logs them in, so an exclusion list reads naturally. */
	const FName ScriptCategory(TEXT("LogScript"));

	/**
	 * Frames dropped so a trace starts at the logging site: FFlockStackTrace::Capture, this sink's
	 * caller of it, and Serialize itself. See FlockStackTrace.h for why it cannot also strip the
	 * engine's UE_LOG dispatch frames above Serialize.
	 */
	constexpr uint32 StackFramesToSkip = 3;

	/**
	 * Per-thread, because Serialize runs on any thread. Guards against a capture path that itself
	 * logs — which would otherwise recurse until the stack gave out.
	 */
	thread_local bool bFlockLogSinkReentrant = false;

	struct FScopedReentrancyGuard
	{
		bool bEntered = false;

		FScopedReentrancyGuard()
		{
			if (!bFlockLogSinkReentrant)
			{
				bFlockLogSinkReentrant = true;
				bEntered = true;
			}
		}

		~FScopedReentrancyGuard()
		{
			if (bEntered)
			{
				bFlockLogSinkReentrant = false;
			}
		}
	};
}

FFlockLogSink::FFlockLogSink(int32 InMaxQueued)
	: MaxQueued(FMath::Max(InMaxQueued, 0))
{
	// The SDK's own error logs must never become exceptions the SDK then tries to upload.
	ExcludedCategories.Add(FName(TEXT("LogFlock")));
	ExcludedCategories.Add(FName(TEXT("LogFlockEditor")));
}

FFlockLogSink::~FFlockLogSink()
{
	Stop();
}

void FFlockLogSink::Start()
{
	if (bRunning)
	{
		return;
	}
	bRunning = true;

	if (GLog != nullptr)
	{
		GLog->AddOutputDevice(this);
	}

	// GErrorHist is where the engine writes down what went wrong, before it raises either delegate.
	SystemErrorHandle = FCoreDelegates::OnHandleSystemError.AddLambda([this]() { HandleSystemError(GErrorHist); });
	ShutdownAfterErrorHandle = FCoreDelegates::OnShutdownAfterError.AddLambda([this]() { HandleSystemError(GErrorHist); });

	// Read before binding: once anything is bound, the engine stops logging a script exception's stack trace.
	bWriteBackScriptWarning = !FBlueprintCoreDelegates::OnScriptException.IsBound();
	ScriptExceptionHandle = FBlueprintCoreDelegates::OnScriptException.AddRaw(this, &FFlockLogSink::HandleScriptException);
}

void FFlockLogSink::Stop()
{
	if (!bRunning)
	{
		return;
	}
	bRunning = false;

	// Must come off GLog before destruction, or the redirector keeps a dangling device.
	if (GLog != nullptr)
	{
		GLog->RemoveOutputDevice(this);
	}

	FCoreDelegates::OnHandleSystemError.Remove(SystemErrorHandle);
	FCoreDelegates::OnShutdownAfterError.Remove(ShutdownAfterErrorHandle);
	// A sink destroyed while still bound here would be called through a dangling pointer on the next script error.
	FBlueprintCoreDelegates::OnScriptException.Remove(ScriptExceptionHandle);
	SystemErrorHandle.Reset();
	ShutdownAfterErrorHandle.Reset();
	ScriptExceptionHandle.Reset();
}

void FFlockLogSink::AddExcludedCategory(FName Category)
{
	ExcludedCategories.Add(Category);
}

void FFlockLogSink::Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category)
{
	// Warnings are noise at this level; only genuine faults become exceptions.
	if (Verbosity != ELogVerbosity::Error && Verbosity != ELogVerbosity::Fatal)
	{
		return;
	}
	if (Message == nullptr || IsExcluded(Category))
	{
		return;
	}

	const FScopedReentrancyGuard Guard;
	if (!Guard.bEntered)
	{
		return;
	}

	const bool bFatal = Verbosity == ELogVerbosity::Fatal;

	// The cap is checked BEFORE the stack walk, not after building the entry: walking is the
	// expensive part, and an error storm must not pay for entries that are going to be dropped.
	// Fatals never queue, so the cap does not apply to them.
	if (!bFatal && QueuedCount.GetValue() >= MaxQueued)
	{
		DroppedCount.Increment();
		return;
	}

	FFlockCapturedLog Captured;
	Captured.Message = Message;
	Captured.Category = Category;
	Captured.bFatal = bFatal;
	Captured.TimestampUtc = FDateTime::UtcNow();
	Captured.Source = TEXT("log");
	Captured.StackTrace = FFlockStackTrace::Capture(StackFramesToSkip);

	if (bFatal)
	{
		// A Fatal line is this process's crash, and the crash delegates that follow it have nothing to add.
		if (!ClaimTheCrashReport())
		{
			return;
		}
		// No tick will follow a fatal; hand it over now and let the handler spool to disk.
		OnFatal.Broadcast(Captured);
		return;
	}

	QueuedCount.Increment();
	Queue.Enqueue(MoveTemp(Captured));
}

bool FFlockLogSink::Peek(FFlockCapturedLog& OutCaptured) const
{
	return Queue.Peek(OutCaptured);
}

bool FFlockLogSink::Dequeue(FFlockCapturedLog& OutCaptured)
{
	if (!Queue.Dequeue(OutCaptured))
	{
		return false;
	}
	QueuedCount.Decrement();
	return true;
}

void FFlockLogSink::HandleSystemError(const TCHAR* EngineErrorRecord)
{
	// A hard crash may never reach the log at all, so synthesize the entry.
	const FScopedReentrancyGuard Guard;
	if (!Guard.bEntered)
	{
		return;
	}
	// One crash, one report. Reported every time the engine raised a delegate, one assertion reached the dashboard as
	// three identical "Unhandled system error" entries next to the assertion itself (measured 2026-09-15).
	if (!ClaimTheCrashReport())
	{
		return;
	}

	FFlockCapturedLog Captured;
	Captured.Message = DescribeCrash(EngineErrorRecord);
	Captured.Category = FName(TEXT("SystemError"));
	Captured.bFatal = true;
	Captured.TimestampUtc = FDateTime::UtcNow();
	Captured.Source = TEXT("crash");
	// The crash delegates carry no message of their own, so the stack is the only useful evidence.
	Captured.StackTrace = FFlockStackTrace::Capture(StackFramesToSkip);
	OnFatal.Broadcast(Captured);
}

FString FFlockLogSink::DescribeCrash(const TCHAR* EngineErrorRecord)
{
	if (EngineErrorRecord != nullptr)
	{
		// The first line names the fault; the lines after it are the engine's own copy of the callstack, which the
		// report already carries in the format every other report uses.
		TArray<FString> Lines;
		FString(EngineErrorRecord).ParseIntoArrayLines(Lines, /*CullEmpty*/ true);
		for (FString& Line : Lines)
		{
			Line.TrimStartAndEndInline();
			if (!Line.IsEmpty())
			{
				return Line;
			}
		}
	}
	return TEXT("Unhandled system error");
}

bool FFlockLogSink::IsReportableScriptException(EBlueprintExceptionType::Type Type)
{
	return !ScriptExceptionTypeToWire(Type).IsEmpty();
}

FString FFlockLogSink::ScriptExceptionTypeToWire(EBlueprintExceptionType::Type Type)
{
	switch (Type)
	{
	case EBlueprintExceptionType::AccessViolation:
		return TEXT("access_violation");
	case EBlueprintExceptionType::InfiniteLoop:
		return TEXT("infinite_loop");
	case EBlueprintExceptionType::NonFatalError:
		return TEXT("non_fatal_error");
	// Despite the name the engine does not crash on this one — it carries on after broadcasting — so nothing
	// else would ever report it.
	case EBlueprintExceptionType::FatalError:
		return TEXT("fatal_error");
	case EBlueprintExceptionType::AbortExecution:
		return TEXT("abort_execution");
	default:
		// Breakpoint, Tracepoint, WireTracepoint, and any type a later engine adds: not reported until someone
		// decides what it means, rather than guessed at.
		return FString();
	}
}

void FFlockLogSink::HandleScriptException(const UObject* ActiveObject, const FFrame& StackFrame,
	const FBlueprintExceptionInfo& Info)
{
	// The Blueprint call stack is what locates the node; a native stack here would only show the script VM.
	CaptureScriptException(Info.GetType(), Info.GetDescription().ToString(), StackFrame.GetStackTrace());
}

void FFlockLogSink::CaptureScriptException(EBlueprintExceptionType::Type Type, const FString& Description,
	const FString& ScriptStack)
{
	const FString TypeWire = ScriptExceptionTypeToWire(Type);
	if (TypeWire.IsEmpty())
	{
		return;
	}

	const FScopedReentrancyGuard Guard;
	if (!Guard.bEntered)
	{
		return;
	}

	// Written back before any filtering: it is the engine's own diagnostic, owed to the developer whether or not
	// this fault is reported anywhere. A Warning, so it never comes back through Serialize as a capture.
	if (bWriteBackScriptWarning && !ScriptStack.IsEmpty())
	{
		++WrittenBackScriptWarnings;
		UE_LOG(LogScript, Warning, TEXT("%s"), *ScriptStack);
	}

	if (IsExcluded(ScriptCategory))
	{
		return;
	}
	if (QueuedCount.GetValue() >= MaxQueued)
	{
		DroppedCount.Increment();
		return;
	}

	FFlockCapturedLog Captured;
	Captured.Message = Description.IsEmpty() ? TEXT("Blueprint script exception") : Description;
	Captured.Category = ScriptCategory;
	Captured.TimestampUtc = FDateTime::UtcNow();
	Captured.Source = TEXT("blueprint");
	Captured.ScriptExceptionType = TypeWire;
	Captured.StackTrace = ScriptStack;

	QueuedCount.Increment();
	Queue.Enqueue(MoveTemp(Captured));
}
