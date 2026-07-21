// Copyright 2022, Qwacks. All Rights Reserved.

#include "Analytics/FlockFileEventCache.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	const TCHAR* EntryExtension = TEXT(".json");
}

FFlockFileEventCache::FFlockFileEventCache(const FString& Subfolder, int32 InMaxEntries, const FString& InRootDirectory)
	: Directory(FPaths::Combine(InRootDirectory.IsEmpty() ? DefaultRoot() : InRootDirectory, Subfolder))
	, MaxEntries(InMaxEntries)
{
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	PlatformFile.CreateDirectoryTree(*Directory);

	// Rebuild the queue from whatever survived the last run. Sorting the stems restores age order
	// because handles are minted to sort that way.
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, FString(TEXT("*")) + EntryExtension), true, false);
	for (const FString& File : Files)
	{
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
	if (!FFileHelper::SaveStringToFile(Payload, *PathForHandle(Handle)))
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
	FFileHelper::SaveStringToFile(Payload, *PathForHandle(Handle));
}

void FFlockFileEventCache::Remove(const FString& Handle)
{
	const int32 Index = Handles.IndexOfByKey(Handle);
	if (Index == INDEX_NONE)
	{
		return;
	}
	Handles.RemoveAt(Index);
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*PathForHandle(Handle));
}

void FFlockFileEventCache::PeekBatch(int32 MaxCount, TArray<FString>& OutHandles, TArray<FString>& OutPayloads) const
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
		FString Payload;
		if (FFileHelper::LoadFileToString(Payload, *PathForHandle(Handles[Index])))
		{
			OutHandles.Add(Handles[Index]);
			OutPayloads.Add(MoveTemp(Payload));
		}
		// An unreadable entry is skipped rather than aborting the batch; the provider drops it by
		// handle once the rest of the batch settles.
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
}
