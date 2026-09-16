// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

class FFlockLaunchFolder;
class IFileHandle;

/** Where a launch that has ended kept its crash marker and its live-session record. */
struct FLOCK_API FFlockEndedLaunchRecords
{
	FString SessionStatePath;
	FString TerminationMarkerPath;
};

/**
 * The analytics files of each launch of the game, in a folder of that launch's own:
 * `<analytics folder>/launches/<UTC time>-<8 hex>/` holds its crash marker, its live-session record and its three queues.
 *
 * Two processes of one game share every folder under the project (two game clients on one machine, Play In Editor beside a
 * standalone game), so no launch may read another's files as left over while that launch still runs. Each launch holds its
 * folder's lock (FFlockLaunchFolder) for as long as this object lives, which is as long as the analytics provider that owns
 * it. When a launch starts, it takes over every launch whose lock it can claim: their queued entries move into this launch's
 * queues at once, and their crash markers and live-session records wait for the analytics provider, which reports each once
 * and then deletes that launch's folder (DeleteEndedLaunch). Files a build before 1.16.0 kept straight in the analytics folder
 * are taken over the same way, once, under a lock of their own; a game of such a build still running at that moment is not
 * told apart from one that ended.
 *
 * The consent decision and the coverage notice stay in the analytics folder: they belong to the install, not to a launch.
 * Game thread only.
 */
class FLOCK_API FFlockAnalyticsLaunches
{
public:
	static constexpr const TCHAR* LaunchesFolderName = TEXT("launches");
	static constexpr const TCHAR* SessionStateFileName = TEXT("session_state.json");
	static constexpr const TCHAR* TerminationMarkerFileName = TEXT("termination_marker.json");
	static constexpr const TCHAR* LogEventsQueueName = TEXT("log_events");
	static constexpr const TCHAR* SessionEndsQueueName = TEXT("session_ends");
	static constexpr const TCHAR* AnalyticsEventsQueueName = TEXT("analytics_events");
	/** Held while the files a build before 1.16.0 kept straight in the analytics folder are taken over. */
	static constexpr const TCHAR* EarlierBuildFilesLockName = TEXT("earlier-build-files.lock");

	/**
	 * Makes this launch's folder under AnalyticsFolder, holds its lock, and takes over every launch that has ended, and the files
	 * of an earlier build. When the lock cannot be taken, this launch still gets a folder of its own but takes over nothing.
	 */
	static TSharedRef<FFlockAnalyticsLaunches> Start(const FString& AnalyticsFolder);

	/** `<ProjectSavedDir>/Flock/analytics`, as a full path. */
	static FString DefaultFolder();

	~FFlockAnalyticsLaunches();

	/** False when this launch's lock could not be taken; nothing was taken over then. */
	bool IsHoldingItsFolder() const;

	/** This launch's folder. The event queues are its subfolders. */
	FString GetFolder() const;
	FString GetSessionStatePath() const;
	FString GetTerminationMarkerPath() const;

	/** The records of every launch Start took over, oldest first. */
	const TArray<FFlockEndedLaunchRecords>& GetEndedLaunchRecords() const { return EndedRecords; }

	/** Deletes the launch at Index in GetEndedLaunchRecords, once its records are reported. */
	void DeleteEndedLaunch(int32 Index);

	/** Lets go of every launch taken over and not deleted, so a later launch finds what it kept. Forgets the records. */
	void LetGoOfEndedLaunches();

	/** Lets go of this launch's lock while everything else stays, the way a launch that crashed looks to the next one. */
	void StopHoldingItsFolderForTesting();

private:
	explicit FFlockAnalyticsLaunches(const FString& InAnalyticsFolder);

	/** Moves every queued entry in FromFolder's queues into this launch's queues. */
	void TakeOverQueues(const FString& FromFolder);

	void TakeOverEarlierBuildFiles();

	FString AnalyticsFolder;
	/** This launch's folder, held or not. */
	FString Folder;
	TSharedPtr<FFlockLaunchFolder> ThisLaunch;
	/** Parallel to EndedRecords. Empty at the earlier build's index, whose lock is EarlierBuildFilesLock. */
	TArray<TSharedPtr<FFlockLaunchFolder>> EndedLaunches;
	TArray<FFlockEndedLaunchRecords> EndedRecords;
	TUniquePtr<IFileHandle> EarlierBuildFilesLock;
	int32 EarlierBuildFilesIndex = INDEX_NONE;
};
