// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Analytics/FlockAnalyticsLaunches.h"

#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FlockLaunchFolder.h"
#include "Misc/Paths.h"

namespace
{
	TFunction<bool(const FString&)> EntryMoveFailsForTesting;

	const TCHAR* const AnalyticsLaunchQueueNames[] = {
		FFlockAnalyticsLaunches::LogEventsQueueName,
		FFlockAnalyticsLaunches::SessionEndsQueueName,
		FFlockAnalyticsLaunches::AnalyticsEventsQueueName,
	};

	/** Whether a build before 1.16.0 left a crash marker, a live-session record or a queued entry straight in AnalyticsFolder. */
	bool HasEarlierBuildAnalyticsFiles(const FString& AnalyticsFolder)
	{
		IFileManager& FileManager = IFileManager::Get();
		if (FileManager.FileExists(*FPaths::Combine(AnalyticsFolder, FFlockAnalyticsLaunches::SessionStateFileName))
			|| FileManager.FileExists(*FPaths::Combine(AnalyticsFolder, FFlockAnalyticsLaunches::TerminationMarkerFileName)))
		{
			return true;
		}
		for (const TCHAR* Queue : AnalyticsLaunchQueueNames)
		{
			TArray<FString> Entries;
			FileManager.FindFiles(Entries, *FPaths::Combine(AnalyticsFolder, Queue, TEXT("*.json")), /*Files*/ true, /*Directories*/ false);
			if (Entries.Num() > 0)
			{
				return true;
			}
		}
		return false;
	}
}

FFlockAnalyticsLaunches::FFlockAnalyticsLaunches(const FString& InAnalyticsFolder)
	: AnalyticsFolder(FPaths::ConvertRelativePathToFull(InAnalyticsFolder))
{
}

FFlockAnalyticsLaunches::~FFlockAnalyticsLaunches() = default;

FString FFlockAnalyticsLaunches::DefaultFolder()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Flock"), TEXT("analytics")));
}

FString FFlockAnalyticsLaunches::LaunchesFolderOf(const FString& AnalyticsFolder, const FString& GameKey)
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(AnalyticsFolder, GamesFolderName, GameKey, LaunchesFolderName));
}

FString FFlockAnalyticsLaunches::EveryGamesLaunchesFolder(const FString& AnalyticsFolder)
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(AnalyticsFolder, LaunchesFolderName));
}

TArray<FString> FFlockAnalyticsLaunches::FindLaunchFoldersOfEveryGame(const FString& AnalyticsFolder)
{
	TArray<FString> Folders = FFlockLaunchFolder::FindFolders(EveryGamesLaunchesFolder(AnalyticsFolder));
	TArray<FString> Games;
	IFileManager::Get().FindFiles(Games, *FPaths::Combine(FPaths::ConvertRelativePathToFull(AnalyticsFolder), GamesFolderName, TEXT("*")),
		/*Files*/ false, /*Directories*/ true);
	for (const FString& Game : Games)
	{
		Folders.Append(FFlockLaunchFolder::FindFolders(LaunchesFolderOf(AnalyticsFolder, Game)));
	}
	Folders.Sort();
	return Folders;
}

TSharedRef<FFlockAnalyticsLaunches> FFlockAnalyticsLaunches::Start(const FString& AnalyticsFolder, const FString& GameKey)
{
	const TSharedRef<FFlockAnalyticsLaunches> Launches = MakeShareable(new FFlockAnalyticsLaunches(AnalyticsFolder));
	const FString LaunchesFolder = LaunchesFolderOf(Launches->AnalyticsFolder, GameKey);

	Launches->ThisLaunch = FFlockLaunchFolder::Create(LaunchesFolder);
	if (!Launches->ThisLaunch.IsValid())
	{
		// A launch that cannot show it is running takes over nothing, so it cannot take a running launch's files either. It keeps
		// none of its own: a folder with no lock is one no later launch would ever take over or delete.
		return Launches;
	}
	Launches->Folder = Launches->ThisLaunch->GetPath();

	Launches->TakeOverEarlierBuildFiles();
	// Those an earlier build kept for every game, then the game's own, so the oldest come first: nearly every project only ever had
	// one game, and a launch still running under an earlier build holds its lock, so its files stay its own.
	Launches->TakeOverEndedLaunches(EveryGamesLaunchesFolder(Launches->AnalyticsFolder));
	Launches->TakeOverEndedLaunches(LaunchesFolder);
	return Launches;
}

void FFlockAnalyticsLaunches::TakeOverEndedLaunches(const FString& LaunchesFolder)
{
	// This launch's own folder is among its game's; its own lock refuses the claim.
	for (const FString& LaunchFolder : FFlockLaunchFolder::FindFolders(LaunchesFolder))
	{
		const TSharedPtr<FFlockLaunchFolder> Ended = FFlockLaunchFolder::ClaimEnded(LaunchFolder);
		if (!Ended.IsValid())
		{
			continue;
		}
		const bool bEveryEntryMoved = TakeOverQueues(LaunchFolder);
		EndedLaunches.Add(Ended);
		EndedRecords.Add({ FPaths::Combine(LaunchFolder, SessionStateFileName), FPaths::Combine(LaunchFolder, TerminationMarkerFileName),
			!bEveryEntryMoved });
	}
}

bool FFlockAnalyticsLaunches::IsHoldingItsFolder() const
{
	return ThisLaunch.IsValid();
}

void FFlockAnalyticsLaunches::StopHoldingItsFolderForTesting()
{
	ThisLaunch.Reset();
}

void FFlockAnalyticsLaunches::SetEntryMoveFailsForTesting(TFunction<bool(const FString& EntryFileName)> InEntryMoveFails)
{
	EntryMoveFailsForTesting = MoveTemp(InEntryMoveFails);
}

FString FFlockAnalyticsLaunches::GetFolder() const
{
	return Folder;
}

FString FFlockAnalyticsLaunches::GetSessionStatePath() const
{
	return GetFolder().IsEmpty() ? FString() : FPaths::Combine(GetFolder(), SessionStateFileName);
}

FString FFlockAnalyticsLaunches::GetTerminationMarkerPath() const
{
	return GetFolder().IsEmpty() ? FString() : FPaths::Combine(GetFolder(), TerminationMarkerFileName);
}

void FFlockAnalyticsLaunches::TakeOverEarlierBuildFiles()
{
	if (!HasEarlierBuildAnalyticsFiles(AnalyticsFolder))
	{
		return;
	}
	// Earlier builds kept one set of files for every launch, with nothing to tell whether the game writing them still runs.
	// They are taken over once, by whichever launch opens this lock first.
	EarlierBuildFilesLock.Reset(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(
		*FPaths::Combine(AnalyticsFolder, EarlierBuildFilesLockName), /*bAppend*/ false, /*bAllowRead*/ false));
	if (!EarlierBuildFilesLock.IsValid())
	{
		return;
	}
	const bool bEveryEntryMoved = TakeOverQueues(AnalyticsFolder);
	EarlierBuildFilesIndex = EndedRecords.Num();
	EndedLaunches.Add(nullptr);
	EndedRecords.Add({ FPaths::Combine(AnalyticsFolder, SessionStateFileName), FPaths::Combine(AnalyticsFolder, TerminationMarkerFileName),
		!bEveryEntryMoved });
}

bool FFlockAnalyticsLaunches::TakeOverQueues(const FString& FromFolder)
{
	bool bEveryEntryMoved = true;
	IFileManager& FileManager = IFileManager::Get();
	const FString ThisFolder = GetFolder();
	for (const TCHAR* Queue : AnalyticsLaunchQueueNames)
	{
		const FString From = FPaths::Combine(FromFolder, Queue);
		TArray<FString> Entries;
		FileManager.FindFiles(Entries, *FPaths::Combine(From, TEXT("*.json")), /*Files*/ true, /*Directories*/ false);
		if (Entries.Num() == 0)
		{
			continue;
		}
		const FString To = FPaths::Combine(ThisFolder, Queue);
		// The first queue taken over becomes this launch's in one rename, however many entries it holds.
		if (!FileManager.DirectoryExists(*To)
			&& FPlatformFileManager::Get().GetPlatformFile().MoveFile(*To, *From))
		{
			continue;
		}
		FileManager.MakeDirectory(*To, /*Tree*/ true);
		for (const FString& Entry : Entries)
		{
			// Entries are named for the time they were queued, so they keep their age order among this launch's. Two launches can
			// name one alike (the same millisecond and count), and a name already taken gets a suffix that sorts right after it.
			FString Destination = FPaths::Combine(To, Entry);
			if (FileManager.FileExists(*Destination))
			{
				Destination = FPaths::Combine(To, FString::Printf(TEXT("%s_%s.json"), *FPaths::GetBaseFilename(Entry),
					*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower()));
			}
			const bool bMoveFails = EntryMoveFailsForTesting && EntryMoveFailsForTesting(Entry);
			if (bMoveFails || !FileManager.Move(*Destination, *FPaths::Combine(From, Entry), /*Replace*/ false, /*EvenIfReadOnly*/ true,
				/*Attributes*/ false, /*bDoNotRetryOrError*/ true))
			{
				bEveryEntryMoved = false;
			}
		}
	}
	return bEveryEntryMoved;
}

void FFlockAnalyticsLaunches::DeleteEndedLaunch(int32 Index)
{
	if (!EndedRecords.IsValidIndex(Index))
	{
		return;
	}
	if (EndedRecords[Index].bQueuedEntriesLeftBehind)
	{
		// Its records are reported, so they go; the entries that could not be moved stay, with the folder and its lock file, for a
		// later launch to take over. Deleting the folder would lose them.
		IFileManager& FileManager = IFileManager::Get();
		FileManager.Delete(*EndedRecords[Index].SessionStatePath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
		FileManager.Delete(*EndedRecords[Index].TerminationMarkerPath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
		if (EndedLaunches.IsValidIndex(Index))
		{
			EndedLaunches[Index].Reset();
		}
		return;
	}
	if (Index == EarlierBuildFilesIndex)
	{
		IFileManager& FileManager = IFileManager::Get();
		FileManager.Delete(*EndedRecords[Index].SessionStatePath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
		FileManager.Delete(*EndedRecords[Index].TerminationMarkerPath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
		for (const TCHAR* Queue : AnalyticsLaunchQueueNames)
		{
			FileManager.DeleteDirectory(*FPaths::Combine(AnalyticsFolder, Queue), /*RequireExists*/ false, /*Tree*/ true);
		}
		return;
	}
	if (EndedLaunches.IsValidIndex(Index) && EndedLaunches[Index].IsValid())
	{
		EndedLaunches[Index]->DeleteEverything();
		EndedLaunches[Index].Reset();
	}
}

void FFlockAnalyticsLaunches::LetGoOfEndedLaunches()
{
	EndedLaunches.Empty();
	EndedRecords.Empty();
	if (EarlierBuildFilesLock.IsValid())
	{
		EarlierBuildFilesLock.Reset();
		IFileManager::Get().Delete(*FPaths::Combine(AnalyticsFolder, EarlierBuildFilesLockName), /*RequireExists*/ false,
			/*EvenReadOnly*/ true, /*Quiet*/ true);
	}
	EarlierBuildFilesIndex = INDEX_NONE;
}
