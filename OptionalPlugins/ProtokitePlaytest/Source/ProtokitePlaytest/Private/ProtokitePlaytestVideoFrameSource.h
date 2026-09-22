// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

class FFrameGrabber;
class FSceneViewport;
class UGameInstance;

/** One captured frame: its pixels at the video's size, and when it is shown. */
struct FProtokitePlaytestVideoFrame
{
	TArray<FColor> Pixels;
	FIntPoint Size = FIntPoint::ZeroValue;
	int64 TimestampMs = 0;
};

/** Where a recording's frames come from. Game thread only. */
class IProtokitePlaytestVideoFrameSource
{
public:
	virtual ~IProtokitePlaytestVideoFrameSource() = default;

	/** The size of every frame this source hands over. */
	virtual FIntPoint GetFrameSize() const = 0;

	/** False while the frames already asked for are still on their way, so asking for another would only queue it up. */
	virtual bool IsReadyForAnotherFrame() const = 0;

	/** Asks for the frame being drawn now, to be shown at TimestampMs. It is handed over on a later frame. */
	virtual void CaptureFrame(int64 TimestampMs) = 0;

	/** Hands over the frames that have arrived, oldest first. */
	virtual void TakeCapturedFrames(TArray<FProtokitePlaytestVideoFrame>& OutFrames) = 0;

	/** Stops capturing for good, and hands over the frames that had already arrived. Frames still on their way are dropped. */
	virtual void Stop(TArray<FProtokitePlaytestVideoFrame>& OutFrames) = 0;
};

/**
 * Frames of a game instance's viewport, read back from the GPU by the engine's frame grabber: what the player sees,
 * the game's interface included.
 *
 * A frame asked for during a frame's tick is filled from the next frame drawn, because the engine draws and presents the
 * window before the core ticker runs. Every frame is late by that same one frame, so the video keeps its rhythm.
 */
class FProtokitePlaytestGameViewportFrameSource : public IProtokitePlaytestVideoFrameSource
{
public:
	/**
	 * A source for GameInstance's viewport, with frames at the size FitVideoSizeInside gives for MaxVideoSize. Null when
	 * there is none. OutWhyNot then stays empty when a viewport may still come (none yet, or a window with no size), and
	 * says why when this process can never record.
	 */
	static TSharedPtr<IProtokitePlaytestVideoFrameSource> Create(const UGameInstance* GameInstance, FIntPoint MaxVideoSize, FString& OutWhyNot);

	virtual ~FProtokitePlaytestGameViewportFrameSource() override;

	virtual FIntPoint GetFrameSize() const override { return FrameSize; }
	virtual bool IsReadyForAnotherFrame() const override;
	virtual void CaptureFrame(int64 TimestampMs) override;
	virtual void TakeCapturedFrames(TArray<FProtokitePlaytestVideoFrame>& OutFrames) override;
	virtual void Stop(TArray<FProtokitePlaytestVideoFrame>& OutFrames) override;

private:
	FProtokitePlaytestGameViewportFrameSource(const TSharedRef<FSceneViewport>& InViewport, FIntPoint InFrameSize);

	void StartGrabber(const TSharedRef<FSceneViewport>& InViewport);

	/** Stops the grabber, hands over what had arrived, and releases it, waiting for readbacks the render thread was sent. */
	void StopGrabber(TArray<FProtokitePlaytestVideoFrame>& OutFrames);

	TWeakPtr<FSceneViewport> Viewport;
	TUniquePtr<FFrameGrabber> Grabber;
	FIntPoint FrameSize;

	/** The grabber reads a fixed part of the window, so it is replaced when the viewport's size changes. */
	FIntPoint ViewportSizeWhenGrabberStarted = FIntPoint::ZeroValue;

	int32 FramesOnTheirWay = 0;
	bool bStopped = false;
};
