// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

class FArchive;

/**
 * Writes encoded VP9 frames to a file in the IVF format: a 32-byte file header, then each frame behind a 12-byte header
 * holding its size and its time in milliseconds. VLC and ffplay play it. Not thread-safe: one thread at a time.
 */
class FFlockPlaytestVideoFile
{
public:
	static constexpr int64 FileHeaderBytes = 32;
	static constexpr int64 FrameHeaderBytes = 12;

	FFlockPlaytestVideoFile() = default;
	~FFlockPlaytestVideoFile();

	FFlockPlaytestVideoFile(const FFlockPlaytestVideoFile&) = delete;
	FFlockPlaytestVideoFile& operator=(const FFlockPlaytestVideoFile&) = delete;

	/** Creates the file at Path, replacing one already there, and writes its header. */
	bool Open(const FString& Path, FIntPoint FrameSize, FString& OutError);

	/** Appends one frame. False when the file is not open or the write failed. */
	bool WriteFrame(const TArray<uint8>& FrameBytes, int64 TimestampMs);

	/** Writes the frame count into the header and closes the file. False when anything written could not be saved. */
	bool Close();

	int64 GetBytesWritten() const { return BytesWritten; }
	int32 GetFramesWritten() const { return FramesWritten; }
	int64 GetLastTimestampMs() const { return LastTimestampMs; }

private:
	TUniquePtr<FArchive> Archive;
	int64 BytesWritten = 0;
	int32 FramesWritten = 0;
	int64 LastTimestampMs = -1;
};
