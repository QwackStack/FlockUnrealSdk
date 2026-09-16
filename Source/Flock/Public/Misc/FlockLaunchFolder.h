// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

class IFileHandle;

/**
 * A folder one launch of the game keeps its own files in, under a parent folder every launch shares.
 *
 * Two processes of one game share every folder under the project (two game clients on one machine, Play In Editor beside a
 * standalone game), so whether a launch is still running cannot be read from a time, a process id or a version. The owner
 * keeps a lock file inside its folder open, shared with nobody, for as long as it holds the folder, and the operating system
 * lets go of it however the owner ends. Another launch touches the folder only after opening that lock itself (ClaimEnded),
 * which Windows' share modes and Unix's flock refuse while the owner still holds it, in the same process as well.
 */
class FLOCK_API FFlockLaunchFolder
{
public:
	static constexpr const TCHAR* LockFileName = TEXT("in-use.lock");

	/** Makes a folder of its own under ParentFolder, named for the time and eight random hex digits, and holds its lock. Null when it could not. */
	static TSharedPtr<FFlockLaunchFolder> Create(const FString& ParentFolder);

	/** Holds Folder when the launch that made it has let go of its lock. Null while the lock is still held, and for a folder with no lock. */
	static TSharedPtr<FFlockLaunchFolder> ClaimEnded(const FString& Folder);

	/** The folders directly under ParentFolder, as full paths, oldest name first. */
	static TArray<FString> FindFolders(const FString& ParentFolder);

	~FFlockLaunchFolder();

	/** The folder's full path. */
	const FString& GetPath() const { return Path; }

	/**
	 * Deletes everything in the folder, its lock last, then the folder, and lets go of it. When anything could not be deleted the
	 * lock file stays, so a later launch still finds the folder and tries again. Returns whether the folder is gone.
	 */
	bool DeleteEverything();

private:
	FFlockLaunchFolder(const FString& InPath, TUniquePtr<IFileHandle> InLock);

	FString Path;
	TUniquePtr<IFileHandle> Lock;
};
