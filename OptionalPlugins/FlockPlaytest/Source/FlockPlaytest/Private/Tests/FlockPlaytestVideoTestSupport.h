// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

#include "FlockPlaytestRecordingsFolder.h"
#include "FlockPlaytestVideoFrameSchedule.h"
#include "FlockPlaytestVideoFrameSource.h"
#include "HAL/FileManager.h"
#include "Math/RandomStream.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_FLOCK_PLAYTEST_VIDEO
THIRD_PARTY_INCLUDES_START
#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"
THIRD_PARTY_INCLUDES_END
#endif

/** What the video tests share: reading a written file back, test frames, and a frame source that needs no GPU. */
namespace FlockPlaytestVideoTesting
{
	struct FVideoFileFrame
	{
		int64 TimestampMs = 0;
		TArray<uint8> Bytes;
	};

	/** A video file read back byte by byte, the way a player reads it. */
	struct FVideoFileRead
	{
		FString Signature;
		FString Codec;
		int32 Width = 0;
		int32 Height = 0;
		uint32 TimeBaseDenominator = 0;
		uint32 TimeBaseNumerator = 0;
		int32 FrameCountInHeader = -1;
		int64 FileBytes = 0;
		TArray<FVideoFileFrame> Frames;
	};

	inline uint64 ReadLittleEndian(const TArray<uint8>& Bytes, int64 Offset, int32 ByteCount)
	{
		uint64 Value = 0;
		for (int32 Index = 0; Index < ByteCount; ++Index)
		{
			Value |= static_cast<uint64>(Bytes[Offset + Index]) << (8 * Index);
		}
		return Value;
	}

	/** Reads the file at Path. False when it is missing, shorter than its header, or a frame runs past its end. */
	inline bool ReadVideoFile(const FString& Path, FVideoFileRead& Out)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() < 32)
		{
			return false;
		}
		Out.FileBytes = Bytes.Num();
		Out.Signature = FString::Printf(TEXT("%c%c%c%c"), Bytes[0], Bytes[1], Bytes[2], Bytes[3]);
		Out.Codec = FString::Printf(TEXT("%c%c%c%c"), Bytes[8], Bytes[9], Bytes[10], Bytes[11]);
		Out.Width = static_cast<int32>(ReadLittleEndian(Bytes, 12, 2));
		Out.Height = static_cast<int32>(ReadLittleEndian(Bytes, 14, 2));
		Out.TimeBaseDenominator = static_cast<uint32>(ReadLittleEndian(Bytes, 16, 4));
		Out.TimeBaseNumerator = static_cast<uint32>(ReadLittleEndian(Bytes, 20, 4));
		Out.FrameCountInHeader = static_cast<int32>(ReadLittleEndian(Bytes, 24, 4));

		int64 Offset = 32;
		while (Offset < Bytes.Num())
		{
			if (Offset + 12 > Bytes.Num())
			{
				return false;
			}
			const int64 Size = static_cast<int64>(ReadLittleEndian(Bytes, Offset, 4));
			FVideoFileFrame& Frame = Out.Frames.AddDefaulted_GetRef();
			Frame.TimestampMs = static_cast<int64>(ReadLittleEndian(Bytes, Offset + 4, 8));
			Offset += 12;
			if (Offset + Size > Bytes.Num())
			{
				return false;
			}
			Frame.Bytes.Append(Bytes.GetData() + Offset, static_cast<int32>(Size));
			Offset += Size;
		}
		return true;
	}

	/** A picture that changes with Seed: a gradient that slides along, so consecutive frames differ. */
	inline TArray<FColor> MakeTestPixels(FIntPoint Size, int64 Seed)
	{
		TArray<FColor> Pixels;
		Pixels.SetNum(Size.X * Size.Y);
		for (int32 Row = 0; Row < Size.Y; ++Row)
		{
			for (int32 Column = 0; Column < Size.X; ++Column)
			{
				const uint8 Shade = static_cast<uint8>((Column * 255 / FMath::Max(1, Size.X - 1) + Seed * 7) & 0xFF);
				Pixels[Row * Size.X + Column] = FColor(Shade, static_cast<uint8>(Row * 255 / FMath::Max(1, Size.Y - 1)), 128, 255);
			}
		}
		return Pixels;
	}

	/** The video files anywhere under Folder, unfinished ones included but no run's lock or session file, full paths, sorted. */
	inline TArray<FString> RecordingFilesIn(const FString& Folder)
	{
		TArray<FString> Found;
		IFileManager::Get().FindFilesRecursive(Found, *Folder, TEXT("*"), /*Files*/ true, /*Directories*/ false);
		Found = Found.FilterByPredicate([](const FString& Path)
		{
			const FString Name = FPaths::GetCleanFilename(Path);
			return !Name.Equals(FFlockPlaytestRecordingsFolder::LockFileName, ESearchCase::IgnoreCase)
				&& !Name.Equals(FFlockPlaytestRecordingsFolder::SessionFileName, ESearchCase::IgnoreCase)
				&& !Name.Equals(FFlockPlaytestRecordingsFolder::ReservedBytesFileName, ESearchCase::IgnoreCase);
		});
		Found.Sort();
		return Found;
	}

	/**
	 * Frames made on the spot, for tests with no GPU: each frame asked for arrives on the next hand-over, the way the
	 * game viewport's arrive a frame later. Everything it is asked is counted.
	 */
	class FTestVideoFrameSource : public IFlockPlaytestVideoFrameSource
	{
	public:
		explicit FTestVideoFrameSource(FIntPoint InFrameSize) : FrameSize(InFrameSize) {}

		virtual FIntPoint GetFrameSize() const override { return FrameSize; }
		virtual bool IsReadyForAnotherFrame() const override { return !bStopped && bReadyForAnotherFrame; }

		virtual void CaptureFrame(int64 TimestampMs) override
		{
			++FramesAskedFor;
			FFlockPlaytestVideoFrame& Frame = Arrived.AddDefaulted_GetRef();
			Frame.Size = FrameSize;
			Frame.TimestampMs = TimestampMs;
			if (bNoisyPictures)
			{
				// Noise cannot be compressed, so the file grows as fast as the bitrate lets it.
				FRandomStream Random(static_cast<int32>(TimestampMs));
				Frame.Pixels.SetNum(FrameSize.X * FrameSize.Y);
				for (FColor& Pixel : Frame.Pixels)
				{
					Pixel = FColor(static_cast<uint8>(Random.RandHelper(256)), static_cast<uint8>(Random.RandHelper(256)),
						static_cast<uint8>(Random.RandHelper(256)), 255);
				}
			}
			else
			{
				Frame.Pixels = MakeTestPixels(FrameSize, TimestampMs);
			}
		}

		virtual void TakeCapturedFrames(TArray<FFlockPlaytestVideoFrame>& OutFrames) override
		{
			OutFrames.Append(MoveTemp(Arrived));
			Arrived.Reset();
		}

		virtual void Stop(TArray<FFlockPlaytestVideoFrame>& OutFrames) override
		{
			bStopped = true;
			TakeCapturedFrames(OutFrames);
		}

		FIntPoint FrameSize;
		TArray<FFlockPlaytestVideoFrame> Arrived;
		int32 FramesAskedFor = 0;
		bool bReadyForAnotherFrame = true;
		bool bNoisyPictures = false;
		bool bStopped = false;
	};

#if WITH_FLOCK_PLAYTEST_VIDEO
	/**
	 * Decodes every frame of a written file with libvpx's VP9 decoder. Returns how many pictures came out, and the
	 * average brightness of the last one in OutLastAverageBrightness, or -1 when a frame could not be decoded.
	 */
	inline int32 DecodeVideoFile(const FVideoFileRead& File, FIntPoint& OutPictureSize, double& OutLastAverageBrightness)
	{
		vpx_codec_ctx_t Decoder;
		if (vpx_codec_dec_init(&Decoder, vpx_codec_vp9_dx(), nullptr, 0) != VPX_CODEC_OK)
		{
			return -1;
		}
		int32 Pictures = 0;
		for (const FVideoFileFrame& Frame : File.Frames)
		{
			if (vpx_codec_decode(&Decoder, Frame.Bytes.GetData(), static_cast<unsigned int>(Frame.Bytes.Num()), nullptr, 0) != VPX_CODEC_OK)
			{
				vpx_codec_destroy(&Decoder);
				return -1;
			}
			vpx_codec_iter_t Iterator = nullptr;
			while (vpx_image_t* Picture = vpx_codec_get_frame(&Decoder, &Iterator))
			{
				++Pictures;
				OutPictureSize = FIntPoint(static_cast<int32>(Picture->d_w), static_cast<int32>(Picture->d_h));
				double Sum = 0.0;
				for (uint32 Row = 0; Row < Picture->d_h; ++Row)
				{
					const uint8* Line = Picture->planes[VPX_PLANE_Y] + Row * Picture->stride[VPX_PLANE_Y];
					for (uint32 Column = 0; Column < Picture->d_w; ++Column)
					{
						Sum += Line[Column];
					}
				}
				OutLastAverageBrightness = Sum / (static_cast<double>(Picture->d_w) * Picture->d_h);
			}
		}
		vpx_codec_destroy(&Decoder);
		return Pictures;
	}
#endif
}

#endif
