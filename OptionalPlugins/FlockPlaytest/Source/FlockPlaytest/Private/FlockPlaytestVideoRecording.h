// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockPlaytestVideoFrameSchedule.h"
#include "Misc/Optional.h"

class IFlockPlaytestVideoFrameSource;
struct FFlockPlaytestVideoFrame;

/** Why a recording stopped. */
enum class EFlockPlaytestVideoStopReason : uint8
{
	StoppedByGame,
	ReachedLengthLimit,
	ReachedSizeLimit,
	PlaytestStopped,
	GameInstanceShutDown,
	CouldNotWrite,
};

/** The reason as the end of a sentence, for example "it reached its length limit". */
FString DescribeVideoStopReason(EFlockPlaytestVideoStopReason Reason);

/** What became of a recording, once its file is written. */
struct FFlockPlaytestVideoRecordingSummary
{
	/** The finished file. Empty when none was kept: no whole frame was written, or the file could not even be finished. */
	FString FilePath;
	EFlockPlaytestVideoStopReason StopReason = EFlockPlaytestVideoStopReason::StoppedByGame;
	int32 FramesWritten = 0;
	/** How long the video plays. */
	double VideoSeconds = 0.0;
	int64 BytesWritten = 0;
	/** Frames thrown away because too many were already waiting to be encoded. */
	int32 FramesDroppedBecauseEncodingFellBehind = 0;
	/** Frames thrown away before encoding because too many encoded frames were already waiting to be written. */
	int32 FramesDroppedBecauseWritingFellBehind = 0;
	/** Capture times that passed while the frames asked for before were still on their way. */
	int32 FramesNotReadyInTime = 0;
	double AverageEncodeMs = 0.0;
	double LongestEncodeMs = 0.0;
	/** The longest a frame waited, after the game thread handed it over, before encoding began. */
	double LongestWaitToEncodeMs = 0.0;
	/** The longest one frame took to write to the file. */
	double LongestWriteMs = 0.0;
	/** Why the recording could not be written to the end; empty when it could. The frames written before can still be kept in FilePath. */
	FString Error;
};

/**
 * One video recording: frames from a source, captured on the schedule, encoded to VP9 and written to a file.
 *
 * The file is written as FilePath plus ".part" and renamed to FilePath once finished, so a file under its final name is
 * always complete, and one that holds no frame is deleted. A write or an encode that fails does not lose what came before
 * it: the file is finished with every whole frame written, and one that cannot even be finished stays under ".part" for
 * the next launch to finish. The game thread only captures and hands
 * frames over. A thread of the recording's own encodes them one at a time, in order, and another writes the encoded frames
 * to the file: measured in live runs, a single write sometimes held the disk for over two seconds, and encoding must not
 * wait for it. At most MaxFramesWaitingToEncode frames wait to be encoded and MaxFramesWaitingToWrite encoded frames wait
 * to be written; past either, frames are dropped before they are encoded, and counted, so memory stays bounded and what
 * is written still decodes. The file never grows past the size limit: a frame counts toward it when it is handed to be
 * written, one that would take the file past is not written, no later frame is either, and the recording stops.
 */
class FFlockPlaytestVideoRecording
{
public:
	static constexpr int32 MaxFramesWaitingToEncode = 8;

	/** Encoded frames are small, so ten seconds of them may wait for a disk that has stopped for a moment. */
	static constexpr int32 MaxFramesWaitingToWrite = 300;

	/**
	 * Sets up the encoder and the file and starts capturing. Null, with OutError, when either cannot be set up. A test
	 * can hand BeforeEachEncodeForTesting or BeforeEachWriteForTesting to hold the encoding or the writing thread before
	 * each frame; BeforeEachWriteForTesting returning false makes that frame's write fail, the way a full disk does.
	 */
	static TSharedPtr<FFlockPlaytestVideoRecording> Start(const TSharedRef<IFlockPlaytestVideoFrameSource>& Source,
		const FFlockPlaytestVideoSettings& Settings, const FString& FilePath, FString& OutError,
		TFunction<void()> BeforeEachEncodeForTesting = nullptr, TFunction<bool()> BeforeEachWriteForTesting = nullptr);

	/** Stops capturing if that has not happened, and waits for the file. */
	~FFlockPlaytestVideoRecording();

	FFlockPlaytestVideoRecording(const FFlockPlaytestVideoRecording&) = delete;
	FFlockPlaytestVideoRecording& operator=(const FFlockPlaytestVideoRecording&) = delete;

	/**
	 * One frame's time, on the game thread. Hands the frames that arrived to be encoded, and asks for this frame when the
	 * schedule captures it. Returns why the recording has to stop, when it has to: a limit was reached, or the file
	 * could not be written. Does nothing once capturing has stopped.
	 */
	TOptional<EFlockPlaytestVideoStopReason> AddFrame(double FrameSeconds);

	/** The next frame's time is not recorded. */
	void LeaveOutNextFrame() { Schedule.LeaveOutNextFrame(); }

	/** Stops capturing for good and has the file finished. Only the first call counts. */
	void StopCapturing(EFlockPlaytestVideoStopReason Reason);

	bool IsCapturing() const { return bCapturing; }

	/** True once the file is finished, after StopCapturing. */
	bool HasFinishedWriting() const;

	/** Blocks until everything handed over has been encoded and written. The file is finished only after StopCapturing. */
	void WaitUntilWritten();

	/** What became of the recording. Only meaningful once HasFinishedWriting is true. */
	FFlockPlaytestVideoRecordingSummary GetSummary() const;

private:
	struct FWriter;
	struct FWorkerThread;

	FFlockPlaytestVideoRecording(const TSharedRef<IFlockPlaytestVideoFrameSource>& InSource, const TSharedRef<FWriter>& InWriter,
		const FFlockPlaytestVideoSettings& Settings);

	void SendFramesToWriter(TArray<FFlockPlaytestVideoFrame>&& Frames);

	TSharedRef<IFlockPlaytestVideoFrameSource> Source;
	TSharedRef<FWriter> Writer;
	FFlockPlaytestVideoFrameSchedule Schedule;

	/** Declared before the encoding thread so it outlives it: encoding hands frames to it until the very end. */
	TUniquePtr<FWorkerThread> FileThread;
	TUniquePtr<FWorkerThread> EncodingThread;

	EFlockPlaytestVideoStopReason StopReason = EFlockPlaytestVideoStopReason::StoppedByGame;
	bool bCapturing = true;
	int32 FramesDroppedBecauseEncodingFellBehind = 0;
	int32 FramesNotReadyInTime = 0;
};
