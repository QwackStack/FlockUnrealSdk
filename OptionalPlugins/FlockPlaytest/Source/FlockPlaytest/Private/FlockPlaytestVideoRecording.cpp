// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockPlaytestVideoRecording.h"

#include "FlockPlaytestVideoEncoder.h"
#include "FlockPlaytestVideoFile.h"
#include "FlockPlaytestVideoFrameSource.h"
#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

#include <atomic>

FString DescribeVideoStopReason(EFlockPlaytestVideoStopReason Reason)
{
	switch (Reason)
	{
	case EFlockPlaytestVideoStopReason::StoppedByGame: return TEXT("the game stopped it");
	case EFlockPlaytestVideoStopReason::ReachedLengthLimit: return TEXT("it reached its length limit");
	case EFlockPlaytestVideoStopReason::ReachedSizeLimit: return TEXT("it reached its size limit");
	case EFlockPlaytestVideoStopReason::PlaytestStopped: return TEXT("playtesting stopped");
	case EFlockPlaytestVideoStopReason::GameInstanceShutDown: return TEXT("the game instance shut down");
	case EFlockPlaytestVideoStopReason::CouldNotWrite: return TEXT("its file could not be written");
	}
	return TEXT("it stopped");
}

/**
 * A thread of the recording's own that runs its work in the order it was handed over. Encoding and writing run on two of
 * these rather than on the engine's shared worker threads, which the engine's own work can hold up.
 */
struct FFlockPlaytestVideoRecording::FWorkerThread : public FRunnable
{
	explicit FWorkerThread(const TCHAR* ThreadName)
		: WorkWaiting(FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ false))
		, AllWorkDone(FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ true))
	{
		AllWorkDone->Trigger();
		Thread = FRunnableThread::Create(this, ThreadName, 0, TPri_Normal);
	}

	virtual ~FWorkerThread() override
	{
		WaitUntilAllWorkDone();
		bStopping = true;
		WorkWaiting->Trigger();
		if (Thread != nullptr)
		{
			Thread->WaitForCompletion();
			delete Thread;
		}
		FPlatformProcess::ReturnSynchEventToPool(WorkWaiting);
		FPlatformProcess::ReturnSynchEventToPool(AllWorkDone);
	}

	void Add(TUniqueFunction<void()> Work)
	{
		// Where no thread could be made, the work runs straight away instead of never.
		if (Thread == nullptr)
		{
			Work();
			return;
		}
		{
			FScopeLock Lock(&QueueLock);
			Queue.Add(MoveTemp(Work));
			AllWorkDone->Reset();
		}
		WorkWaiting->Trigger();
	}

	void WaitUntilAllWorkDone()
	{
		AllWorkDone->Wait();
	}

	virtual uint32 Run() override
	{
		while (true)
		{
			TArray<TUniqueFunction<void()>> Batch;
			{
				FScopeLock Lock(&QueueLock);
				Batch = MoveTemp(Queue);
				Queue.Reset();
				if (Batch.Num() == 0)
				{
					AllWorkDone->Trigger();
					if (bStopping)
					{
						return 0;
					}
				}
			}
			if (Batch.Num() == 0)
			{
				WorkWaiting->Wait();
				continue;
			}
			for (TUniqueFunction<void()>& Work : Batch)
			{
				Work();
			}
		}
	}

	FEvent* WorkWaiting;
	FEvent* AllWorkDone;
	FRunnableThread* Thread = nullptr;
	FCriticalSection QueueLock;
	TArray<TUniqueFunction<void()>> Queue;
	std::atomic<bool> bStopping{ false };
};

/**
 * Everything the encoding and file threads touch. The game thread reads only the atomics, and the rest once bWritten is
 * set. Members marked for one thread are touched by that thread only, except where Finish reads them after waiting for the
 * file thread.
 */
struct FFlockPlaytestVideoRecording::FWriter : public TSharedFromThis<FFlockPlaytestVideoRecording::FWriter>
{
	FFlockPlaytestVideoEncoder Encoder;
	FFlockPlaytestVideoFile File;
	FFlockPlaytestVideoSettings Settings;
	FString FinishedPath;
	FString PartPath;
	TFunction<void()> BeforeEachEncodeForTesting;
	TFunction<bool()> BeforeEachWriteForTesting;

	/** Where encoded frames go to be written. Owned by the recording, which waits for all work before it goes. */
	FWorkerThread* FileThread = nullptr;

	std::atomic<int32> FramesWaitingToEncode{ 0 };
	std::atomic<int32> FramesWaitingToWrite{ 0 };
	std::atomic<bool> bSizeLimitReached{ false };
	std::atomic<bool> bCouldNotWrite{ false };
	std::atomic<bool> bWritten{ false };

	// The encoding thread's.
	int64 BytesHandedToFile = FFlockPlaytestVideoFile::FileHeaderBytes;
	int32 FramesEncoded = 0;
	int32 FramesDroppedBecauseWritingFellBehind = 0;
	double EncodeSecondsTotal = 0.0;
	double LongestEncodeSeconds = 0.0;
	double LongestWaitToEncodeSeconds = 0.0;
	FString EncodeError;
	FString KeptPath;
	int32 FramesInKeptFile = 0;
	int64 BytesInKeptFile = 0;

	// The file thread's.
	double LongestWriteSeconds = 0.0;
	FString WriteError;

	void EncodeFrame(const FFlockPlaytestVideoFrame& Frame)
	{
		if (bCouldNotWrite)
		{
			return;
		}
		if (BeforeEachEncodeForTesting)
		{
			BeforeEachEncodeForTesting();
		}
		// A disk that has stopped for a while leaves encoded frames waiting. Past the limit, frames are dropped before they
		// are encoded, never after, so everything written still decodes.
		if (FramesWaitingToWrite.load() >= MaxFramesWaitingToWrite)
		{
			++FramesDroppedBecauseWritingFellBehind;
			return;
		}

		const double StartSeconds = FPlatformTime::Seconds();
		TArray<FFlockPlaytestEncodedFrame> Encoded;
		FString Error;
		if (!Encoder.Encode(Frame.Pixels, Frame.TimestampMs, Settings.FrameDurationMs(), Encoded, Error))
		{
			EncodeError = Error;
			bCouldNotWrite = true;
			return;
		}
		const double EncodeSeconds = FPlatformTime::Seconds() - StartSeconds;
		EncodeSecondsTotal += EncodeSeconds;
		LongestEncodeSeconds = FMath::Max(LongestEncodeSeconds, EncodeSeconds);
		++FramesEncoded;
		HandFramesToFile(MoveTemp(Encoded));
	}

	/** The one check of the size limit: a frame counts toward it when it is handed to be written. */
	void HandFramesToFile(TArray<FFlockPlaytestEncodedFrame>&& Encoded)
	{
		for (FFlockPlaytestEncodedFrame& Frame : Encoded)
		{
			// Once a frame has been refused, no later one is written either, so the video ends there rather than skipping it.
			if (bSizeLimitReached || BytesHandedToFile + FFlockPlaytestVideoFile::FrameHeaderBytes + Frame.Bytes.Num() > Settings.MaxBytes)
			{
				bSizeLimitReached = true;
				return;
			}
			BytesHandedToFile += FFlockPlaytestVideoFile::FrameHeaderBytes + Frame.Bytes.Num();
			++FramesWaitingToWrite;
			FileThread->Add([Self = AsShared(), Bytes = MoveTemp(Frame.Bytes), TimestampMs = Frame.TimestampMs]()
			{
				Self->WriteFrame(Bytes, TimestampMs);
			});
		}
	}

	/** On the file thread. */
	void WriteFrame(const TArray<uint8>& Bytes, int64 TimestampMs)
	{
		if (!bCouldNotWrite)
		{
			// A test's hook stands in for a disk that holds the write, or refuses it, so it is timed as part of the write.
			const double WriteStartSeconds = FPlatformTime::Seconds();
			const bool bDiskTakesTheWrite = !BeforeEachWriteForTesting || BeforeEachWriteForTesting();
			const bool bFrameWritten = bDiskTakesTheWrite && File.WriteFrame(Bytes, TimestampMs);
			LongestWriteSeconds = FMath::Max(LongestWriteSeconds, FPlatformTime::Seconds() - WriteStartSeconds);
			if (!bFrameWritten)
			{
				WriteError = FString::Printf(TEXT("a write to %s failed (is the disk full?)"), *PartPath);
				bCouldNotWrite = true;
			}
		}
		--FramesWaitingToWrite;
	}

	/** On the encoding thread, as its last work. */
	void Finish()
	{
		if (!bCouldNotWrite)
		{
			TArray<FFlockPlaytestEncodedFrame> Encoded;
			FString Error;
			if (Encoder.Finish(Encoded, Error))
			{
				HandFramesToFile(MoveTemp(Encoded));
			}
			else
			{
				EncodeError = Error;
				bCouldNotWrite = true;
			}
		}

		// Everything handed to be written is written before the file closes.
		FileThread->WaitUntilAllWorkDone();
		if (!File.Close() && !bCouldNotWrite)
		{
			WriteError = FString::Printf(TEXT("%s could not be saved (is the disk full?)"), *PartPath);
			bCouldNotWrite = true;
		}

		if (!bCouldNotWrite)
		{
			if (File.GetFramesWritten() == 0)
			{
				IFileManager::Get().Delete(*PartPath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
			}
			else if (IFileManager::Get().Move(*FinishedPath, *PartPath, /*Replace*/ true, /*EvenIfReadOnly*/ true, /*Attributes*/ false,
				/*bDoNotRetryOrError*/ true))
			{
				KeptPath = FinishedPath;
				FramesInKeptFile = File.GetFramesWritten();
				BytesInKeptFile = File.GetBytesWritten();
			}
			else
			{
				WriteError = FString::Printf(TEXT("%s could not be renamed to %s"), *PartPath, *FinishedPath);
				bCouldNotWrite = true;
			}
		}
		if (bCouldNotWrite)
		{
			// What was recorded before the failure is not lost: the file is finished with every whole frame it holds. One that
			// cannot even be finished now stays as it is, for the next launch to finish.
			int32 FramesKept = 0;
			FString FinishError;
			switch (FFlockPlaytestVideoFile::FinishInterruptedFile(PartPath, FinishedPath, FramesKept, FinishError))
			{
			case EFlockPlaytestInterruptedVideoResult::Finished:
				KeptPath = FinishedPath;
				FramesInKeptFile = FramesKept;
				BytesInKeptFile = IFileManager::Get().FileSize(*FinishedPath);
				break;
			case EFlockPlaytestInterruptedVideoResult::HeldNoFrame:
				break;
			case EFlockPlaytestInterruptedVideoResult::CouldNotFinish:
				WriteError = WriteError.IsEmpty() ? FinishError : WriteError + TEXT("; ") + FinishError;
				break;
			}
		}
		bWritten = true;
	}
};

TSharedPtr<FFlockPlaytestVideoRecording> FFlockPlaytestVideoRecording::Start(const TSharedRef<IFlockPlaytestVideoFrameSource>& Source,
	const FFlockPlaytestVideoSettings& Settings, const FString& FilePath, FString& OutError, TFunction<void()> BeforeEachEncodeForTesting,
	TFunction<bool()> BeforeEachWriteForTesting)
{
	const TSharedRef<FWriter> Writer = MakeShared<FWriter>();
	Writer->Settings = Settings;
	Writer->FinishedPath = FilePath;
	Writer->PartPath = FilePath + TEXT(".part");
	Writer->BeforeEachEncodeForTesting = MoveTemp(BeforeEachEncodeForTesting);
	Writer->BeforeEachWriteForTesting = MoveTemp(BeforeEachWriteForTesting);

	if (!Writer->Encoder.Initialize(Source->GetFrameSize(), Settings.FramesPerSecond, Settings.BitrateKbps, OutError))
	{
		return nullptr;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), /*Tree*/ true);
	if (!Writer->File.Open(Writer->PartPath, Source->GetFrameSize(), OutError))
	{
		return nullptr;
	}
	return MakeShareable(new FFlockPlaytestVideoRecording(Source, Writer, Settings));
}

FFlockPlaytestVideoRecording::FFlockPlaytestVideoRecording(const TSharedRef<IFlockPlaytestVideoFrameSource>& InSource,
	const TSharedRef<FWriter>& InWriter, const FFlockPlaytestVideoSettings& Settings)
	: Source(InSource)
	, Writer(InWriter)
	, Schedule(Settings.FramesPerSecond, Settings.MaxSeconds)
	, FileThread(MakeUnique<FWorkerThread>(TEXT("FlockPlaytestVideoFileWriter")))
	, EncodingThread(MakeUnique<FWorkerThread>(TEXT("FlockPlaytestVideoEncoder")))
{
	Writer->FileThread = FileThread.Get();
}

FFlockPlaytestVideoRecording::~FFlockPlaytestVideoRecording()
{
	StopCapturing(EFlockPlaytestVideoStopReason::GameInstanceShutDown);
	WaitUntilWritten();
}

TOptional<EFlockPlaytestVideoStopReason> FFlockPlaytestVideoRecording::AddFrame(double FrameSeconds)
{
	if (!bCapturing)
	{
		return {};
	}

	TArray<FFlockPlaytestVideoFrame> Arrived;
	Source->TakeCapturedFrames(Arrived);
	SendFramesToWriter(MoveTemp(Arrived));

	if (Writer->bCouldNotWrite)
	{
		return EFlockPlaytestVideoStopReason::CouldNotWrite;
	}
	if (Writer->bSizeLimitReached)
	{
		return EFlockPlaytestVideoStopReason::ReachedSizeLimit;
	}

	int64 TimestampMs = 0;
	switch (Schedule.AddFrame(FrameSeconds, TimestampMs))
	{
	case FFlockPlaytestVideoFrameSchedule::EFrameResult::ReachedLengthLimit:
		return EFlockPlaytestVideoStopReason::ReachedLengthLimit;
	case FFlockPlaytestVideoFrameSchedule::EFrameResult::Capture:
		if (Source->IsReadyForAnotherFrame())
		{
			Source->CaptureFrame(TimestampMs);
		}
		else
		{
			++FramesNotReadyInTime;
		}
		break;
	case FFlockPlaytestVideoFrameSchedule::EFrameResult::Skip:
		break;
	}
	return {};
}

void FFlockPlaytestVideoRecording::StopCapturing(EFlockPlaytestVideoStopReason Reason)
{
	if (!bCapturing)
	{
		return;
	}
	bCapturing = false;
	StopReason = Reason;

	TArray<FFlockPlaytestVideoFrame> Remaining;
	Source->Stop(Remaining);
	SendFramesToWriter(MoveTemp(Remaining));

	const TSharedRef<FWriter> FinishingWriter = Writer;
	EncodingThread->Add([FinishingWriter]() { FinishingWriter->Finish(); });
}

bool FFlockPlaytestVideoRecording::HasFinishedWriting() const
{
	return Writer->bWritten.load();
}

void FFlockPlaytestVideoRecording::WaitUntilWritten()
{
	// Encoding first: it is what hands the file thread its work.
	EncodingThread->WaitUntilAllWorkDone();
	FileThread->WaitUntilAllWorkDone();
}

FFlockPlaytestVideoRecordingSummary FFlockPlaytestVideoRecording::GetSummary() const
{
	FFlockPlaytestVideoRecordingSummary Summary;
	Summary.StopReason = StopReason;
	Summary.FramesDroppedBecauseEncodingFellBehind = FramesDroppedBecauseEncodingFellBehind;
	Summary.FramesNotReadyInTime = FramesNotReadyInTime;
	if (!HasFinishedWriting())
	{
		return Summary;
	}
	Summary.FilePath = Writer->KeptPath;
	Summary.Error = Writer->EncodeError.IsEmpty() ? Writer->WriteError
		: Writer->WriteError.IsEmpty() ? Writer->EncodeError : Writer->EncodeError + TEXT("; ") + Writer->WriteError;
	// A file finished after a failed write keeps the whole frames found in it, which is what it plays.
	Summary.FramesWritten = Summary.FilePath.IsEmpty() ? Writer->File.GetFramesWritten() : Writer->FramesInKeptFile;
	Summary.BytesWritten = Summary.FilePath.IsEmpty() ? Writer->File.GetBytesWritten() : Writer->BytesInKeptFile;
	Summary.FramesDroppedBecauseWritingFellBehind = Writer->FramesDroppedBecauseWritingFellBehind;
	if (Summary.FramesWritten > 0)
	{
		Summary.VideoSeconds = (Writer->File.GetLastTimestampMs() + Writer->Settings.FrameDurationMs()) / 1000.0;
	}
	if (Writer->FramesEncoded > 0)
	{
		Summary.AverageEncodeMs = Writer->EncodeSecondsTotal / Writer->FramesEncoded * 1000.0;
	}
	Summary.LongestEncodeMs = Writer->LongestEncodeSeconds * 1000.0;
	Summary.LongestWaitToEncodeMs = Writer->LongestWaitToEncodeSeconds * 1000.0;
	Summary.LongestWriteMs = Writer->LongestWriteSeconds * 1000.0;
	return Summary;
}

void FFlockPlaytestVideoRecording::SendFramesToWriter(TArray<FFlockPlaytestVideoFrame>&& Frames)
{
	for (FFlockPlaytestVideoFrame& Frame : Frames)
	{
		if (Writer->FramesWaitingToEncode.load() >= MaxFramesWaitingToEncode)
		{
			++FramesDroppedBecauseEncodingFellBehind;
			continue;
		}
		++Writer->FramesWaitingToEncode;
		const TSharedRef<FFlockPlaytestVideoFrame> Shared = MakeShared<FFlockPlaytestVideoFrame>(MoveTemp(Frame));
		const TSharedRef<FWriter> EncodingWriter = Writer;
		const double HandedOverSeconds = FPlatformTime::Seconds();
		EncodingThread->Add([EncodingWriter, Shared, HandedOverSeconds]()
		{
			EncodingWriter->LongestWaitToEncodeSeconds = FMath::Max(EncodingWriter->LongestWaitToEncodeSeconds,
				FPlatformTime::Seconds() - HandedOverSeconds);
			EncodingWriter->EncodeFrame(*Shared);
			--EncodingWriter->FramesWaitingToEncode;
		});
	}
}
