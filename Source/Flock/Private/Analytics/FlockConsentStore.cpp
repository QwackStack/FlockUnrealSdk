// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Analytics/FlockConsentStore.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/FlockTemporaryFiles.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	const TCHAR* GrantedField = TEXT("granted");
	const TCHAR* DecidedAtField = TEXT("decided_at");

	/** False when the file is missing, unreadable, corrupt, or holds no decision. */
	bool ReadConsentDecision(const FString& Path, bool& OutGranted)
	{
		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *Path))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		return FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid() && Root->TryGetBoolField(GrantedField, OutGranted);
	}
}

FFlockConsentStore::FFlockConsentStore(const FString& InFilePath)
	: FilePath(InFilePath.IsEmpty() ? DefaultPath() : InFilePath)
{
	bool bStored = false;
	bool bDecisionLivesInItsTemporaryFile = false;
	if (ReadConsentDecision(FilePath, bStored))
	{
		bHasDecision = true;
		bGrantedValue = bStored;
	}
	else if (!IFileManager::Get().FileExists(*FilePath))
	{
		// A save cut off between the old file's delete and the new one's move leaves the new decision whole in its temporary
		// file. The newest one that reads back whole is the latest decision, so it is kept and moved into place.
		for (const FString& TemporaryFile : FFlockTemporaryFiles::FindTemporaryFilesOf(FilePath))
		{
			if (ReadConsentDecision(TemporaryFile, bStored))
			{
				bHasDecision = true;
				bGrantedValue = bStored;
				// One that cannot be moved into place stays where it is: it holds the only copy of the decision, and the sweep
				// below would take it.
				bDecisionLivesInItsTemporaryFile = !FFlockTemporaryFiles::MoveIntoPlace(TemporaryFile, FilePath);
				break;
			}
		}
	}
	// A corrupt file reads as no decision rather than failing construction; the temporary files a crash left go.
	if (!bDecisionLivesInItsTemporaryFile)
	{
		FFlockTemporaryFiles::DeleteLeftOverFilesOf(FilePath);
	}
}

FString FFlockConsentStore::DefaultPath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Flock"), TEXT("analytics"), FileName);
}

bool FFlockConsentStore::Load(bool& OutGranted) const
{
	if (!bHasDecision)
	{
		return false;
	}
	OutGranted = bGrantedValue;
	return true;
}

void FFlockConsentStore::Save(bool bGranted)
{
	bHasDecision = true;
	bGrantedValue = bGranted;

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(GrantedField, bGranted);
	Root->SetStringField(DecidedAtField, FDateTime::UtcNow().ToIso8601());

	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Root, Writer);

	// Log-and-continue: a consent write failing must not take the game down. Through a temporary file, so a crash mid-write
	// never leaves a torn file, which would read as no decision and collect again from a player who had opted out.
	FFlockTemporaryFiles::SaveThenMove(Json, FilePath);
}

void FFlockConsentStore::Clear()
{
	bHasDecision = false;
	bGrantedValue = false;
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*FilePath);
	// Every temporary file goes with it, however fresh: a decision saved moments ago must not come back after the player
	// asked to erase it.
	FFlockTemporaryFiles::DeleteTemporaryFilesOf(FilePath);
}

bool FFlockConsentStore::ResolveEffective(bool bRequireExplicitConsent) const
{
	if (bHasDecision)
	{
		return bGrantedValue;
	}
	return !bRequireExplicitConsent;
}
