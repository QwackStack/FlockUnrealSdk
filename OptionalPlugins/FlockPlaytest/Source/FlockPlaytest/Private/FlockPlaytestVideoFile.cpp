// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockPlaytestVideoFile.h"

#include "HAL/FileManager.h"
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
