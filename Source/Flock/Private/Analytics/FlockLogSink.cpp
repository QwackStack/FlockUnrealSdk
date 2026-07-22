// Copyright 2022, Qwacks. All Rights Reserved.

#include "Analytics/FlockLogSink.h"

#include "Analytics/FlockStackTrace.h"
#include "Misc/CoreDelegates.h"
#include "Misc/OutputDeviceRedirector.h"

namespace
{
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

	SystemErrorHandle = FCoreDelegates::OnHandleSystemError.AddLambda([this]() { HandleSystemError(); });
	ShutdownAfterErrorHandle = FCoreDelegates::OnShutdownAfterError.AddLambda([this]() { HandleSystemError(); });
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
	SystemErrorHandle.Reset();
	ShutdownAfterErrorHandle.Reset();
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
	Captured.StackTrace = FFlockStackTrace::Capture(StackFramesToSkip);

	if (bFatal)
	{
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

void FFlockLogSink::HandleSystemError()
{
	// A hard crash may never reach the log at all, so synthesize the entry.
	const FScopedReentrancyGuard Guard;
	if (!Guard.bEntered)
	{
		return;
	}

	FFlockCapturedLog Captured;
	Captured.Message = TEXT("Unhandled system error");
	Captured.Category = FName(TEXT("SystemError"));
	Captured.bFatal = true;
	Captured.TimestampUtc = FDateTime::UtcNow();
	// The crash delegates carry no message of their own, so the stack is the only useful evidence.
	Captured.StackTrace = FFlockStackTrace::Capture(StackFramesToSkip);
	OnFatal.Broadcast(Captured);
}
