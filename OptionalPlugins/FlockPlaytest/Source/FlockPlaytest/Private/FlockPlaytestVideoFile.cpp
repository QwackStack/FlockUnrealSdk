// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockPlaytestVideoFile.h"

#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Serialization/Archive.h"

namespace
{
	/** Where the header keeps the frame count. */
	constexpr int64 FrameCountOffset = 24;

	void AppendLittleEndian(TArray<uint8>& Bytes, uint64 Value, int32 ByteCount)
	{
		for (int32 Index = 0; Index < ByteCount; ++Index)
		{
			Bytes.Add(static_cast<uint8>((Value >> (8 * Index)) & 0xFF));
		}
	}

	uint64 ReadVideoFileLittleEndian(const uint8* Bytes, int32 ByteCount)
	{
		uint64 Value = 0;
		for (int32 Index = 0; Index < ByteCount; ++Index)
		{
			Value |= static_cast<uint64>(Bytes[Index]) << (8 * Index);
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
		uint8 FileHeader[FileHeaderBytes];
		int64 WholeFramesEnd = 0;
		if (FileBytes >= FileHeaderBytes && Handle->Seek(0) && Handle->Read(FileHeader, FileHeaderBytes)
			&& FileHeader[0] == 'D' && FileHeader[1] == 'K' && FileHeader[2] == 'I' && FileHeader[3] == 'F')
		{
			WholeFramesEnd = FileHeaderBytes;
			uint8 FrameHeader[FrameHeaderBytes];
			while (WholeFramesEnd + FrameHeaderBytes <= FileBytes && Handle->Seek(WholeFramesEnd) && Handle->Read(FrameHeader, FrameHeaderBytes))
			{
				const int64 FrameBytes = static_cast<int64>(ReadVideoFileLittleEndian(FrameHeader, 4));
				// The encoder never hands over an empty frame, so a size of zero is where the writing stopped.
				if (FrameBytes <= 0 || WholeFramesEnd + FrameHeaderBytes + FrameBytes > FileBytes)
				{
					break;
				}
				// Every VP9 frame starts with the two bits 10. A frame whose bytes the disk never wrote (zeros after a power
				// cut) does not, and neither does anything else, so the video ends before it.
				uint8 FirstFrameByte = 0;
				if (!Handle->Read(&FirstFrameByte, 1) || (FirstFrameByte & 0xC0) != 0x80)
				{
					break;
				}
				WholeFramesEnd += FrameHeaderBytes + FrameBytes;
				++Frames;
			}
		}

		if (Frames > 0)
		{
			TArray<uint8> FrameCount;
			AppendLittleEndian(FrameCount, static_cast<uint64>(Frames), 4);
			if (!Handle->Truncate(WholeFramesEnd) || !Handle->Seek(FrameCountOffset) || !Handle->Write(FrameCount.GetData(), FrameCount.Num())
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

	TArray<uint8> Header;
	Header.Append({ 'D', 'K', 'I', 'F' });
	AppendLittleEndian(Header, 0, 2);
	AppendLittleEndian(Header, static_cast<uint64>(FileHeaderBytes), 2);
	Header.Append({ 'V', 'P', '9', '0' });
	AppendLittleEndian(Header, static_cast<uint64>(FrameSize.X), 2);
	AppendLittleEndian(Header, static_cast<uint64>(FrameSize.Y), 2);
	// Frame times are in milliseconds: a time base of 1/1000, written denominator first.
	AppendLittleEndian(Header, 1000, 4);
	AppendLittleEndian(Header, 1, 4);
	AppendLittleEndian(Header, 0, 4);
	AppendLittleEndian(Header, 0, 4);

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

bool FFlockPlaytestVideoFile::WriteFrame(const TArray<uint8>& FrameBytes, int64 TimestampMs)
{
	if (!Archive.IsValid())
	{
		return false;
	}
	TArray<uint8> FrameHeader;
	AppendLittleEndian(FrameHeader, static_cast<uint64>(FrameBytes.Num()), 4);
	AppendLittleEndian(FrameHeader, static_cast<uint64>(TimestampMs), 8);
	Archive->Serialize(FrameHeader.GetData(), FrameHeader.Num());
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

bool FFlockPlaytestVideoFile::Close()
{
	if (!Archive.IsValid())
	{
		return true;
	}
	TArray<uint8> FrameCount;
	AppendLittleEndian(FrameCount, static_cast<uint64>(FramesWritten), 4);
	const int64 EndOfFile = Archive->Tell();
	Archive->Seek(FrameCountOffset);
	Archive->Serialize(FrameCount.GetData(), FrameCount.Num());
	Archive->Seek(EndOfFile);
	const bool bSaved = Archive->Close();
	Archive.Reset();
	return bSaved;
}
