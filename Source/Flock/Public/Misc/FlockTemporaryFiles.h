// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Misc/FileHelper.h"

/**
 * Saving a file by writing it beside its destination and moving it over, so a crash part-way through leaves the previous
 * file or none, never half of one.
 *
 * Two processes of one game share every folder under the project (two game clients on one machine, Play In Editor beside
 * a standalone game), and a store deletes what it takes for a crash's leftovers when it starts. Three rules make that
 * safe, and they live here so every store follows all three:
 * - Every write has a temporary file of its own, `<destination>.<32 hex digits>.tmp`, so two processes saving the same
 *   file never write, move or delete each other's.
 * - A temporary file counts as left over only once it is older than LeftOverAfterMinutes. A fresh one may belong to a
 *   process that is writing it now, or has written it and is about to move it.
 * - A move gives up at once when it fails. The file manager would otherwise retry for five seconds on the calling thread
 *   and log an error, which exception capture reports.
 */
class FLOCK_API FFlockTemporaryFiles
{
public:
	/** A temporary file older than this was left behind by a process that never finished its write. */
	static constexpr double LeftOverAfterMinutes = 1.0;

	/** A temporary path beside DestinationPath that no other write uses. Touches nothing on disk. */
	static FString MakePath(const FString& DestinationPath);

	/**
	 * Moves a finished temporary file over DestinationPath. When the move fails the temporary file stays where it is.
	 *
	 * Not one atomic step: the engine deletes the destination and then moves the file in, and it has no call that replaces
	 * in one step. For a moment neither file is there, and when two processes move onto one destination at once, the one
	 * whose move lands second fails.
	 */
	static bool MoveIntoPlace(const FString& TemporaryPath, const FString& DestinationPath);

	/** Writes Text to a temporary file of its own and moves it over DestinationPath. The temporary file is deleted when either step fails. */
	static bool SaveThenMove(const FString& Text, const FString& DestinationPath,
		FFileHelper::EEncodingOptions Encoding = FFileHelper::EEncodingOptions::AutoDetect);

	/** Writes Bytes to a temporary file of its own and moves it over DestinationPath. The temporary file is deleted when either step fails. */
	static bool SaveThenMove(const TArray<uint8>& Bytes, const FString& DestinationPath);

	/**
	 * Deletes the files in Folder whose names match NamePattern (a wildcard such as `*.tmp`) and that are older than
	 * LeftOverAfterMinutes. Returns how many files were deleted.
	 */
	static int32 DeleteLeftOverFiles(const FString& Folder, const FString& NamePattern, bool bIncludeSubfolders);

	/** Deletes DestinationPath's own temporary files (`<destination>.*.tmp` beside it) that are older than LeftOverAfterMinutes. */
	static int32 DeleteLeftOverFilesOf(const FString& DestinationPath);

	/**
	 * Deletes every one of DestinationPath's temporary files, however fresh. For forgetting a file on purpose: a save from
	 * moments ago must not bring back what the player asked to erase.
	 */
	static int32 DeleteTemporaryFilesOf(const FString& DestinationPath);

	/**
	 * DestinationPath's own temporary files, newest first, whatever their age. A save cut off in the moment between the engine
	 * deleting the destination and moving the new file in leaves that new file whole here, so a store whose destination is
	 * missing reads its latest save back from the first of these that reads back whole.
	 */
	static TArray<FString> FindTemporaryFilesOf(const FString& DestinationPath);

	/**
	 * Runs Hook once, on the game thread, just before the next move into place. A test starts a second store there, the way
	 * another process can between this process's write and its move.
	 */
	static void SetBeforeNextMoveForTesting(TFunction<void(const FString& TemporaryPath)> Hook);
};
