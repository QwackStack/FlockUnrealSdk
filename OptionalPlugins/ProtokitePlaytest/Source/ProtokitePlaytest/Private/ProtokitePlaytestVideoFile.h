// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

class FArchive;

/** What finishing a video file that stopped being written part of the way through did. */
enum class EProtokitePlaytestInterruptedVideoResult : uint8
{
	/** Its whole frames are kept under the finished name. */
	Finished,
	/** It held no whole frame, so it was deleted. */
	HeldNoFrame,
	/** It could not be opened, changed, renamed or deleted, and is left as it was. */
	CouldNotFinish,
};

/**
 * Writes encoded VP9 frames to a WebM file, which a browser plays in a <video> element with nothing installed.
 *
 * WebM rather than a simpler container because the recording is watched in the dashboard, and rather than writing one
 * format and converting to another afterwards because the conversion would copy the whole file -- up to the size limit --
 * while the player is still in the game, and hold both copies on disk against the recordings budget at once.
 *
 * Written so that a file whose process died is still a file. Every element carries an explicit size, decided before it is
 * written, so nothing has to be patched as the recording runs: one cluster holds one frame, and a recording cut off
 * anywhere leaves whole clusters up to the cut. The two fields that can only be known at the end -- the segment's size and
 * the duration -- are written as fixed-width placeholders and stamped in place when the file is closed. A file that was
 * never closed keeps the segment's "size unknown" marker, which is itself legal WebM, so even an unfinished recording
 * plays up to where it stopped.
 *
 * Fixed-width sizes cost a few bytes a frame over the tightest possible encoding and buy every offset being known in
 * advance, which is what lets the recovery pass walk the file without a parser.
 *
 * Not thread-safe: one thread at a time.
 */
class FProtokitePlaytestVideoFile
{
public:
	/**
	 * The bytes written before the first frame: the EBML header, the segment header, the info and the one video track.
	 *
	 * It counts the name this plugin writes into the file twice (the muxing and writing app), so **renaming the plugin
	 * changes this number**: it went from 151 to 159 when FlockPlaytest became ProtokitePlaytest. Open() refuses to
	 * record rather than write a header of a length the recovery pass does not expect, and says both numbers.
	 */
	static constexpr int64 FileHeaderBytes = 159;
	/** What each frame costs on top of its own bytes: a cluster around it, its time, and the block header. */
	static constexpr int64 FrameHeaderBytes = 23;

	FProtokitePlaytestVideoFile() = default;
	~FProtokitePlaytestVideoFile();

	FProtokitePlaytestVideoFile(const FProtokitePlaytestVideoFile&) = delete;
	FProtokitePlaytestVideoFile& operator=(const FProtokitePlaytestVideoFile&) = delete;

	/** Creates the file at Path, replacing one already there, and writes its header. */
	bool Open(const FString& Path, FIntPoint FrameSize, FString& OutError);

	/**
	 * Appends one frame. bKeyFrame comes from the encoder and is written into the block, because a player seeking to a
	 * time needs to know which frames it can start decoding from. False when the file is not open or the write failed.
	 */
	bool WriteFrame(const TArray<uint8>& FrameBytes, int64 TimestampMs, bool bKeyFrame);

	/** Stamps the segment's size and the duration into the header and closes the file. False when anything could not be saved. */
	bool Close();

	/**
	 * Closes the file without stamping the segment's size or the duration, leaving it exactly as a process that died
	 * part-way through a recording leaves it. Only a test wants this; the recovery pass is what finishes such a file.
	 */
	void AbandonForTesting();

	/**
	 * Finishes a file whose writing stopped part of the way through, because its process ended or a write failed: every
	 * whole frame is kept, a frame only partly written is cut off, the segment size and duration are stamped in, and the
	 * file is renamed from UnfinishedPath to FinishedPath. A file holding no whole frame, or no WebM header, is deleted.
	 * The file is opened shared with nobody, so it cannot change while this runs.
	 */
	static EProtokitePlaytestInterruptedVideoResult FinishInterruptedFile(const FString& UnfinishedPath, const FString& FinishedPath,
		int32& OutFramesKept, FString& OutError);

	int64 GetBytesWritten() const { return BytesWritten; }
	int32 GetFramesWritten() const { return FramesWritten; }
	int64 GetLastTimestampMs() const { return LastTimestampMs; }

private:
	TUniquePtr<FArchive> Archive;
	int64 BytesWritten = 0;
	int32 FramesWritten = 0;
	int64 LastTimestampMs = -1;
};
