// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

class IFileHandle;

/** Which recordings a run's folder holds. */
enum class EProtokitePlaytestRecordingKind : uint8
{
	/** A playtest's recording. Kept until it is uploaded, by this launch or a later one. */
	Playtest,
	/** A test video, from ProtokitePlaytest.RecordTestVideo or Record Video In Play In Editor. Never uploaded. */
	TestVideo,
};

/**
 * The Protokite session a playtest recording belongs to, as a later launch has to name it to upload the recording. Never
 * holds the API key: a later launch sends its own.
 */
struct FProtokitePlaytestRecordingSession
{
	FString ProtokiteSessionId;
	FString ProtokiteApiUrl;
	/** The Game Version ID the session started with. Protokite finds the session's playtest from it. */
	FString FlockGameVersionId;

	bool IsEmpty() const { return ProtokiteSessionId.IsEmpty(); }
};

/** A playtest recording that an ended run left, finished, with the session it is to be uploaded to. */
struct FProtokitePlaytestRecordingWaitingToUpload
{
	FString RunFolder;
	FString VideoFilePath;
	int64 VideoBytes = 0;
	FProtokitePlaytestRecordingSession Session;
};

/** What a launch found, finished and deleted among the runs that ended before it. */
struct FProtokitePlaytestWhatEndedRunsLeft
{
	/** Playtest recordings kept, waiting to be uploaded, the ones finished below included. */
	int32 RecordingsWaitingToUpload = 0;
	/** Recordings a run was still writing when it ended, finished with every whole frame they held. */
	int32 InterruptedRecordingsFinished = 0;
	/** Playtest recordings deleted because no Protokite session started for them, so they can never be uploaded. */
	int32 RecordingsWithoutASessionDeleted = 0;
	/** Files that could not be finished or deleted, left for the next launch to try again. */
	TArray<FString> FilesLeftForTheNextLaunch;
};

/** What making room for a new recording deleted, and how much room it found. */
struct FProtokitePlaytestRecordingsRoom
{
	/** What the runs in the folder use after the deletions, a running game's counted at the size it may still grow to. */
	int64 BytesUsed = 0;
	/** What the budget has left after the deletions: how large the new recording may grow. */
	int64 BytesLeftInBudget = 0;
	TArray<FString> PlaytestRecordingsDeleted;
	TArray<FString> TestVideosDeleted;
};

/**
 * One run's hold on its folder under the recordings folder: Playtest/<run name> or TestVideos/<run name>, holding the
 * video file, a playtest recording's session file, how large the video may grow, and a lock file this keeps open, shared
 * with nobody, for as long as it lives. The operating system lets go of the lock when the process ends, however it ends,
 * so a launch tells a run that is still going from one that has ended by trying to open the lock itself: only then does
 * it touch what the run left.
 */
class FProtokitePlaytestRecordingRun
{
public:
	/**
	 * A new run, named from the UTC time and eight random hex digits. ReservedBytes, how large its video may grow, is saved
	 * in its folder, so a launch making room while this one still records counts it at that size. Null, with OutError, when
	 * the folder, its lock or that file cannot be made.
	 */
	static TSharedPtr<FProtokitePlaytestRecordingRun> Create(const FString& RecordingsFolder, EProtokitePlaytestRecordingKind Kind, int64 ReservedBytes,
		FString& OutError);

	/** A new run named RunName, for a test that needs runs in a known order: room is made oldest first, by name. */
	static TSharedPtr<FProtokitePlaytestRecordingRun> CreateNamedForTesting(const FString& RecordingsFolder, EProtokitePlaytestRecordingKind Kind,
		const FString& RunName, int64 ReservedBytes, FString& OutError);

	/**
	 * Takes hold of a run that has ended, so nothing else touches it while this lives. Null when RunFolder is not a run's
	 * folder, or its run is still going, or another launch holds it.
	 */
	static TSharedPtr<FProtokitePlaytestRecordingRun> ClaimEnded(const FString& RunFolder);

	~FProtokitePlaytestRecordingRun();

	FProtokitePlaytestRecordingRun(const FProtokitePlaytestRecordingRun&) = delete;
	FProtokitePlaytestRecordingRun& operator=(const FProtokitePlaytestRecordingRun&) = delete;

	const FString& GetFolder() const { return Folder; }
	const FString& GetName() const { return Name; }
	EProtokitePlaytestRecordingKind GetKind() const { return Kind; }

	/** Where the run's video is saved once finished: recording-<run name>.webm or test-recording-<run name>.webm. */
	FString GetVideoFilePath() const;

	/** The same path with .part added, which the video has while it is being written. */
	FString GetUnfinishedVideoFilePath() const;

	/** Saves the Protokite session the recording belongs to beside it, replacing one saved before. */
	bool SaveSession(const FProtokitePlaytestRecordingSession& Session, FString& OutError) const;

	/** The saved session; empty when none was saved or the file cannot be read. */
	FProtokitePlaytestRecordingSession LoadSession() const;

	/**
	 * Deletes every file in the folder, the lock last, then the folder, and lets go of the run. When a file cannot be
	 * deleted (another program has it open, say), it is added to OutFilesLeft, the lock stays and so does the hold.
	 */
	bool DeleteEverything(TArray<FString>& OutFilesLeft);

private:
	FProtokitePlaytestRecordingRun(const FString& InFolder, const FString& InName, EProtokitePlaytestRecordingKind InKind, TUniquePtr<IFileHandle> InLock);

	static TSharedPtr<FProtokitePlaytestRecordingRun> CreateWithName(const FString& RecordingsFolder, EProtokitePlaytestRecordingKind Kind,
		const FString& RunName, int64 ReservedBytes, FString& OutError);

	FString Folder;
	FString Name;
	EProtokitePlaytestRecordingKind Kind;
	TUniquePtr<IFileHandle> Lock;
};

/**
 * The folder recordings are kept in, and what a launch does with what earlier runs left there. A recording belongs to its
 * run, never to a game version: nothing here reads one, so a Flock Game Version ID changing, during a run or between
 * launches, deletes nothing. Only run folders under Playtest and TestVideos are touched, and only once their run has
 * ended; everything else in the folder, and everything beside it, is left alone.
 */
class FProtokitePlaytestRecordingsFolder
{
public:
	static constexpr const TCHAR* PlaytestFolderName = TEXT("Playtest");
	static constexpr const TCHAR* TestVideoFolderName = TEXT("TestVideos");
	static constexpr const TCHAR* LockFileName = TEXT("in-use.lock");
	static constexpr const TCHAR* SessionFileName = TEXT("session.json");
	static constexpr const TCHAR* ReservedBytesFileName = TEXT("reserved-bytes.txt");

	/** A recording is not started with less room than this. */
	static constexpr int64 SmallestRoomForARecording = 1024 * 1024;

	/** Saved/ProtokitePlaytest/Recordings under the project, as a full path. */
	static FString GetDefaultPath();

	/**
	 * Goes through every run that has ended. A video it was still writing is finished with the whole frames it holds, or
	 * deleted when it holds none. A playtest recording is kept, waiting to be uploaded, when a Protokite session was saved
	 * for it, and deleted when none was, since it can never be uploaded. A finished test video is kept. A run left with no
	 * video is removed, and a temporary file a crash left in a run is deleted.
	 */
	static FProtokitePlaytestWhatEndedRunsLeft FinishWhatEndedRunsLeft(const FString& RecordingsFolder);

	/** The finished playtest recordings of ended runs that have a session to be uploaded to, oldest first. */
	static TArray<FProtokitePlaytestRecordingWaitingToUpload> FindRecordingsWaitingToUpload(const FString& RecordingsFolder);

	/**
	 * Makes room for a recording expected to need BytesWanted inside BudgetBytes, counting every run in the folder, a
	 * running game's at the size it may still grow to. Until it fits, ended runs are deleted: test videos first, oldest
	 * first, and only when that is not enough the playtest recordings waiting to be uploaded, oldest first. A run still
	 * going is never deleted. Answers what the budget has left.
	 */
	static FProtokitePlaytestRecordingsRoom MakeRoom(const FString& RecordingsFolder, int64 BudgetBytes, int64 BytesWanted);

	/** The folder the runs of Kind are kept in. */
	static FString GetKindFolder(const FString& RecordingsFolder, EProtokitePlaytestRecordingKind Kind);
};
