// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Assets/FlockAssetCache.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

namespace
{
	const TCHAR* const CacheExtension = TEXT(".cache");
	const TCHAR* const TempExtension = TEXT(".tmp");
	const TCHAR* const DefaultFolder = TEXT("Flock/assets");
}

FFlockAssetCache::FFlockAssetCache(const FString& InDirectory, int32 InMaxSizeMB, const TSharedRef<IFlockLogger>& InLogger)
	: Directory(InDirectory.IsEmpty()
		? FPaths::Combine(FPaths::ProjectPersistentDownloadDir(), DefaultFolder)
		: InDirectory)
	, MaxSizeBytes(InMaxSizeMB > 0 ? static_cast<int64>(InMaxSizeMB) * 1024 * 1024 : 0)
	, Logger(InLogger)
{
	SweepTempFiles();
}

FString FFlockAssetCache::GetFinalPath(const FString& AssetId, const FString& VersionToken) const
{
	return FPaths::Combine(Directory,
		FString::Printf(TEXT("%s_%s%s"), *Sanitize(AssetId), *Sanitize(VersionToken), CacheExtension));
}

bool FFlockAssetCache::TryGetCachedPath(const FString& AssetId, const FString& VersionToken, FString& OutPath)
{
	const FString Path = GetFinalPath(AssetId, VersionToken);
	if (!IFileManager::Get().FileExists(*Path))
	{
		OutPath.Reset();
		return false;
	}

	// Stamp it so the LRU sweep treats a read as a use, not just a write.
	IFileManager::Get().SetTimeStamp(*Path, FDateTime::UtcNow());
	OutPath = Path;
	return true;
}

bool FFlockAssetCache::Contains(const FString& AssetId, const FString& VersionToken) const
{
	return IFileManager::Get().FileExists(*GetFinalPath(AssetId, VersionToken));
}

FString FFlockAssetCache::BeginWrite(const FString& AssetId, const FString& VersionToken)
{
	IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
	return GetFinalPath(AssetId, VersionToken) + TempExtension;
}

FString FFlockAssetCache::Commit(const FString& AssetId, const FString& VersionToken, const FString& TempPath)
{
	const FString FinalPath = GetFinalPath(AssetId, VersionToken);

	// Move first, prune second: losing the old copy before the new one is in place would turn a failed
	// move into "no cached asset at all" instead of "still the previous version".
	if (!IFileManager::Get().Move(*FinalPath, *TempPath, /*bReplace*/ true))
	{
		Logger->LogWarning(FString::Printf(TEXT("Asset cache: couldn't commit '%s' into place"), *AssetId));
		return FString();
	}

	DeleteOtherVersions(AssetId, FinalPath);
	EnforceMaxSize();
	return FinalPath;
}

void FFlockAssetCache::Abandon(const FString& TempPath) const
{
	if (!TempPath.IsEmpty())
	{
		IFileManager::Get().Delete(*TempPath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
	}
}

void FFlockAssetCache::Clear()
{
	IFileManager::Get().DeleteDirectory(*Directory, /*RequireExists*/ false, /*Tree*/ true);
}

int64 FFlockAssetCache::GetTotalSizeBytes() const
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, FString::Printf(TEXT("*%s"), CacheExtension)), true, false);

	int64 Total = 0;
	for (const FString& File : Files)
	{
		const int64 Size = IFileManager::Get().FileSize(*FPaths::Combine(Directory, File));
		if (Size > 0)
		{
			Total += Size;
		}
	}
	return Total;
}

void FFlockAssetCache::DeleteOtherVersions(const FString& AssetId, const FString& KeepPath) const
{
	TArray<FString> Files;
	const FString Pattern = FString::Printf(TEXT("%s_*%s"), *Sanitize(AssetId), CacheExtension);
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, Pattern), true, false);

	for (const FString& File : Files)
	{
		const FString FullPath = FPaths::Combine(Directory, File);
		if (FullPath != KeepPath)
		{
			IFileManager::Get().Delete(*FullPath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
		}
	}
}

void FFlockAssetCache::EnforceMaxSize()
{
	if (MaxSizeBytes <= 0)
	{
		return;
	}

	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, FString::Printf(TEXT("*%s"), CacheExtension)), true, false);

	struct FEntry
	{
		FString Path;
		int64 Size = 0;
		FDateTime Accessed;
	};

	TArray<FEntry> Entries;
	Entries.Reserve(Files.Num());
	int64 Total = 0;
	for (const FString& File : Files)
	{
		FEntry Entry;
		Entry.Path = FPaths::Combine(Directory, File);
		Entry.Size = FMath::Max<int64>(IFileManager::Get().FileSize(*Entry.Path), 0);
		Entry.Accessed = IFileManager::Get().GetTimeStamp(*Entry.Path);
		Total += Entry.Size;
		Entries.Add(MoveTemp(Entry));
	}

	if (Total <= MaxSizeBytes)
	{
		return;
	}

	// Oldest access first — TryGetCachedPath restamps on every hit, so this is LRU and not merely FIFO.
	Entries.Sort([](const FEntry& A, const FEntry& B) { return A.Accessed < B.Accessed; });

	for (const FEntry& Entry : Entries)
	{
		if (Total <= MaxSizeBytes)
		{
			break;
		}
		if (IFileManager::Get().Delete(*Entry.Path, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true))
		{
			Total -= Entry.Size;
			Logger->LogDebug(FString::Printf(TEXT("Asset cache: evicted '%s' to stay under budget"),
				*FPaths::GetCleanFilename(Entry.Path)));
		}
	}
}

void FFlockAssetCache::SweepTempFiles() const
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, FString::Printf(TEXT("*%s"), TempExtension)), true, false);

	for (const FString& File : Files)
	{
		IFileManager::Get().Delete(*FPaths::Combine(Directory, File), /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
	}
}

FString FFlockAssetCache::Sanitize(const FString& In)
{
	if (In.IsEmpty())
	{
		return TEXT("_");
	}

	FString Out;
	Out.Reserve(In.Len());
	for (const TCHAR Char : In)
	{
		const bool bSafe = (Char >= TEXT('a') && Char <= TEXT('z'))
			|| (Char >= TEXT('A') && Char <= TEXT('Z'))
			|| (Char >= TEXT('0') && Char <= TEXT('9'))
			|| Char == TEXT('-') || Char == TEXT('_');
		Out.AppendChar(bSafe ? Char : TEXT('_'));
	}
	return Out;
}
