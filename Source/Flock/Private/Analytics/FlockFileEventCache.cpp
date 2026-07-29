// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Analytics/FlockFileEventCache.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	const TCHAR* EntryExtension = TEXT(".json");
	const TCHAR* TempExtension = TEXT(".json.tmp");
}

FFlockFileEventCache::FFlockFileEventCache(const FString& Subfolder, int32 InMaxEntries, const FString& InRootDirectory)
	: Directory(FPaths::Combine(InRootDirectory.IsEmpty() ? DefaultRoot() : InRootDirectory, Subfolder))
	, MaxEntries(InMaxEntries)
{
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	PlatformFile.CreateDirectoryTree(*Directory);

	// Temps first: a crash between write and move leaves one behind, and it is not an entry — it is the
	// half of a write that never committed.
	TArray<FString> Temps;
	IFileManager::Get().FindFiles(Temps, *FPaths::Combine(Directory, FString(TEXT("*")) + TempExtension), true, false);
	for (const FString& Temp : Temps)
	{
		PlatformFile.DeleteFile(*FPaths::Combine(Directory, Temp));
	}

	// Rebuild the queue from whatever survived the last run. Sorting the stems restores age order
	// because handles are minted to sort that way.
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, FString(TEXT("*")) + EntryExtension), true, false);
	for (const FString& File : Files)
	{
		// Don't lean on the platform's wildcard semantics to have excluded the temps: GetBaseFilename
		// would strip only the trailing ".tmp" and mint a handle of "<stem>.json", whose PathForHandle
		// then names a file that does not exist.
		if (File.EndsWith(TempExtension, ESearchCase::IgnoreCase))
		{
			continue;
		}
		Handles.Add(FPaths::GetBaseFilename(File));
	}
	Handles.Sort();

	// A cap lowered between runs applies to what is already on disk.
	EvictToCap();
}

FString FFlockFileEventCache::DefaultRoot()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Flock"), TEXT("analytics"));
}

FString FFlockFileEventCache::PathForHandle(const FString& Handle) const
{
	return FPaths::Combine(Directory, Handle + EntryExtension);
}

FString FFlockFileEventCache::TempPathForHandle(const FString& Handle) const
{
	return FPaths::Combine(Directory, Handle + TempExtension);
}

bool FFlockFileEventCache::WriteAtomic(const FString& Handle, const FString& Payload) const
{
	const FString TempPath = TempPathForHandle(Handle);
	if (!FFileHelper::SaveStringToFile(Payload, *TempPath))
	{
		return false;
	}
	if (!IFileManager::Get().Move(*PathForHandle(Handle), *TempPath, /*bReplace*/ true))
	{
		FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*TempPath);
		return false;
	}
	return true;
}

FString FFlockFileEventCache::MakeHandle()
{
	// Millisecond stamp is zero-padded so string order matches time order; the sequence breaks ties
	// inside the same millisecond without needing a GUID's width.
	const FDateTime Now = FDateTime::UtcNow();
	const int64 Millis = Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond();
	return FString::Printf(TEXT("%013lld_%08x"), Millis, ++Sequence);
}

void FFlockFileEventCache::EvictToCap()
{
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	while (Handles.Num() > FMath::Max(MaxEntries, 0))
	{
		const FString Oldest = Handles[0];
		Handles.RemoveAt(0);
		PlatformFile.DeleteFile(*PathForHandle(Oldest));
	}
}

int32 FFlockFileEventCache::PendingCount() const
{
	return Handles.Num();
}

FString FFlockFileEventCache::Enqueue(const FString& Payload)
{
	if (MaxEntries <= 0)
	{
		return FString();
	}

	const FString Handle = MakeHandle();
	if (!WriteAtomic(Handle, Payload))
	{
		// Out of space or unwritable: lose the entry, keep the game running.
		return FString();
	}

	Handles.Add(Handle);
	EvictToCap();

	// The write itself may have been the entry evicted when the cap is 1.
	return Handles.Contains(Handle) ? Handle : FString();
}

bool FFlockFileEventCache::Read(const FString& Handle, FString& OutPayload) const
{
	if (!Handles.Contains(Handle))
	{
		return false;
	}
	return FFileHelper::LoadFileToString(OutPayload, *PathForHandle(Handle));
}

void FFlockFileEventCache::Replace(const FString& Handle, const FString& Payload)
{
	if (!Handles.Contains(Handle))
	{
		return;
	}
	// Atomic for the same reason Enqueue is, and more pressing: the entry being overwritten is one that
	// already survived a failed send, so a torn write here loses a record on its second chance.
	WriteAtomic(Handle, Payload);
}

void FFlockFileEventCache::Remove(const FString& Handle)
{
	const int32 Index = Handles.IndexOfByKey(Handle);
	if (Index == INDEX_NONE)
	{
		return;
	}
	Handles.RemoveAt(Index);
	ReadFailures.Remove(Handle);
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*PathForHandle(Handle));
}

void FFlockFileEventCache::PeekBatch(int32 MaxCount, TArray<FString>& OutHandles, TArray<FString>& OutPayloads)
{
	OutHandles.Reset();
	OutPayloads.Reset();
	if (MaxCount <= 0)
	{
		return;
	}

	const int32 Count = FMath::Min(MaxCount, Handles.Num());
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FString& Handle = Handles[Index];
		FString Payload;
		if (FFileHelper::LoadFileToString(Payload, *PathForHandle(Handle)))
		{
			ReadFailures.Remove(Handle);
			OutHandles.Add(Handle);
			OutPayloads.Add(MoveTemp(Payload));
			continue;
		}

		// Unreadable. Give it another pass or two in case something merely had the file open, then hand
		// it over with an empty payload: no caller can parse that, so it goes down the same
		// "never deliverable" path a corrupt entry does and the slot is reclaimed. Skipping it instead
		// would strand a handle nothing can ever Remove.
		int32& Failures = ReadFailures.FindOrAdd(Handle);
		++Failures;
		if (Failures >= ReadFailuresBeforeDrop)
		{
			Failures = 0;
			OutHandles.Add(Handle);
			OutPayloads.Add(FString());
		}
	}
}

TArray<FString> FFlockFileEventCache::AllHandles() const
{
	return Handles;
}

void FFlockFileEventCache::Clear()
{
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	for (const FString& Handle : Handles)
	{
		PlatformFile.DeleteFile(*PathForHandle(Handle));
	}
	Handles.Reset();
	ReadFailures.Reset();
}
