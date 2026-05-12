#include "FlockEventSpool.h"

#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"

namespace
{
	constexpr const TCHAR* EvtExt = TEXT(".evt");
	constexpr const TCHAR* TmpExt = TEXT(".evt.tmp");
}

FFlockEventSpool::FFlockEventSpool(const FString& InDirectory, int32 InMaxEvents, int32 InBatchSize)
	: Directory(InDirectory)
	, MaxEvents(InMaxEvents)
	, Batch(FMath::Max(1, InBatchSize))
{
	IFileManager::Get().MakeDirectory(*Directory, /*Tree=*/true);
	PendingCounter.Set(SweepStaleAndCount());
}

FString FFlockEventSpool::Enqueue(const FString& WireJson)
{
	if (WireJson.IsEmpty())
	{
		return FString();
	}

	// Zero-padded ticks + GUID -> lexical order matches chronological order.
	const FString BaseName = FString::Printf(TEXT("%020lld_%s"),
		FDateTime::UtcNow().GetTicks(),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString Handle = BaseName + EvtExt;
	const FString TmpAbs = Directory / (BaseName + TmpExt);
	const FString DstAbs = Directory / Handle;

	FScopeLock Lock(&Mutex);

	// UE's default encoding writes UTF-16 with a BOM, which a JSON parser will choke on.
	if (!FFileHelper::SaveStringToFile(WireJson, *TmpAbs,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return FString();
	}

	// MoveFileEx with MOVEFILE_REPLACE_EXISTING on Windows; rename(2) on POSIX. Same volume = atomic.
	if (!IFileManager::Get().Move(*DstAbs, *TmpAbs, /*bReplace=*/true))
	{
		IFileManager::Get().Delete(*TmpAbs, /*bRequireExists=*/false, /*bEvenReadOnly=*/false, /*bQuiet=*/true);
		return FString();
	}

	PendingCounter.Increment();

	if (MaxEvents > 0 && PendingCounter.GetValue() > MaxEvents)
	{
		TrimOldest();
	}
	return Handle;
}

void FFlockEventSpool::Remove(const FString& Handle)
{
	if (Handle.IsEmpty()) return;

	FScopeLock Lock(&Mutex);
	const FString Abs = Directory / Handle;
	if (IFileManager::Get().Delete(*Abs, /*bRequireExists=*/false, /*bEvenReadOnly=*/false, /*bQuiet=*/true))
	{
		PendingCounter.Decrement();
	}
}

void FFlockEventSpool::RemoveMany(const TArray<FString>& Handles)
{
	if (Handles.Num() == 0) return;

	FScopeLock Lock(&Mutex);
	for (const FString& H : Handles)
	{
		if (H.IsEmpty()) continue;
		const FString Abs = Directory / H;
		if (IFileManager::Get().Delete(*Abs, false, false, true))
		{
			PendingCounter.Decrement();
		}
	}
}

void FFlockEventSpool::ReadBatch(TArray<FString>& OutHandles, TArray<FString>& OutBodies)
{
	OutHandles.Reset();
	OutBodies.Reset();

	FScopeLock Lock(&Mutex);

	TArray<FString> Files = EnumerateEntryFiles();
	Files.Sort([](const FString& A, const FString& B) { return A < B; });

	const int32 Take = FMath::Min(Files.Num(), Batch);
	OutHandles.Reserve(Take);
	OutBodies.Reserve(Take);

	for (int32 i = 0; i < Take; ++i)
	{
		const FString Abs = Directory / Files[i];
		FString Body;
		if (FFileHelper::LoadFileToString(Body, *Abs))
		{
			OutHandles.Add(Files[i]);
			OutBodies.Add(MoveTemp(Body));
		}
		else
		{
			// Unreadable: drop so the spool can make progress instead of getting stuck.
			if (IFileManager::Get().Delete(*Abs, false, false, true))
			{
				PendingCounter.Decrement();
			}
		}
	}
}

bool FFlockEventSpool::TryBeginFlush()
{
	int32 Expected = 0;
	return Flushing.compare_exchange_strong(Expected, 1);
}

void FFlockEventSpool::EndFlush()
{
	Flushing.store(0);
}

void FFlockEventSpool::Clear()
{
	FScopeLock Lock(&Mutex);
	for (const FString& F : EnumerateEntryFiles())
	{
		IFileManager::Get().Delete(*(Directory / F), false, false, true);
	}
	PendingCounter.Set(0);
}

void FFlockEventSpool::TrimOldest()
{
	TArray<FString> Files = EnumerateEntryFiles();
	if (MaxEvents <= 0 || Files.Num() <= MaxEvents) return;

	Files.Sort([](const FString& A, const FString& B) { return A < B; });
	const int32 Overflow = Files.Num() - MaxEvents;
	for (int32 i = 0; i < Overflow; ++i)
	{
		if (IFileManager::Get().Delete(*(Directory / Files[i]), false, false, true))
		{
			PendingCounter.Decrement();
		}
	}
}

int32 FFlockEventSpool::SweepStaleAndCount()
{
	int32 Count = 0;
	TArray<FString> All;
	IFileManager::Get().FindFiles(All, *(Directory / TEXT("*")), /*Files=*/true, /*Dirs=*/false);
	for (const FString& Name : All)
	{
		// Order matters: ".evt.tmp" also ends with ".tmp" not ".evt", so the EvtExt check is safe.
		if (Name.EndsWith(TmpExt))
		{
			IFileManager::Get().Delete(*(Directory / Name), false, false, true);
		}
		else if (Name.EndsWith(EvtExt))
		{
			++Count;
		}
	}
	return Count;
}

TArray<FString> FFlockEventSpool::EnumerateEntryFiles() const
{
	TArray<FString> All;
	IFileManager::Get().FindFiles(All, *(Directory / TEXT("*")), true, false);

	TArray<FString> Result;
	Result.Reserve(All.Num());
	for (FString& Name : All)
	{
		if (Name.EndsWith(EvtExt))
		{
			Result.Add(MoveTemp(Name));
		}
	}
	return Result;
}
