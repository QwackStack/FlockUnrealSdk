// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Codegen/FlockCodegenManifest.h"

#include "Codegen/FlockCodegenPaths.h"
#include "Codegen/FlockSchemaHasher.h"
#include "FlockSubsystem.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	const TCHAR* const KeyGameVersionId = TEXT("game_version_id");
	const TCHAR* const KeyContentHash = TEXT("content_hash");
	const TCHAR* const KeyGeneratedAt = TEXT("generated_at_utc");
	const TCHAR* const KeySdkVersion = TEXT("sdk_version");
	const TCHAR* const KeyTemplateCount = TEXT("player_template_count");
	const TCHAR* const KeyConfigCount = TEXT("game_config_count");
	const TCHAR* const KeyShopCount = TEXT("shop_count");
}

FFlockCodegenManifest FFlockCodegenManifest::FromSnapshot(const FFlockSchemaSnapshot& Snapshot)
{
	FFlockCodegenManifest Manifest;
	Manifest.GameVersionId = Snapshot.GameVersionId;
	Manifest.ContentHash = FFlockSchemaHasher::ComputeContentHash(Snapshot);
	Manifest.GeneratedAtUtc = Snapshot.FetchedAtUtc.ToIso8601();
	Manifest.SdkVersion = UFlockSubsystem::SdkVersion;
	Manifest.PlayerTemplateCount = Snapshot.PlayerTemplates.Num();
	Manifest.GameConfigCount = Snapshot.GameConfigs.Num();
	Manifest.ShopCount = Snapshot.Shops.Num();
	return Manifest;
}

FString FFlockCodegenManifest::ToJson() const
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(KeyGameVersionId, GameVersionId);
	Object->SetStringField(KeyContentHash, ContentHash);
	Object->SetStringField(KeyGeneratedAt, GeneratedAtUtc);
	Object->SetStringField(KeySdkVersion, SdkVersion);
	Object->SetNumberField(KeyTemplateCount, PlayerTemplateCount);
	Object->SetNumberField(KeyConfigCount, GameConfigCount);
	Object->SetNumberField(KeyShopCount, ShopCount);

	// Pretty-printed on purpose: this file is committed alongside generated code, so a human reads it in
	// a diff. It is small enough that the formatting costs nothing.
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Object, Writer);
	return Json;
}

bool FFlockCodegenManifest::TryParse(const FString& Json, FFlockCodegenManifest& OutManifest)
{
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return false;
	}

	// A manifest with no version id is not a manifest — treating it as one would report "Current" against
	// a snapshot that also failed to resolve, which is the one wrong answer this must never give.
	FString GameVersionId;
	if (!Object->TryGetStringField(KeyGameVersionId, GameVersionId) || GameVersionId.IsEmpty())
	{
		return false;
	}

	OutManifest = FFlockCodegenManifest();
	OutManifest.GameVersionId = GameVersionId;
	Object->TryGetStringField(KeyContentHash, OutManifest.ContentHash);
	Object->TryGetStringField(KeyGeneratedAt, OutManifest.GeneratedAtUtc);
	Object->TryGetStringField(KeySdkVersion, OutManifest.SdkVersion);
	Object->TryGetNumberField(KeyTemplateCount, OutManifest.PlayerTemplateCount);
	Object->TryGetNumberField(KeyConfigCount, OutManifest.GameConfigCount);
	Object->TryGetNumberField(KeyShopCount, OutManifest.ShopCount);
	return true;
}

bool FFlockCodegenManifest::Write(const FString& GeneratedRoot, const FFlockCodegenManifest& Manifest, FString& OutError)
{
	if (GeneratedRoot.IsEmpty())
	{
		OutError = TEXT("No generated root to write the manifest into.");
		return false;
	}
	if (!IFileManager::Get().MakeDirectory(*GeneratedRoot, /*Tree*/ true))
	{
		// MakeDirectory returns false when the directory already exists, so only an actually-absent one
		// is a failure.
		if (!IFileManager::Get().DirectoryExists(*GeneratedRoot))
		{
			OutError = FString::Printf(TEXT("Could not create the generated folder '%s'."), *GeneratedRoot);
			return false;
		}
	}

	const FString Path = FFlockCodegenPaths::ManifestPath(GeneratedRoot);
	if (!FFileHelper::SaveStringToFile(Manifest.ToJson(), *Path))
	{
		OutError = FString::Printf(TEXT("Could not write the manifest to '%s'."), *Path);
		return false;
	}
	OutError.Reset();
	return true;
}

bool FFlockCodegenManifest::TryRead(const FString& GeneratedRoot, FFlockCodegenManifest& OutManifest)
{
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *FFlockCodegenPaths::ManifestPath(GeneratedRoot)))
	{
		return false;
	}
	return TryParse(Json, OutManifest);
}

EFlockCodegenDrift FFlockCodegenManifest::Compare(const FFlockCodegenManifest& Stored, const FFlockSchemaSnapshot& Fresh)
{
	if (Stored.GameVersionId.IsEmpty())
	{
		return EFlockCodegenDrift::NeverGenerated;
	}
	if (Stored.GameVersionId != Fresh.GameVersionId)
	{
		return EFlockCodegenDrift::VersionChanged;
	}
	// A manifest from before content hashing (or one hand-edited to drop the field) cannot be verified,
	// and "cannot verify" has to read as drift — the alternative is silently claiming Current.
	if (Stored.ContentHash.IsEmpty())
	{
		return EFlockCodegenDrift::ContentChanged;
	}
	if (Stored.ContentHash != FFlockSchemaHasher::ComputeContentHash(Fresh))
	{
		return EFlockCodegenDrift::ContentChanged;
	}
	return EFlockCodegenDrift::Current;
}

FString FFlockCodegenManifest::Describe(EFlockCodegenDrift Drift, const FFlockCodegenManifest& Stored,
	const FFlockSchemaSnapshot& Fresh)
{
	switch (Drift)
	{
	case EFlockCodegenDrift::Current:
		return FString::Printf(
			TEXT("Generated code is up to date with game_version_id='%s'."), *Fresh.GameVersionId);
	case EFlockCodegenDrift::NeverGenerated:
		return TEXT("No generated schemas found. Run Flock > Sync Schemas.");
	case EFlockCodegenDrift::VersionChanged:
		return FString::Printf(
			TEXT("Generated code is for game_version_id='%s' but the backend now reports '%s'. Re-run Flock > Sync Schemas."),
			*Stored.GameVersionId, *Fresh.GameVersionId);
	case EFlockCodegenDrift::ContentChanged:
		return FString::Printf(
			TEXT("The schemas for game_version_id='%s' changed since the code was generated (fields, types, tags, shop items, or currencies). Re-run Flock > Sync Schemas."),
			*Fresh.GameVersionId);
	default:
		return FString();
	}
}
