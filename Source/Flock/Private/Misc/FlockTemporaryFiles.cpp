// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/FlockTemporaryFiles.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"

#if PLATFORM_UNIX || PLATFORM_APPLE || PLATFORM_ANDROID
#include <errno.h>
#include <unistd.h>
#define FLOCK_MOVES_IN_BY_LINKING 1
#else
#define FLOCK_MOVES_IN_BY_LINKING 0
#endif

namespace
{
	/** Read and written on the game thread only. */
	TFunction<void(const FString&)>& BeforeNextTemporaryFileMove()
	{
		static TFunction<void(const FString&)> Hook;
		return Hook;
	}

	/** Moves a written temporary file into place, and deletes it when the write or the move failed. */
	bool FinishTemporaryFileSave(bool bWritten, const FString& TemporaryPath, const FString& DestinationPath)
	{
		if (bWritten && FFlockTemporaryFiles::MoveIntoPlace(TemporaryPath, DestinationPath))
		{
			return true;
		}
		IFileManager::Get().Delete(*TemporaryPath, /*bRequireExists*/ false, /*bEvenReadOnly*/ true, /*bQuiet*/ true);
		return false;
	}
}

FString FFlockTemporaryFiles::MakePath(const FString& DestinationPath)
{
	return FString::Printf(TEXT("%s.%s.tmp"), *DestinationPath, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

bool FFlockTemporaryFiles::MoveIntoPlace(const FString& TemporaryPath, const FString& DestinationPath)
{
	if (IsInGameThread() && BeforeNextTemporaryFileMove())
	{
		// Cleared before it runs, so a move the hook itself makes goes straight through.
		const TFunction<void(const FString&)> Hook = BeforeNextTemporaryFileMove();
		BeforeNextTemporaryFileMove() = nullptr;
		Hook(TemporaryPath);
	}
	return IFileManager::Get().Move(*DestinationPath, *TemporaryPath, /*bReplace*/ true, /*bEvenIfReadOnly*/ true,
		/*bAttributes*/ false, /*bDoNotRetryOrError*/ true);
}

bool FFlockTemporaryFiles::SaveThenMove(const FString& Text, const FString& DestinationPath, FFileHelper::EEncodingOptions Encoding)
{
	const FString TemporaryPath = MakePath(DestinationPath);
	return FinishTemporaryFileSave(FFileHelper::SaveStringToFile(Text, *TemporaryPath, Encoding), TemporaryPath, DestinationPath);
}

bool FFlockTemporaryFiles::SaveThenMove(const TArray<uint8>& Bytes, const FString& DestinationPath)
{
	const FString TemporaryPath = MakePath(DestinationPath);
	return FinishTemporaryFileSave(FFileHelper::SaveArrayToFile(Bytes, *TemporaryPath), TemporaryPath, DestinationPath);
}

bool FFlockTemporaryFiles::MoveIntoPlaceIfNone(const FString& TemporaryPath, const FString& DestinationPath)
{
	IFileManager& Files = IFileManager::Get();
#if PLATFORM_WINDOWS
	// Without replacing, the engine's move is MoveFileW, which refuses a destination that is there in the same step.
	return Files.Move(*DestinationPath, *TemporaryPath, /*bReplace*/ false, /*bEvenIfReadOnly*/ false, /*bAttributes*/ false,
		/*bDoNotRetryOrError*/ true);
#else
#if FLOCK_MOVES_IN_BY_LINKING
	// rename() replaces a file that is there, so the file is linked in under its new name, which link() refuses when one is
	// there, and its temporary name is dropped.
	Files.MakeDirectory(*FPaths::GetPath(DestinationPath), /*Tree*/ true);
	const FTCHARToUTF8 From(*Files.ConvertToAbsolutePathForExternalAppForWrite(*TemporaryPath));
	const FTCHARToUTF8 To(*Files.ConvertToAbsolutePathForExternalAppForWrite(*DestinationPath));
	const int32 Linked = link(From.Get(), To.Get());
	const int32 LinkError = errno;
	if (Linked == 0)
	{
		Files.Delete(*TemporaryPath, /*bRequireExists*/ false, /*bEvenReadOnly*/ true, /*bQuiet*/ true);
		return true;
	}
	if (LinkError == EEXIST)
	{
		return false;
	}
#endif
	// No hard links here: checked, then moved.
	return !Files.FileExists(*DestinationPath) && Files.Move(*DestinationPath, *TemporaryPath, /*bReplace*/ false,
		/*bEvenIfReadOnly*/ false, /*bAttributes*/ false, /*bDoNotRetryOrError*/ true);
#endif
}

bool FFlockTemporaryFiles::SaveAsNew(const FString& Text, const FString& DestinationPath, FFileHelper::EEncodingOptions Encoding)
{
	const FString TemporaryPath = MakePath(DestinationPath);
	if (FFileHelper::SaveStringToFile(Text, *TemporaryPath, Encoding) && MoveIntoPlaceIfNone(TemporaryPath, DestinationPath))
	{
		return true;
	}
	IFileManager::Get().Delete(*TemporaryPath, /*bRequireExists*/ false, /*bEvenReadOnly*/ true, /*bQuiet*/ true);
	return false;
}

int32 FFlockTemporaryFiles::DeleteLeftOverFilesOf(const FString& DestinationPath)
{
	return DeleteLeftOverFiles(FPaths::GetPath(DestinationPath), FPaths::GetCleanFilename(DestinationPath) + TEXT(".*.tmp"),
		/*bIncludeSubfolders*/ false);
}

int32 FFlockTemporaryFiles::DeleteTemporaryFilesOf(const FString& DestinationPath)
{
	int32 Deleted = 0;
	for (const FString& Path : FindTemporaryFilesOf(DestinationPath))
	{
		if (IFileManager::Get().Delete(*Path, /*bRequireExists*/ false, /*bEvenReadOnly*/ true, /*bQuiet*/ true))
		{
			++Deleted;
		}
	}
	return Deleted;
}

TArray<FString> FFlockTemporaryFiles::FindTemporaryFilesOf(const FString& DestinationPath)
{
	const FString Folder = FPaths::GetPath(DestinationPath);
	TArray<FString> Names;
	IFileManager::Get().FindFiles(Names, *FPaths::Combine(Folder, FPaths::GetCleanFilename(DestinationPath) + TEXT(".*.tmp")),
		/*bFiles*/ true, /*bDirectories*/ false);

	TArray<TPair<FDateTime, FString>> Dated;
	for (const FString& Name : Names)
	{
		const FString Path = FPaths::Combine(Folder, Name);
		Dated.Emplace(IFileManager::Get().GetTimeStamp(*Path), Path);
	}
	Dated.Sort([](const TPair<FDateTime, FString>& A, const TPair<FDateTime, FString>& B) { return A.Key > B.Key; });

	TArray<FString> Paths;
	for (const TPair<FDateTime, FString>& Entry : Dated)
	{
		Paths.Add(Entry.Value);
	}
	return Paths;
}

int32 FFlockTemporaryFiles::DeleteLeftOverFiles(const FString& Folder, const FString& NamePattern, bool bIncludeSubfolders)
{
	IFileManager& FileManager = IFileManager::Get();
	TArray<FString> Found;
	if (bIncludeSubfolders)
	{
		FileManager.FindFilesRecursive(Found, *Folder, *NamePattern, /*bFiles*/ true, /*bDirectories*/ false);
	}
	else
	{
		FileManager.FindFiles(Found, *FPaths::Combine(Folder, NamePattern), /*bFiles*/ true, /*bDirectories*/ false);
		for (FString& Name : Found)
		{
			Name = FPaths::Combine(Folder, Name);
		}
	}

	const FDateTime OldestKept = FDateTime::UtcNow() - FTimespan::FromMinutes(LeftOverAfterMinutes);
	int32 Deleted = 0;
	for (const FString& Path : Found)
	{
		if (FileManager.GetTimeStamp(*Path) < OldestKept
			&& FileManager.Delete(*Path, /*bRequireExists*/ false, /*bEvenReadOnly*/ true, /*bQuiet*/ true))
		{
			++Deleted;
		}
	}
	return Deleted;
}

void FFlockTemporaryFiles::SetBeforeNextMoveForTesting(TFunction<void(const FString& TemporaryPath)> Hook)
{
	check(IsInGameThread());
	BeforeNextTemporaryFileMove() = MoveTemp(Hook);
}
