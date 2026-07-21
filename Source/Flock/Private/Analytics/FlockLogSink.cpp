// Copyright 2022, Qwacks. All Rights Reserved.

#include "Analytics/FlockLogSink.h"

#include "HAL/PlatformStackWalk.h"
#include "Misc/CoreDelegates.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Paths.h"

namespace
{
	/**
	 * Drops this sink's own frames: CaptureStackTrace, its caller inside the walker, and Serialize.
	 *
	 * It does NOT reach the logging site when capture came through UE_LOG — the engine's
	 * UE_LOG -> GLog -> FOutputDevice dispatch sits above Serialize and contributes roughly five
	 * Core frames first (observed live). That depth is not fixed (redirector, threading, buffering
	 * all change it), so a larger constant would eat real frames on other paths, which is worse than
	 * a few lines of plumbing. Trimming it would need to identify dispatch frames by symbol, which is
	 * fragile across platforms — deliberately not attempted.
	 */
	constexpr uint32 StackFramesToSkip = 3;
	constexpr uint32 MaxStackFrames = 64;

	/** Guards the cached module table: Serialize runs on any thread. */
	FCriticalSection GModuleTableLock;

	/**
	 * Loaded-module base addresses, cached because enumerating them per frame would be absurd.
	 *
	 * This exists because FProgramCounterSymbolInfo::OffsetInModule comes back as 0 on Windows — it
	 * is simply not populated — so the offset has to be computed as (program counter - module base)
	 * from the module table instead. Verified: without this every frame reported `+0x0`.
	 */
	TArray<FStackWalkModuleInfo>& GetModuleTable_Locked(bool bRefresh)
	{
		static TArray<FStackWalkModuleInfo> Modules;
		if (Modules.Num() > 0 && !bRefresh)
		{
			return Modules;
		}

		Modules.Reset();
		const int32 Count = FPlatformStackWalk::GetProcessModuleCount();
		if (Count > 0)
		{
			Modules.SetNumZeroed(Count);
			const int32 Filled = FPlatformStackWalk::GetProcessModuleSignatures(Modules.GetData(), Count);
			Modules.SetNum(FMath::Clamp(Filled, 0, Count));
		}
		return Modules;
	}

	/** Module name + offset from its base, or false when the counter belongs to no known module. */
	bool TryResolveModuleOffset(uint64 ProgramCounter, FString& OutModule, uint64& OutOffset)
	{
		FScopeLock Lock(&GModuleTableLock);

		// A module loaded since the table was built would miss, so a miss refreshes once and retries.
		for (int32 Attempt = 0; Attempt < 2; ++Attempt)
		{
			for (const FStackWalkModuleInfo& Module : GetModuleTable_Locked(/*bRefresh*/ Attempt == 1))
			{
				if (ProgramCounter < Module.BaseOfImage ||
					ProgramCounter >= Module.BaseOfImage + Module.ImageSize)
				{
					continue;
				}
				OutModule = FPaths::GetCleanFilename(FString(Module.ImageName));
				if (OutModule.IsEmpty())
				{
					OutModule = FString(Module.ModuleName);
				}
				OutOffset = ProgramCounter - Module.BaseOfImage;
				return true;
			}
		}
		return false;
	}

	/**
	 * Walks the stack and formats each frame as `Module+0xOffset`, plus the function and source line
	 * when symbols happen to be available.
	 *
	 * The offset is measured from the module's base address, NOT the raw program counter. A program
	 * counter is shifted by ASLR on every run, so a trace built from raw addresses identifies which
	 * module failed but can never be symbolicated afterwards — the same frame reports a different
	 * number each launch. Module + offset is stable across runs and is what a symbol server needs.
	 *
	 * Named frames are a bonus, not the contract: a shipped build has no PDBs on the symbol path, so
	 * FunctionName is usually empty and resolution happens later from the module and offset.
	 *
	 * Symbol lookup is the expensive part of capture, which is why Serialize applies the queue cap
	 * before calling this.
	 */
	FString CaptureStackTrace()
	{
		// One-time symbol-handler init, paid on the first capture so a run with no errors never pays it.
		static bool bStackWalkingInitialized = false;
		if (!bStackWalkingInitialized)
		{
			bStackWalkingInitialized = true;
			FPlatformStackWalk::InitStackWalking();
		}

		uint64 BackTrace[MaxStackFrames] = { 0 };
		const uint32 Depth = FPlatformStackWalk::CaptureStackBackTrace(BackTrace, MaxStackFrames);

		TArray<FString> Frames;
		Frames.Reserve(Depth);
		for (uint32 Index = StackFramesToSkip; Index < Depth; ++Index)
		{
			if (BackTrace[Index] == 0)
			{
				continue;
			}

			FProgramCounterSymbolInfo Info;
			FPlatformStackWalk::ProgramCounterToSymbolInfo(BackTrace[Index], Info);

			FString Module;
			uint64 Offset = 0;
			FString Frame;
			if (TryResolveModuleOffset(BackTrace[Index], Module, Offset))
			{
				Frame = FString::Printf(TEXT("%s+0x%llx"), *Module, Offset);
			}
			else
			{
				// No owning module: keep the raw counter so the frame is not silently lost, and mark
				// it so nobody mistakes it for something a symbol server can resolve.
				Module = FPaths::GetCleanFilename(FString(ANSI_TO_TCHAR(Info.ModuleName)));
				Frame = FString::Printf(TEXT("%s@0x%llx (unresolved)"),
					Module.IsEmpty() ? TEXT("<unknown>") : *Module, BackTrace[Index]);
			}

			const FString Function = FString(ANSI_TO_TCHAR(Info.FunctionName));
			if (!Function.IsEmpty() && !Function.Contains(TEXT("UnknownFunction")))
			{
				Frame += FString::Printf(TEXT(" %s"), *Function);

				const FString File = FPaths::GetCleanFilename(FString(ANSI_TO_TCHAR(Info.Filename)));
				if (Info.LineNumber > 0 && !File.IsEmpty())
				{
					Frame += FString::Printf(TEXT(" [%s:%d]"), *File, Info.LineNumber);
				}
			}
			Frames.Add(MoveTemp(Frame));
		}
		return FString::Join(Frames, TEXT("\n"));
	}
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
	Captured.StackTrace = CaptureStackTrace();

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
	Captured.StackTrace = CaptureStackTrace();
	OnFatal.Broadcast(Captured);
}
