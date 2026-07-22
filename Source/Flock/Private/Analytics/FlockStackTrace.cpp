// Copyright 2022, Qwacks. All Rights Reserved.

#include "Analytics/FlockStackTrace.h"

#include "HAL/PlatformStackWalk.h"
#include "Misc/Paths.h"

namespace
{
	constexpr uint32 MaxStackFrames = 64;

	/** Guards the cached module table: capture runs on any thread. */
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
}

FString FFlockStackTrace::Capture(uint32 FramesToSkip)
{
	// One-time symbol-handler init, paid on the first capture so a run with no faults never pays it.
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
	for (uint32 Index = FramesToSkip; Index < Depth; ++Index)
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
			// No owning module: keep the raw counter so the frame is not silently lost, and mark it
			// so nobody mistakes it for something a symbol server can resolve.
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
