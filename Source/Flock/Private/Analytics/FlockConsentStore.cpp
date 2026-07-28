// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Analytics/FlockConsentStore.h"

#include "Dom/JsonObject.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	const TCHAR* GrantedField = TEXT("granted");
	const TCHAR* DecidedAtField = TEXT("decided_at");
}

FFlockConsentStore::FFlockConsentStore(const FString& InFilePath)
	: FilePath(InFilePath.IsEmpty() ? DefaultPath() : InFilePath)
{
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *FilePath))
	{
		return;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		// Corrupt file: treat as no decision rather than failing construction.
		return;
	}

	bool bStored = false;
	if (Root->TryGetBoolField(GrantedField, bStored))
	{
		bHasDecision = true;
		bGrantedValue = bStored;
	}
}

FString FFlockConsentStore::DefaultPath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Flock"), TEXT("analytics"), TEXT("consent.json"));
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

	// Log-and-continue: a consent write failing must not take the game down.
	FFileHelper::SaveStringToFile(Json, *FilePath);
}

void FFlockConsentStore::Clear()
{
	bHasDecision = false;
	bGrantedValue = false;
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*FilePath);
}

bool FFlockConsentStore::ResolveEffective(bool bRequireExplicitConsent) const
{
	if (bHasDecision)
	{
		return bGrantedValue;
	}
	return !bRequireExplicitConsent;
}
