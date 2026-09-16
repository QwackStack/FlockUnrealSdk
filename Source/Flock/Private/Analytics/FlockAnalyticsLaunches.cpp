// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Analytics/FlockAnalyticsLaunches.h"

#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FlockLaunchFolder.h"
#include "Misc/Paths.h"

namespace
{
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

TSharedRef<FFlockAnalyticsLaunches> FFlockAnalyticsLaunches::Start(const FString& AnalyticsFolder)
{
	const TSharedRef<FFlockAnalyticsLaunches> Launches = MakeShareable(new FFlockAnalyticsLaunches(AnalyticsFolder));
	const FString LaunchesFolder = FPaths::Combine(Launches->AnalyticsFolder, LaunchesFolderName);

	Launches->ThisLaunch = FFlockLaunchFolder::Create(LaunchesFolder);
	if (!Launches->ThisLaunch.IsValid())
	{
		// A launch that cannot show it is running takes over nothing, so it cannot take a running launch's files either.
		Launches->Folder = FPaths::Combine(LaunchesFolder,
			FString::Printf(TEXT("unheld-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower()));
		return Launches;
	}
	Launches->Folder = Launches->ThisLaunch->GetPath();

	Launches->TakeOverEarlierBuildFiles();
	// This launch's own folder is among these; its own lock refuses the claim.
	for (const FString& LaunchFolder : FFlockLaunchFolder::FindFolders(LaunchesFolder))
	{
		const TSharedPtr<FFlockLaunchFolder> Ended = FFlockLaunchFolder::ClaimEnded(LaunchFolder);
		if (!Ended.IsValid())
		{
			continue;
		}
		Launches->TakeOverQueues(LaunchFolder);
		Launches->EndedLaunches.Add(Ended);
		Launches->EndedRecords.Add({ FPaths::Combine(LaunchFolder, SessionStateFileName), FPaths::Combine(LaunchFolder, TerminationMarkerFileName) });
	}
	return Launches;
}

bool FFlockAnalyticsLaunches::IsHoldingItsFolder() const
{
	return ThisLaunch.IsValid();
}

void FFlockAnalyticsLaunches::StopHoldingItsFolderForTesting()
{
	ThisLaunch.Reset();
}

FString FFlockAnalyticsLaunches::GetFolder() const
{
	return Folder;
}

FString FFlockAnalyticsLaunches::GetSessionStatePath() const
{
	return FPaths::Combine(GetFolder(), SessionStateFileName);
}

FString FFlockAnalyticsLaunches::GetTerminationMarkerPath() const
{
	return FPaths::Combine(GetFolder(), TerminationMarkerFileName);
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
	TakeOverQueues(AnalyticsFolder);
	EarlierBuildFilesIndex = EndedRecords.Num();
	EndedLaunches.Add(nullptr);
	EndedRecords.Add({ FPaths::Combine(AnalyticsFolder, SessionStateFileName), FPaths::Combine(AnalyticsFolder, TerminationMarkerFileName) });
}

void FFlockAnalyticsLaunches::TakeOverQueues(const FString& FromFolder)
{
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
			FileManager.Move(*Destination, *FPaths::Combine(From, Entry), /*Replace*/ false, /*EvenIfReadOnly*/ true, /*Attributes*/ false,
				/*bDoNotRetryOrError*/ true);
		}
	}
}

void FFlockAnalyticsLaunches::DeleteEndedLaunch(int32 Index)
{
	if (!EndedRecords.IsValidIndex(Index))
	{
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
