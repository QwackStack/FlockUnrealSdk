// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

/** One frame the encoder produced, ready to write. */
struct FFlockPlaytestEncodedFrame
{
	TArray<uint8> Bytes;
	int64 TimestampMs = 0;
	bool bKeyFrame = false;
};

/**
 * Turns game frames into VP9 video with the engine's libvpx, tuned for recording while the game runs: one pass, no
 * frames held back, a steady bitrate. Every call blocks for the time the encoding takes, so it belongs on a worker
 * thread. One thread at a time.
 *
 * Only a build for 64-bit Windows has the encoder. Elsewhere Initialize fails and says so.
 */
class FFlockPlaytestVideoEncoder
{
public:
	FFlockPlaytestVideoEncoder();
	~FFlockPlaytestVideoEncoder();

	FFlockPlaytestVideoEncoder(const FFlockPlaytestVideoEncoder&) = delete;
	FFlockPlaytestVideoEncoder& operator=(const FFlockPlaytestVideoEncoder&) = delete;

	/** Whether this build carries the encoder at all. */
	static bool IsBuiltWithVideo();

	/** Gets ready for frames of FrameSize. Each side must be even. */
	bool Initialize(FIntPoint FrameSize, int32 FramesPerSecond, int32 BitrateKbps, FString& OutError);

	/**
	 * Encodes one frame of FrameSize pixels, shown at TimestampMs for DurationMs. Adds what the encoder produced to
	 * OutFrames, which can be nothing.
	 */
	bool Encode(const TArray<FColor>& Pixels, int64 TimestampMs, int64 DurationMs, TArray<FFlockPlaytestEncodedFrame>& OutFrames, FString& OutError);

	/** Hands over anything the encoder still holds. Nothing can be encoded afterwards. */
	bool Finish(TArray<FFlockPlaytestEncodedFrame>& OutFrames, FString& OutError);

	/**
	 * Converts colour pixels to the video's own colours (BT.601, limited range): a brightness plane of one byte per
	 * pixel, then two colour planes at half the width and height, each value the average of a two-by-two block. Written
	 * one plane after another into OutPlanes.
	 */
	static void ConvertToVideoColours(const TArray<FColor>& Pixels, FIntPoint FrameSize, TArray<uint8>& OutPlanes);

private:
	struct FLibVpxState;
	TUniquePtr<FLibVpxState> State;
	FIntPoint FrameSize = FIntPoint::ZeroValue;
};
