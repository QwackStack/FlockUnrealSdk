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
		bool bKeyFrame = false;
		TArray<uint8> Bytes;
	};

	/** A video file read back the way a player reads it. */
	struct FVideoFileRead
	{
		FString DocType;
		FString Codec;
		int32 Width = 0;
		int32 Height = 0;
		uint64 TimecodeScaleNanoseconds = 0;
		double DurationMs = -1.0;
		/** False while a recording is still being written, or was cut off: its segment keeps the "size unknown" marker. */
		bool bSegmentSizeWritten = false;
		int64 FileBytes = 0;
		TArray<FVideoFileFrame> Frames;
	};

	/**
	 * Reads one EBML number: an element's id when bKeepMarker, or a size when not. Its width is stated by how many
	 * leading zero bits the first byte has, so nothing here assumes the widths the writer happened to choose -- a reader
	 * built from the writer's own layout would agree with it about a mistake.
	 */
	inline bool ReadEbmlNumber(const TArray<uint8>& Bytes, int64 Offset, bool bKeepMarker,
		uint64& OutValue, int32& OutWidth, bool& bOutUnknown)
	{
		bOutUnknown = false;
		if (Offset < 0 || Offset >= Bytes.Num() || Bytes[Offset] == 0)
		{
			return false;
		}
		const uint8 First = Bytes[Offset];
		int32 Width = 1;
		uint8 Marker = 0x80;
		while (Width <= 8 && (First & Marker) == 0)
		{
			Marker >>= 1;
			++Width;
		}
		if (Width > 8 || Offset + Width > Bytes.Num())
		{
			return false;
		}
		uint64 Value = bKeepMarker ? First : static_cast<uint64>(First & (Marker - 1));
		uint64 AllValueBits = bKeepMarker ? 0 : static_cast<uint64>(Marker - 1);
		for (int32 Index = 1; Index < Width; ++Index)
		{
			Value = (Value << 8) | Bytes[Offset + Index];
			AllValueBits = (AllValueBits << 8) | 0xFF;
		}
		bOutUnknown = !bKeepMarker && Value == AllValueBits;
		OutValue = Value;
		OutWidth = Width;
		return true;
	}

	inline uint64 ReadBigEndian(const TArray<uint8>& Bytes, int64 Offset, int32 ByteCount)
	{
		uint64 Value = 0;
		for (int32 Index = 0; Index < ByteCount; ++Index)
		{
			Value = (Value << 8) | Bytes[Offset + Index];
		}
		return Value;
	}

	/** Walks the children of one element, handing each id, content start and content end to Visit. */
	template <typename VisitType>
	inline bool VisitEbmlChildren(const TArray<uint8>& Bytes, int64 Start, int64 End, VisitType&& Visit)
	{
		int64 Offset = Start;
		while (Offset < End)
		{
			uint64 Id = 0;
			int32 IdWidth = 0;
			bool bUnknown = false;
			if (!ReadEbmlNumber(Bytes, Offset, /*bKeepMarker*/ true, Id, IdWidth, bUnknown))
			{
				return false;
			}
			uint64 Size = 0;
			int32 SizeWidth = 0;
			bool bUnknownSize = false;
			if (!ReadEbmlNumber(Bytes, Offset + IdWidth, /*bKeepMarker*/ false, Size, SizeWidth, bUnknownSize))
			{
				return false;
			}
			const int64 ContentStart = Offset + IdWidth + SizeWidth;
			const int64 ContentEnd = bUnknownSize ? End : ContentStart + static_cast<int64>(Size);
			if (ContentEnd > End)
			{
				return false;
			}
			Visit(Id, ContentStart, ContentEnd, bUnknownSize);
			Offset = ContentEnd;
		}
		return true;
	}

	/** Reads the WebM file at Path. False when it is missing, or an element runs past the end of what it sits in. */
	inline bool ReadVideoFile(const FString& Path, FVideoFileRead& Out)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() < 4)
		{
			return false;
		}
		Out.FileBytes = Bytes.Num();

		bool bWellFormed = true;
		const bool bWalked = VisitEbmlChildren(Bytes, 0, Bytes.Num(),
			[&Bytes, &Out, &bWellFormed](uint64 Id, int64 Start, int64 End, bool bUnknownSize)
		{
			if (Id == 0x1A45DFA3)
			{
				bWellFormed &= VisitEbmlChildren(Bytes, Start, End, [&Bytes, &Out](uint64 ChildId, int64 ChildStart, int64 ChildEnd, bool)
				{
					if (ChildId == 0x4282)
					{
						Out.DocType = FString(static_cast<int32>(ChildEnd - ChildStart), reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + ChildStart));
					}
				});
				return;
			}
			if (Id != 0x18538067)
			{
				return;
			}
			Out.bSegmentSizeWritten = !bUnknownSize;
			bWellFormed &= VisitEbmlChildren(Bytes, Start, End, [&Bytes, &Out, &bWellFormed](uint64 SegmentId, int64 SegmentStart, int64 SegmentEnd, bool)
			{
				if (SegmentId == 0x1549A966)
				{
					bWellFormed &= VisitEbmlChildren(Bytes, SegmentStart, SegmentEnd, [&Bytes, &Out](uint64 InfoId, int64 InfoStart, int64 InfoEnd, bool)
					{
						if (InfoId == 0x2AD7B1)
						{
							Out.TimecodeScaleNanoseconds = ReadBigEndian(Bytes, InfoStart, static_cast<int32>(InfoEnd - InfoStart));
						}
						else if (InfoId == 0x4489 && InfoEnd - InfoStart == 8)
						{
							const uint64 Raw = ReadBigEndian(Bytes, InfoStart, 8);
							double Value = 0.0;
							FMemory::Memcpy(&Value, &Raw, sizeof(Value));
							Out.DurationMs = Value;
						}
					});
				}
				else if (SegmentId == 0x1654AE6B)
				{
					bWellFormed &= VisitEbmlChildren(Bytes, SegmentStart, SegmentEnd, [&Bytes, &Out, &bWellFormed](uint64, int64 EntryStart, int64 EntryEnd, bool)
					{
						bWellFormed &= VisitEbmlChildren(Bytes, EntryStart, EntryEnd, [&Bytes, &Out](uint64 TrackId, int64 TrackStart, int64 TrackEnd, bool)
						{
							if (TrackId == 0x86)
							{
								Out.Codec = FString(static_cast<int32>(TrackEnd - TrackStart), reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + TrackStart));
							}
							else if (TrackId == 0xE0)
							{
								VisitEbmlChildren(Bytes, TrackStart, TrackEnd, [&Bytes, &Out](uint64 VideoId, int64 VideoStart, int64 VideoEnd, bool)
								{
									if (VideoId == 0xB0)
									{
										Out.Width = static_cast<int32>(ReadBigEndian(Bytes, VideoStart, static_cast<int32>(VideoEnd - VideoStart)));
									}
									else if (VideoId == 0xBA)
									{
										Out.Height = static_cast<int32>(ReadBigEndian(Bytes, VideoStart, static_cast<int32>(VideoEnd - VideoStart)));
									}
								});
							}
						});
					});
				}
				else if (SegmentId == 0x1F43B675)
				{
					int64 ClusterTimecodeMs = 0;
					bWellFormed &= VisitEbmlChildren(Bytes, SegmentStart, SegmentEnd, [&Bytes, &Out, &ClusterTimecodeMs](uint64 ClusterId, int64 BlockStart, int64 BlockEnd, bool)
					{
						if (ClusterId == 0xE7)
						{
							ClusterTimecodeMs = static_cast<int64>(ReadBigEndian(Bytes, BlockStart, static_cast<int32>(BlockEnd - BlockStart)));
						}
						else if (ClusterId == 0xA3 && BlockEnd - BlockStart > 4)
						{
							// A block holds the track it belongs to, then how far it sits from its cluster's time, then
							// its flags, and the frame after that.
							uint64 Track = 0;
							int32 TrackWidth = 0;
							bool bUnknownTrack = false;
							if (!ReadEbmlNumber(Bytes, BlockStart, /*bKeepMarker*/ false, Track, TrackWidth, bUnknownTrack))
							{
								return;
							}
							const int64 Relative = static_cast<int16>(ReadBigEndian(Bytes, BlockStart + TrackWidth, 2));
							const uint8 Flags = Bytes[BlockStart + TrackWidth + 2];
							const int64 FrameStart = BlockStart + TrackWidth + 3;
							FVideoFileFrame& Frame = Out.Frames.AddDefaulted_GetRef();
							Frame.TimestampMs = ClusterTimecodeMs + Relative;
							Frame.bKeyFrame = (Flags & 0x80) != 0;
							Frame.Bytes.Append(Bytes.GetData() + FrameStart, static_cast<int32>(BlockEnd - FrameStart));
						}
					});
				}
			});
		});
		return bWalked && bWellFormed;
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
