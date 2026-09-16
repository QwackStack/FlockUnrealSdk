// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockPlaytestVideoFile.h"

#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Serialization/Archive.h"

namespace
{
	// Element ids, each held as the bytes WebM writes for it. They are not numbers to be encoded: the id *is* the encoding,
	// which is why a one-byte id like 0xAE has its high bit set already.
	constexpr uint32 IdEbmlHeader          = 0x1A45DFA3;
	constexpr uint32 IdEbmlVersion         = 0x4286;
	constexpr uint32 IdEbmlReadVersion     = 0x42F7;
	constexpr uint32 IdEbmlMaxIdLength     = 0x42F2;
	constexpr uint32 IdEbmlMaxSizeLength   = 0x42F3;
	constexpr uint32 IdDocType             = 0x4282;
	constexpr uint32 IdDocTypeVersion      = 0x4287;
	constexpr uint32 IdDocTypeReadVersion  = 0x4285;
	constexpr uint32 IdSegment             = 0x18538067;
	constexpr uint32 IdInfo                = 0x1549A966;
	constexpr uint32 IdTimecodeScale       = 0x2AD7B1;
	constexpr uint32 IdMuxingApp           = 0x4D80;
	constexpr uint32 IdWritingApp          = 0x5741;
	constexpr uint32 IdDuration            = 0x4489;
	constexpr uint32 IdTracks              = 0x1654AE6B;
	constexpr uint32 IdTrackEntry          = 0xAE;
	constexpr uint32 IdTrackNumber         = 0xD7;
	constexpr uint32 IdTrackUid            = 0x73C5;
	constexpr uint32 IdTrackType            = 0x83;
	constexpr uint32 IdCodecId             = 0x86;
	constexpr uint32 IdVideo               = 0xE0;
	constexpr uint32 IdPixelWidth          = 0xB0;
	constexpr uint32 IdPixelHeight         = 0xBA;
	constexpr uint32 IdCluster             = 0x1F43B675;
	constexpr uint32 IdTimecode            = 0xE7;
	constexpr uint32 IdSimpleBlock         = 0xA3;

	/** One millisecond per tick, so every time in the file is a whole number of milliseconds. */
	constexpr uint64 TimecodeScaleNanoseconds = 1000000;

	/** Where the segment's size sits: straight after its four-byte id, itself after the EBML header. */
	constexpr int64 SegmentSizeOffset = 40;
	/** Where the duration's eight float bytes sit inside the info element. */
	constexpr int64 DurationValueOffset = 99;
	/** How far into a cluster the frame's own first byte is. Fixed, because every field before it has a fixed width. */
	constexpr int64 FrameStartInCluster = 23;

	const ANSICHAR* const WrittenBy = "FlockPlaytest";

	void AppendId(TArray<uint8>& Bytes, uint32 Id)
	{
		if (Id > 0x00FFFFFF) { Bytes.Add(static_cast<uint8>((Id >> 24) & 0xFF)); }
		if (Id > 0x0000FFFF) { Bytes.Add(static_cast<uint8>((Id >> 16) & 0xFF)); }
		if (Id > 0x000000FF) { Bytes.Add(static_cast<uint8>((Id >> 8) & 0xFF)); }
		Bytes.Add(static_cast<uint8>(Id & 0xFF));
	}

	// A size is written big-endian with a leading 1 bit saying how many bytes it takes. The width is passed in rather than
	// chosen from the value, so that a size which has to be stamped in later never needs more room than it was given.
	void AppendSize(TArray<uint8>& Bytes, uint64 Size, int32 Width)
	{
		for (int32 Index = 0; Index < Width; ++Index)
		{
			uint8 Byte = static_cast<uint8>((Size >> (8 * (Width - 1 - Index))) & 0xFF);
			if (Index == 0)
			{
				Byte |= static_cast<uint8>(1 << (8 - Width));
			}
			Bytes.Add(Byte);
		}
	}

	/** Matroska writes its numbers big-endian, the opposite way round from the IVF this replaced. */
	void AppendUnsigned(TArray<uint8>& Bytes, uint64 Value, int32 Width)
	{
		for (int32 Index = 0; Index < Width; ++Index)
		{
			Bytes.Add(static_cast<uint8>((Value >> (8 * (Width - 1 - Index))) & 0xFF));
		}
	}

	void AppendUnsignedElement(TArray<uint8>& Bytes, uint32 Id, uint64 Value, int32 ValueWidth)
	{
		AppendId(Bytes, Id);
		AppendSize(Bytes, static_cast<uint64>(ValueWidth), 1);
		AppendUnsigned(Bytes, Value, ValueWidth);
	}

	void AppendStringElement(TArray<uint8>& Bytes, uint32 Id, const ANSICHAR* Value)
	{
		const int32 Length = FCStringAnsi::Strlen(Value);
		AppendId(Bytes, Id);
		AppendSize(Bytes, static_cast<uint64>(Length), 1);
		for (int32 Index = 0; Index < Length; ++Index)
		{
			Bytes.Add(static_cast<uint8>(Value[Index]));
		}
	}

	/** The duration is a float in timecode-scale units, so eight bytes big-endian. */
	void MakeDurationBytes(double Milliseconds, uint8(&OutBytes)[8])
	{
		uint64 Bits = 0;
		FMemory::Memcpy(&Bits, &Milliseconds, sizeof(Bits));
		for (int32 Index = 0; Index < 8; ++Index)
		{
			OutBytes[Index] = static_cast<uint8>((Bits >> (8 * (7 - Index))) & 0xFF);
		}
	}

	uint64 ReadUnsignedBigEndian(const uint8* Bytes, int32 Width)
	{
		uint64 Value = 0;
		for (int32 Index = 0; Index < Width; ++Index)
		{
			Value = (Value << 8) | Bytes[Index];
		}
		return Value;
	}
}

EFlockPlaytestInterruptedVideoResult FFlockPlaytestVideoFile::FinishInterruptedFile(const FString& UnfinishedPath, const FString& FinishedPath,
	int32& OutFramesKept, FString& OutError)
{
	OutFramesKept = 0;
	int32 Frames = 0;
	{
		const TUniquePtr<IFileHandle> Handle(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*UnfinishedPath, /*bAppend*/ true, /*bAllowRead*/ true));
		if (!Handle.IsValid())
		{
			OutError = FString::Printf(TEXT("%s could not be opened (is another program using it?)"), *UnfinishedPath);
			return EFlockPlaytestInterruptedVideoResult::CouldNotFinish;
		}

		const int64 FileBytes = Handle->Size();
		int64 WholeClustersEnd = 0;
		int64 LastTimecodeMs = 0;
		uint8 Start[4];
		if (FileBytes >= FileHeaderBytes && Handle->Seek(0) && Handle->Read(Start, 4)
			&& Start[0] == 0x1A && Start[1] == 0x45 && Start[2] == 0xDF && Start[3] == 0xA3)
		{
			WholeClustersEnd = FileHeaderBytes;
			// Every cluster here was written in one piece with its size already known, so walking them needs no parser:
			// a cluster is whole when its declared size fits inside what the file actually holds.
			// The cluster's fixed head, plus the frame's own first byte straight after it -- which is the byte the VP9
			// check below needs, and is one past the head rather than the last byte of it.
			uint8 Cluster[FrameStartInCluster + 1];
			while (WholeClustersEnd + FrameStartInCluster + 1 <= FileBytes
				&& Handle->Seek(WholeClustersEnd) && Handle->Read(Cluster, FrameStartInCluster + 1))
			{
				if (Cluster[0] != 0x1F || Cluster[1] != 0x43 || Cluster[2] != 0xB6 || Cluster[3] != 0x75)
				{
					break;
				}
				// The size is a four-byte field whose top nibble is the width marker.
				if ((Cluster[4] & 0xF0) != 0x10)
				{
					break;
				}
				const int64 ClusterBytes = static_cast<int64>(ReadUnsignedBigEndian(Cluster + 4, 4) & 0x0FFFFFFF);
				const int64 ClusterEnd = WholeClustersEnd + 8 + ClusterBytes;
				// A cluster must hold its time, its block header and at least one byte of frame. Anything smaller is a
				// header whose frame never arrived, so the video ends before it.
				if (ClusterBytes <= FrameStartInCluster - 8 || ClusterEnd > FileBytes || Cluster[8] != 0xE7)
				{
					break;
				}
				// Every VP9 frame starts with the two bits 10. A frame whose bytes the disk never wrote (zeros after a
				// power cut) does not, and neither does anything else, so the video ends before it.
				if ((Cluster[FrameStartInCluster] & 0xC0) != 0x80)
				{
					break;
				}
				LastTimecodeMs = static_cast<int64>(ReadUnsignedBigEndian(Cluster + 10, 4));
				WholeClustersEnd = ClusterEnd;
				++Frames;
			}
		}

		if (Frames > 0)
		{
			TArray<uint8> SegmentSize;
			AppendSize(SegmentSize, static_cast<uint64>(WholeClustersEnd - (SegmentSizeOffset + 8)), 8);
			uint8 Duration[8];
			MakeDurationBytes(static_cast<double>(LastTimecodeMs), Duration);
			if (!Handle->Truncate(WholeClustersEnd)
				|| !Handle->Seek(SegmentSizeOffset) || !Handle->Write(SegmentSize.GetData(), SegmentSize.Num())
				|| !Handle->Seek(DurationValueOffset) || !Handle->Write(Duration, 8)
				|| !Handle->Flush())
			{
				OutError = FString::Printf(TEXT("%s could not be finished (is the disk full?)"), *UnfinishedPath);
				return EFlockPlaytestInterruptedVideoResult::CouldNotFinish;
			}
		}
	}

	IFileManager& FileManager = IFileManager::Get();
	if (Frames == 0)
	{
		if (!FileManager.Delete(*UnfinishedPath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true))
		{
			OutError = FString::Printf(TEXT("%s holds no whole frame and could not be deleted"), *UnfinishedPath);
			return EFlockPlaytestInterruptedVideoResult::CouldNotFinish;
		}
		return EFlockPlaytestInterruptedVideoResult::HeldNoFrame;
	}
	if (!FileManager.Move(*FinishedPath, *UnfinishedPath, /*Replace*/ true, /*EvenIfReadOnly*/ true, /*Attributes*/ false, /*bDoNotRetryOrError*/ true))
	{
		OutError = FString::Printf(TEXT("%s could not be renamed to %s"), *UnfinishedPath, *FinishedPath);
		return EFlockPlaytestInterruptedVideoResult::CouldNotFinish;
	}
	OutFramesKept = Frames;
	return EFlockPlaytestInterruptedVideoResult::Finished;
}

FFlockPlaytestVideoFile::~FFlockPlaytestVideoFile()
{
	Close();
}

bool FFlockPlaytestVideoFile::Open(const FString& Path, FIntPoint FrameSize, FString& OutError)
{
	Close();
	BytesWritten = 0;
	FramesWritten = 0;
	LastTimestampMs = -1;

	Archive.Reset(IFileManager::Get().CreateFileWriter(*Path, FILEWRITE_EvenIfReadOnly));
	if (!Archive.IsValid())
	{
		OutError = FString::Printf(TEXT("the file %s could not be created"), *Path);
		return false;
	}

	TArray<uint8> EbmlHeader;
	AppendUnsignedElement(EbmlHeader, IdEbmlVersion, 1, 1);
	AppendUnsignedElement(EbmlHeader, IdEbmlReadVersion, 1, 1);
	AppendUnsignedElement(EbmlHeader, IdEbmlMaxIdLength, 4, 1);
	AppendUnsignedElement(EbmlHeader, IdEbmlMaxSizeLength, 8, 1);
	AppendStringElement(EbmlHeader, IdDocType, "webm");
	AppendUnsignedElement(EbmlHeader, IdDocTypeVersion, 2, 1);
	AppendUnsignedElement(EbmlHeader, IdDocTypeReadVersion, 2, 1);

	TArray<uint8> Info;
	AppendUnsignedElement(Info, IdTimecodeScale, TimecodeScaleNanoseconds, 4);
	AppendStringElement(Info, IdMuxingApp, WrittenBy);
	AppendStringElement(Info, IdWritingApp, WrittenBy);
	// Stamped in when the file is closed, and left at zero by a recording that never was.
	AppendId(Info, IdDuration);
	AppendSize(Info, 8, 1);
	uint8 Duration[8];
	MakeDurationBytes(0.0, Duration);
	Info.Append(Duration, 8);

	TArray<uint8> Video;
	AppendUnsignedElement(Video, IdPixelWidth, static_cast<uint64>(FrameSize.X), 4);
	AppendUnsignedElement(Video, IdPixelHeight, static_cast<uint64>(FrameSize.Y), 4);

	TArray<uint8> TrackEntry;
	AppendUnsignedElement(TrackEntry, IdTrackNumber, 1, 1);
	AppendUnsignedElement(TrackEntry, IdTrackUid, 1, 1);
	AppendUnsignedElement(TrackEntry, IdTrackType, 1, 1);
	AppendStringElement(TrackEntry, IdCodecId, "V_VP9");
	AppendId(TrackEntry, IdVideo);
	AppendSize(TrackEntry, static_cast<uint64>(Video.Num()), 1);
	TrackEntry.Append(Video);

	TArray<uint8> Header;
	AppendId(Header, IdEbmlHeader);
	AppendSize(Header, static_cast<uint64>(EbmlHeader.Num()), 1);
	Header.Append(EbmlHeader);

	AppendId(Header, IdSegment);
	// "Size unknown", which is legal and is what a recording that never closed keeps. Eight bytes wide so the real size
	// fits in the same room when it is stamped in.
	AppendSize(Header, 0x00FFFFFFFFFFFFFFull, 8);

	AppendId(Header, IdInfo);
	AppendSize(Header, static_cast<uint64>(Info.Num()), 4);
	Header.Append(Info);

	AppendId(Header, IdTracks);
	AppendSize(Header, static_cast<uint64>(TrackEntry.Num() + 5), 4);
	AppendId(Header, IdTrackEntry);
	AppendSize(Header, static_cast<uint64>(TrackEntry.Num()), 4);
	Header.Append(TrackEntry);

	// The recovery pass and the size budget both read from fixed offsets, so a header that came out a different length
	// would corrupt files rather than fail. Caught here, loudly, instead.
	if (Header.Num() != FileHeaderBytes)
	{
		OutError = FString::Printf(TEXT("the WebM header came out %d bytes, not the %lld this build expects"),
			Header.Num(), FileHeaderBytes);
		Archive.Reset();
		return false;
	}

	Archive->Serialize(Header.GetData(), Header.Num());
	if (Archive->IsError())
	{
		OutError = FString::Printf(TEXT("the file %s could not be written"), *Path);
		Archive.Reset();
		return false;
	}
	BytesWritten = Header.Num();
	return true;
}

bool FFlockPlaytestVideoFile::WriteFrame(const TArray<uint8>& FrameBytes, int64 TimestampMs, bool bKeyFrame)
{
	if (!Archive.IsValid())
	{
		return false;
	}

	// One cluster per frame: its size is known before it is written, so nothing has to be revisited, and a recording cut
	// off anywhere leaves whole clusters behind it.
	TArray<uint8> ClusterHeader;
	AppendId(ClusterHeader, IdCluster);
	AppendSize(ClusterHeader, static_cast<uint64>(FrameHeaderBytes - 8 + FrameBytes.Num()), 4);
	AppendUnsignedElement(ClusterHeader, IdTimecode, static_cast<uint64>(FMath::Max<int64>(0, TimestampMs)), 4);
	AppendId(ClusterHeader, IdSimpleBlock);
	AppendSize(ClusterHeader, static_cast<uint64>(4 + FrameBytes.Num()), 4);
	// The track this block belongs to, then how far it sits from its cluster's time -- nothing, there being one frame in
	// each -- then whether a player may start decoding here.
	ClusterHeader.Add(0x81);
	ClusterHeader.Add(0x00);
	ClusterHeader.Add(0x00);
	ClusterHeader.Add(bKeyFrame ? 0x80 : 0x00);

	Archive->Serialize(ClusterHeader.GetData(), ClusterHeader.Num());
	Archive->Serialize(const_cast<uint8*>(FrameBytes.GetData()), FrameBytes.Num());
	if (Archive->IsError())
	{
		return false;
	}
	BytesWritten += FrameHeaderBytes + FrameBytes.Num();
	++FramesWritten;
	LastTimestampMs = TimestampMs;
	return true;
}

void FFlockPlaytestVideoFile::AbandonForTesting()
{
	if (Archive.IsValid())
	{
		// The archive's own close saves what was written; ours is what stamps the header, and is skipped on purpose.
		Archive->Close();
		Archive.Reset();
	}
}

bool FFlockPlaytestVideoFile::Close()
{
	if (!Archive.IsValid())
	{
		return true;
	}
	const int64 EndOfFile = Archive->Tell();

	TArray<uint8> SegmentSize;
	AppendSize(SegmentSize, static_cast<uint64>(EndOfFile - (SegmentSizeOffset + 8)), 8);
	Archive->Seek(SegmentSizeOffset);
	Archive->Serialize(SegmentSize.GetData(), SegmentSize.Num());

	uint8 Duration[8];
	MakeDurationBytes(static_cast<double>(FMath::Max<int64>(0, LastTimestampMs)), Duration);
	Archive->Seek(DurationValueOffset);
	Archive->Serialize(Duration, 8);

	Archive->Seek(EndOfFile);
	const bool bSaved = Archive->Close();
	Archive.Reset();
	return bSaved;
}
