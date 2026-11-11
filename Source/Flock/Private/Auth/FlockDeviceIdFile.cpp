// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Auth/FlockDeviceIdFile.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/FlockTemporaryFiles.h"
#include "Misc/FlockWhiteSpace.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

const TCHAR* const FFlockDeviceIdFile::FileName = TEXT("device_id.txt");

namespace
{
	/** A file this young that is empty, holds the start of an id or is held may be another launch's save under way, so it is read again. */
	constexpr double YoungFileSeconds = 5.0;
	constexpr double LongestWaitSeconds = 1.0;
	constexpr int32 Attempts = 4;

	/** Read and written on the game thread only. */
	TFunction<void()>& BeforeNextSave()
	{
		static TFunction<void()> Hook;
		return Hook;
	}

	void RunBeforeNextSave()
	{
		if (BeforeNextSave())
		{
			// Cleared before it runs, so a save the hook itself makes goes straight through.
			const TFunction<void()> Hook = MoveTemp(BeforeNextSave());
			BeforeNextSave() = nullptr;
			Hook();
		}
	}

	bool IsTrimmed(TCHAR Character)
	{
		return FlockWhiteSpace::IsWhiteSpace(Character) || Character == static_cast<TCHAR>(0xFEFF);
	}

	FString Trimmed(const FString& Text)
	{
		int32 Start = 0;
		int32 End = Text.Len();
		while (Start < End && IsTrimmed(Text[Start]))
		{
			++Start;
		}
		while (End > Start && IsTrimmed(Text[End - 1]))
		{
			--End;
		}
		return Text.Mid(Start, End - Start);
	}

	/** The forms a GUID is written in: 32 digits, with hyphens, in braces or parentheses, or as hex values in braces. */
	bool IsAGuid(const FString& Text)
	{
		FGuid Guid;
		switch (Text.Len())
		{
		case 32:
			return FGuid::ParseExact(Text, EGuidFormats::Digits, Guid);
		case 36:
			return FGuid::ParseExact(Text, EGuidFormats::DigitsWithHyphens, Guid);
		case 38:
			return FGuid::ParseExact(Text, Text.StartsWith(TEXT("{"), ESearchCase::CaseSensitive)
				? EGuidFormats::DigitsWithHyphensInBraces : EGuidFormats::DigitsWithHyphensInParentheses, Guid);
		case 68:
			return FGuid::ParseExact(Text, EGuidFormats::HexValuesInBraces, Guid);
		default:
			return false;
		}
	}

	/** Empty, or the start of an id written so far. */
	bool CouldBePartOfAnId(const FString& Contents)
	{
		const FString Text = Trimmed(Contents);
		if (Text.Len() >= 36)
		{
			return false;
		}
		for (const TCHAR Character : Text)
		{
			if (!FChar::IsHexDigit(Character) && Character != TEXT('-'))
			{
				return false;
			}
		}
		return true;
	}

	bool IsYoung(const FString& Path)
	{
		const FDateTime Written = IFileManager::Get().GetTimeStamp(*Path);
		return Written != FDateTime::MinValue() && (FDateTime::UtcNow() - Written).GetTotalSeconds() < YoungFileSeconds;
	}

	/** The system's own words for why the file call just made failed, read before any other call can change them. */
	FString LastSystemFailure()
	{
		const uint32 Error = FPlatformMisc::GetLastError();
		if (Error == 0)
		{
			return TEXT("the system gave no reason");
		}
		TCHAR Message[512] = { 0 };
		FPlatformMisc::GetSystemErrorMessage(Message, UE_ARRAY_COUNT(Message), static_cast<int32>(Error));
		const FString Reason = FString(Message).TrimStartAndEnd();
		return Reason.IsEmpty() ? FString::Printf(TEXT("system error %u"), Error) : Reason;
	}

	/**
	 * The id the file holds; empty when there is no file or it holds no GUID. bOutUnreadable: it is there but cannot be read,
	 * and OutFailure says why in the system's words.
	 */
	FString ReadWhenSettled(const FString& Path, bool& bOutUnreadable, FString& OutFailure)
	{
		IFileManager& Files = IFileManager::Get();
		const double GiveUpAt = FPlatformTime::Seconds() + LongestWaitSeconds;
		while (true)
		{
			bOutUnreadable = false;
			OutFailure.Reset();
			if (!Files.FileExists(*Path))
			{
				return FString();
			}
			FString Contents;
			if (FFileHelper::LoadFileToString(Contents, *Path))
			{
				const FString Id = FFlockDeviceIdFile::IdIn(Contents);
				if (!Id.IsEmpty())
				{
					return Id;
				}
			}
			else
			{
				const FString Failure = LastSystemFailure();
				if (!Files.FileExists(*Path))
				{
					return FString();
				}
				bOutUnreadable = true;
				OutFailure = Failure;
			}
			const bool bMayBeUnderWay = bOutUnreadable || CouldBePartOfAnId(Contents);
			if (!bMayBeUnderWay || FPlatformTime::Seconds() >= GiveUpAt || !IsYoung(Path))
			{
				return FString();
			}
			FPlatformProcess::Sleep(0.02f);
		}
	}

	/** Made new, never over a file that is there: of two launches making it at once, exactly one does. */
	bool TryCreate(const FString& Path, const FString& Id)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
		RunBeforeNextSave();
		return FFlockTemporaryFiles::SaveAsNew(Id, Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	/**
	 * Moves a file holding no GUID out of the way. When what was moved holds an id after all (another launch made the file in
	 * between), that file is put back.
	 */
	bool MoveAside(const FString& Path)
	{
		IFileManager& Files = IFileManager::Get();
		const FString Aside = FFlockTemporaryFiles::MakePath(Path);
		RunBeforeNextSave();
		if (!Files.Move(*Aside, *Path, /*bReplace*/ false, /*bEvenIfReadOnly*/ true, /*bAttributes*/ false, /*bDoNotRetryOrError*/ true))
		{
			return false;
		}
		bool bUnreadable = false;
		FString WhyUnreadable;
		if (ReadWhenSettled(Aside, bUnreadable, WhyUnreadable).IsEmpty())
		{
			Files.Delete(*Aside, /*bRequireExists*/ false, /*bEvenReadOnly*/ true, /*bQuiet*/ true);
		}
		else
		{
			// A put-back that fails leaves the moved file to the sweep; whoever made the file meanwhile is read next.
			FFlockTemporaryFiles::MoveIntoPlaceIfNone(Aside, Path);
		}
		return true;
	}

	FString ReadOrMake(const FString& Path, const TFunction<void(const FString&)>& Warn)
	{
		IFileManager& Files = IFileManager::Get();
		// A launch that ended between moving a file aside and deleting it leaves it; a fresh one may be another launch's under way.
		FFlockTemporaryFiles::DeleteLeftOverFilesOf(Path);
		const FString NewId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		FString Failure;
		for (int32 Attempt = 0; Attempt < Attempts; ++Attempt)
		{
			bool bUnreadable = false;
			FString ReadFailure;
			const FString Found = ReadWhenSettled(Path, bUnreadable, ReadFailure);
			if (!Found.IsEmpty())
			{
				return Found;
			}
			if (bUnreadable)
			{
				// It may be held by something else, and replacing it would change this install's id for good.
				if (Warn)
				{
					Warn(FString::Printf(TEXT("The device id file %s is there but could not be read (%s). This start of the game uses a new ")
						TEXT("device id that is not saved, so a device sign-in now is not this device's usual player. Close whatever ")
						TEXT("holds the file (another copy of the game, a backup or antivirus tool) and start the game again."), *Path, *ReadFailure));
				}
				return NewId;
			}
			if (Files.FileExists(*Path))
			{
				if (!MoveAside(Path))
				{
					Failure = TEXT("the file holding no device id could not be moved aside");
				}
				continue;
			}
			if (TryCreate(Path, NewId))
			{
				return NewId;
			}
			// Still no file: nothing can be saved there. Otherwise another launch made it first, and the next pass reads it.
			if (!Files.FileExists(*Path))
			{
				Failure = TEXT("nothing could be written in that folder");
				break;
			}
		}
		if (Warn)
		{
			Warn(FString::Printf(TEXT("A new device id could not be saved to %s (%s). This start of the game uses it until the SDK ")
				TEXT("shuts down and the next start makes another, so each start signs in as a new player. Let the game write to ")
				TEXT("that folder, or pass a device id of your own to the device sign-in calls."),
				*Path, Failure.IsEmpty() ? TEXT("the file kept changing under another copy of the game") : *Failure));
		}
		return NewId;
	}
}

FFlockDeviceIdFile::FFlockDeviceIdFile(const FString& Folder)
	: Path(FPaths::Combine(Folder, FileName))
{
}

FString FFlockDeviceIdFile::Read(const TFunction<void(const FString&)>& Warn)
{
	if (Id.IsEmpty())
	{
		Id = ReadOrMake(Path, Warn);
	}
	return Id;
}

FString FFlockDeviceIdFile::IdIn(const FString& Text)
{
	const FString Id = Trimmed(Text);
	return IsAGuid(Id) ? Id : FString();
}

void FFlockDeviceIdFile::SetBeforeNextSaveForTesting(TFunction<void()> Hook)
{
	check(IsInGameThread());
	BeforeNextSave() = MoveTemp(Hook);
}
