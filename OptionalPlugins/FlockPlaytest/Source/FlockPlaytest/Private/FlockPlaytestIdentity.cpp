// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockPlaytestIdentity.h"

#include "FlockPlaytestText.h"
#include "HAL/FileManager.h"
#include "Interfaces/OnlineIdentityInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemNames.h"

namespace
{
	/** The one form a device id takes. The file is refused when it holds anything else, surrounding whitespace included. */
	bool IsDeviceId(const FString& Text)
	{
		FGuid Guid;
		return FGuid::Parse(Text, Guid) && Guid.IsValid()
			&& Guid.ToString(EGuidFormats::DigitsWithHyphensLower).Equals(Text, ESearchCase::CaseSensitive);
	}
}

FFlockRunningSteamAccount ReadRunningSteamAccount()
{
	FFlockRunningSteamAccount Account;

	// Asking for a subsystem by name creates it when it does not exist, which would try to load and start Steam. So
	// only an instance something else already created is read.
	if (!IOnlineSubsystem::DoesInstanceExist(STEAM_SUBSYSTEM))
	{
		return Account;
	}
	const IOnlineSubsystem* Steam = IOnlineSubsystem::Get(STEAM_SUBSYSTEM);
	const IOnlineIdentityPtr Identity = Steam != nullptr ? Steam->GetIdentityInterface() : nullptr;
	if (!Identity.IsValid())
	{
		return Account;
	}
	const FUniqueNetIdPtr UserId = Identity->GetUniquePlayerId(0);
	if (UserId.IsValid() && UserId->IsValid())
	{
		Account.Id = UserId->ToString();
		Account.Nickname = Identity->GetPlayerNickname(0);
	}
	return Account;
}

bool IsUsablePlaytestId(const FString& Id, int32 MaxLength)
{
	return !Id.IsEmpty() && Id.Len() <= MaxLength && !FlockPlaytestText::ContainsWhitespace(Id);
}

FFlockPlaytestDeviceIdFile::FFlockPlaytestDeviceIdFile(const FString& InPath)
	: Path(InPath)
{
}

FString FFlockPlaytestDeviceIdFile::GetDefaultPath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FlockPlaytest"), TEXT("device_id.txt"));
}

EFlockDeviceIdFileResult FFlockPlaytestDeviceIdFile::ReadOrCreate(FString& OutDeviceId) const
{
	OutDeviceId.Empty();
	SweepStrayTemporaryFiles();

	if (!IFileManager::Get().FileExists(*Path))
	{
		return SaveNewDeviceId(OutDeviceId) ? EFlockDeviceIdFileResult::Created : EFlockDeviceIdFileResult::CouldNotSave;
	}

	FString Contents;
	if (!FFileHelper::LoadFileToString(Contents, *Path))
	{
		// It may be locked for a moment by something else. Replacing it would change this install's id for good.
		return EFlockDeviceIdFileResult::Unreadable;
	}
	if (IsDeviceId(Contents))
	{
		OutDeviceId = Contents;
		return EFlockDeviceIdFileResult::Read;
	}
	return SaveNewDeviceId(OutDeviceId) ? EFlockDeviceIdFileResult::Replaced : EFlockDeviceIdFileResult::CouldNotSave;
}

bool FFlockPlaytestDeviceIdFile::SaveNewDeviceId(FString& OutDeviceId) const
{
	// Written next to the file and moved over it, so a crash part-way through never leaves half an id behind. The move
	// gives up at once when it fails: the file manager would otherwise retry for seconds on the game thread and log an
	// error, and a failure here already has its own answer.
	const FString TemporaryPath = FString::Printf(TEXT("%s.%s.tmp"), *Path, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString NewDeviceId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	const bool bSaved = FFileHelper::SaveStringToFile(NewDeviceId, *TemporaryPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		&& IFileManager::Get().Move(*Path, *TemporaryPath, /*bReplace*/ true, /*bEvenIfReadOnly*/ false, /*bAttributes*/ false,
			/*bDoNotRetryOrError*/ true);
	if (!bSaved)
	{
		IFileManager::Get().Delete(*TemporaryPath, /*bRequireExists*/ false, /*bEvenReadOnly*/ false, /*bQuiet*/ true);
		return false;
	}

	// Whatever the file holds after the move is the id used. When another launch moves its own id in first, this
	// launch takes that one; a launch whose move lands last still keeps its own for this run.
	FString Contents;
	if (!FFileHelper::LoadFileToString(Contents, *Path) || !IsDeviceId(Contents))
	{
		return false;
	}
	OutDeviceId = Contents;
	return true;
}

void FFlockPlaytestDeviceIdFile::SweepStrayTemporaryFiles() const
{
	// A launch that crashed between writing a new id and moving it into place leaves its temporary file behind. A fresh
	// one may belong to a launch saving right now, so only files older than a minute go.
	TArray<FString> Found;
	IFileManager::Get().FindFiles(Found, *(Path + TEXT(".*.tmp")), /*bFiles*/ true, /*bDirectories*/ false);
	const FString Folder = FPaths::GetPath(Path);
	const FDateTime OldestKept = FDateTime::UtcNow() - FTimespan::FromMinutes(1.0);
	for (const FString& Name : Found)
	{
		const FString Stray = FPaths::Combine(Folder, Name);
		if (IFileManager::Get().GetTimeStamp(*Stray) < OldestKept)
		{
			IFileManager::Get().Delete(*Stray, /*bRequireExists*/ false, /*bEvenReadOnly*/ false, /*bQuiet*/ true);
		}
	}
}
