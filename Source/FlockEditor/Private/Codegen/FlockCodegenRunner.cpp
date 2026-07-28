// Copyright 2022, Qwacks. All Rights Reserved.

#include "Codegen/FlockCodegenRunner.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Codegen/FlockCatalogBuilder.h"
#include "Codegen/FlockCodegenPaths.h"
#include "Codegen/FlockEnumEmitter.h"
#include "Codegen/FlockFunctionLibraryEmitter.h"
#include "Codegen/FlockMacroLibraryEmitter.h"
#include "Codegen/FlockSchemaFetcher.h"
#include "Codegen/FlockStructEmitter.h"
#include "Config/FlockConfig.h"
#include "Engine/UserDefinedEnum.h"
#include "FlockEditor.h"
#include "HAL/FileManager.h"
#include "ObjectTools.h"

FString FFlockCodegenRunner::FRunResult::Describe() const
{
	if (!bSucceeded)
	{
		return FString::Printf(TEXT("Flock schema sync failed: %s"), *Error);
	}
	return FString::Printf(
		TEXT("Flock schemas synced for %s — %d template(s), %d config(s), %d shop(s) -> %d struct(s), %d enum(s), %d function(s), %d macro(s)%s"),
		*GameVersionId, TemplateCount, ConfigCount, ShopCount, StructCount, EnumCount, FunctionCount, MacroCount,
		Warnings.Num() > 0 ? *FString::Printf(TEXT(" (%d warning(s))"), Warnings.Num()) : TEXT(""));
}

FFlockCodegenRunner::FRunResult FFlockCodegenRunner::EmitAll(const FFlockSchemaSnapshot& Snapshot,
	const FString& ContentPath, const FString& GeneratedCodeRoot)
{
	FRunResult Result;
	Result.GameVersionId = Snapshot.GameVersionId;
	Result.TemplateCount = Snapshot.PlayerTemplates.Num();
	Result.ConfigCount = Snapshot.GameConfigs.Num();
	Result.ShopCount = Snapshot.Shops.Num();
	Result.bBakeStale = Snapshot.IsBakeStale();

	// The catalog first: it is the browsable record of what this sync saw, and is useful even if a later
	// emitter has trouble.
	FString CatalogError;
	if (!FFlockCatalogBuilder::Save(Snapshot, ContentPath, CatalogError))
	{
		Result.Warnings.Add(FString::Printf(TEXT("Content catalog: %s"), *CatalogError));
	}

	// Structs before the library: the library bakes struct *types* into its pins, which a name cannot
	// supply.
	FString StructError;
	const FFlockStructEmitter::FEmitResult Structs =
		FFlockStructEmitter::Emit(Snapshot, ContentPath, StructError);
	Result.Warnings.Append(Structs.Warnings);
	Result.StructCount = Structs.StructCount;
	if (!StructError.IsEmpty())
	{
		Result.Warnings.Add(StructError);
	}

	FString EnumError;
	const FFlockEnumEmitter::FEmitResult Enums = FFlockEnumEmitter::Emit(Snapshot, ContentPath, EnumError);
	Result.Warnings.Append(Enums.Warnings);
	Result.EnumCount = Enums.EnumCount();
	if (!EnumError.IsEmpty())
	{
		Result.Warnings.Add(EnumError);
	}

	// Each emitted enum becomes a lookup turning a picked member into the string the SDK sends. The
	// mapping has to travel from the enum emitter rather than be re-derived: a member's display name is
	// not its wire value, and the enum itself does not record which is which.
	TArray<FFlockEnumLookupSpec> Lookups;
	auto AddLookup = [&Lookups](const TCHAR* FunctionName, const FFlockEnumEmitter::FEnumResult& Emitted)
	{
		if (Emitted.IsValid())
		{
			FFlockEnumLookupSpec Spec;
			Spec.FunctionName = FunctionName;
			Spec.Enum = Emitted.Enum;
			Spec.Members = Emitted.WireValueByDisplayName;
			Lookups.Add(MoveTemp(Spec));
		}
	};
	AddLookup(TEXT("ShopItemId"), Enums.ShopItems);
	AddLookup(TEXT("CurrencyName"), Enums.Currencies);
	AddLookup(TEXT("AchievementName"), Enums.Achievements);

	FString LibraryError;
	const FFlockFunctionLibraryEmitter::FEmitResult Library = FFlockFunctionLibraryEmitter::Emit(
		Snapshot, ContentPath, Structs.StructById, Lookups, LibraryError);
	Result.Warnings.Append(Library.Warnings);
	Result.FunctionCount = Library.FunctionCount;
	if (!LibraryError.IsEmpty())
	{
		Result.Warnings.Add(LibraryError);
	}

	// The macro library last of the emitters: it wraps the generated functions, so it needs the function
	// library compiled first. This is the one-node surface — `Get Gameplay`, `Save Wallet`, `Purchase` —
	// and the reason the rest of the tier exists.
	FString MacroError;
	const FFlockMacroLibraryEmitter::FEmitResult Macros = FFlockMacroLibraryEmitter::Emit(
		Snapshot, ContentPath, Library, MacroError);
	Result.Warnings.Append(Macros.Warnings);
	Result.MacroCount = Macros.MacroCount;
	if (!MacroError.IsEmpty())
	{
		Result.Warnings.Add(MacroError);
	}

	// The manifest last, and only if something was actually emitted: it is the claim that generated output
	// exists and matches this schema, so writing it after a failed run would make a later drift check
	// report "up to date" about assets that were never written.
	if (Result.StructCount == 0 && Result.EnumCount == 0 && Result.FunctionCount == 0)
	{
		Result.bSucceeded = false;
		Result.Error = TEXT("Nothing was generated. Check the warnings, or whether this game version declares any templates, configs, or shops.");
		return Result;
	}

	FString ManifestError;
	if (!FFlockCodegenManifest::Write(GeneratedCodeRoot, FFlockCodegenManifest::FromSnapshot(Snapshot), ManifestError))
	{
		// Fatal: without a manifest nothing can tell whether the generated code is current, which is worse
		// than a failed sync because it is silent.
		Result.bSucceeded = false;
		Result.Error = ManifestError;
		return Result;
	}

	Result.bSucceeded = true;
	return Result;
}

void FFlockCodegenRunner::Sync(FOnSyncComplete OnComplete)
{
	const UFlockConfig* Config = GetDefault<UFlockConfig>();
	const FString ContentPath = Config ? Config->GeneratedContentPath : FString();

	FString GeneratedCodeRoot;
	FString PathError;
	if (!FFlockCodegenPaths::TryResolveGeneratedRootFromSettings(GeneratedCodeRoot, PathError))
	{
		FRunResult Failed;
		Failed.Error = PathError;
		OnComplete.ExecuteIfBound(Failed);
		return;
	}

	UE_LOG(LogFlockEditor, Log, TEXT("Flock: syncing schemas from the backend."));

	FFlockSchemaFetcher::FetchFromSettings(FFlockSchemaFetcher::FOnSchemaFetched::CreateLambda(
		[OnComplete, ContentPath, GeneratedCodeRoot](TFlockResult<FFlockSchemaSnapshot> Fetched)
		{
			if (!Fetched.bSuccess)
			{
				// Nothing is written, so whatever was generated before is still intact and still correct
				// for the version it was generated against.
				FRunResult Failed;
				Failed.Error = Fetched.Error.Message;
				OnComplete.ExecuteIfBound(Failed);
				return;
			}

			const FRunResult Result = EmitAll(Fetched.Value, ContentPath, GeneratedCodeRoot);
			for (const FString& Warning : Result.Warnings)
			{
				UE_LOG(LogFlockEditor, Warning, TEXT("Flock codegen: %s"), *Warning);
			}
			if (Result.bBakeStale)
			{
				UE_LOG(LogFlockEditor, Warning,
					TEXT("Flock codegen: the backend resolved game version '%s' but project settings have '%s' baked. ")
					TEXT("Run Tools > Flock > Resolve Game Version so the game runs against what was generated."),
					*Fetched.Value.GameVersionId, *Fetched.Value.BakedGameVersionId);
			}
			OnComplete.ExecuteIfBound(Result);
		}));
}

FString FFlockCodegenRunner::FCleanResult::Describe() const
{
	if (!bSucceeded)
	{
		return FString::Printf(TEXT("Flock clean failed: %s"), *Error);
	}
	return FString::Printf(TEXT("Flock generated content cleaned — %d asset(s) deleted%s"),
		AssetsDeleted, bManifestRemoved ? TEXT(", manifest removed") : TEXT(""));
}

FFlockCodegenRunner::FCleanResult FFlockCodegenRunner::Clean(bool bShowConfirmation)
{
	const UFlockConfig* Config = GetDefault<UFlockConfig>();

	// Resolved here rather than inside CleanAt so a bad *code* path cannot stop the assets being removed:
	// the two settings are independent, and a project with a broken GeneratedCodePath should still be able
	// to clean its content.
	FString GeneratedCodeRoot;
	FString PathError;
	if (!FFlockCodegenPaths::TryResolveGeneratedRootFromSettings(GeneratedCodeRoot, PathError))
	{
		GeneratedCodeRoot.Reset();
	}

	return CleanAt(Config ? Config->GeneratedContentPath : FString(), GeneratedCodeRoot, bShowConfirmation);
}

FFlockCodegenRunner::FCleanResult FFlockCodegenRunner::CleanAt(const FString& ContentPath,
	const FString& GeneratedRoot, bool bShowConfirmation)
{
	FCleanResult Result;

	FString Root = ContentPath.TrimStartAndEnd();
	Root.RemoveFromEnd(TEXT("/"));
	if (Root.IsEmpty() || !Root.StartsWith(TEXT("/")))
	{
		Result.Error = FString::Printf(
			TEXT("Generated Content Path must be a package path such as /Game/Flock/Generated. Got '%s'."), *Root);
		return Result;
	}

	// The registry may never have looked inside the generated folder — nothing forces a scan of a path no
	// one has browsed — so an unscanned root would report zero assets and "clean" nothing.
	FAssetRegistryModule& RegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& Registry = RegistryModule.Get();
	Registry.ScanPathsSynchronous({ Root }, /*bForceRescan*/ true);

	TArray<FAssetData> Assets;
	Registry.GetAssetsByPath(FName(*Root), Assets, /*bRecursive*/ true);

	if (Assets.Num() > 0)
	{
		// FAssetData rather than loaded objects on purpose: DeleteAssets loads only what it needs and runs
		// the referencer check itself, which is the whole reason for going through it.
		Result.AssetsDeleted = ObjectTools::DeleteAssets(Assets, bShowConfirmation);
		// A confirmation the user declined comes back as zero, which is a cancel rather than a failure —
		// and the manifest must then stay, or the drift check would report never-generated for assets that
		// are still there.
		if (Result.AssetsDeleted == 0)
		{
			Result.Error = TEXT("Nothing was deleted. The assets may still be referenced, or the confirmation was cancelled.");
			return Result;
		}
	}

	if (!GeneratedRoot.IsEmpty())
	{
		const FString ManifestPath = FFlockCodegenPaths::ManifestPath(GeneratedRoot);
		if (IFileManager::Get().FileExists(*ManifestPath))
		{
			Result.bManifestRemoved = IFileManager::Get().Delete(*ManifestPath);
		}
	}

	Result.bSucceeded = true;
	return Result;
}

EFlockCodegenDrift FFlockCodegenRunner::CompareBakedVersion(const FString& BakedGameVersionId,
	const FFlockCodegenManifest& Stored, bool bHasStoredManifest)
{
	if (!bHasStoredManifest || Stored.GameVersionId.IsEmpty())
	{
		return EFlockCodegenDrift::NeverGenerated;
	}
	// An unbaked project cannot be compared against; the play-mode guard already covers that case, and
	// reporting drift here would just be noise on top of it.
	if (BakedGameVersionId.IsEmpty())
	{
		return EFlockCodegenDrift::Current;
	}
	return Stored.GameVersionId == BakedGameVersionId
		? EFlockCodegenDrift::Current
		: EFlockCodegenDrift::VersionChanged;
}

void FFlockCodegenRunner::WarnIfBakedVersionDrifted()
{
	FString GeneratedCodeRoot;
	FString PathError;
	if (!FFlockCodegenPaths::TryResolveGeneratedRootFromSettings(GeneratedCodeRoot, PathError))
	{
		return;
	}

	FFlockCodegenManifest Stored;
	const bool bHasManifest = FFlockCodegenManifest::TryRead(GeneratedCodeRoot, Stored);
	// Silent when codegen has never run: a project that does not use it should hear nothing about it.
	if (!bHasManifest)
	{
		return;
	}

	const UFlockConfig* Config = GetDefault<UFlockConfig>();
	const FString Baked = Config ? Config->GameVersionId : FString();
	if (CompareBakedVersion(Baked, Stored, bHasManifest) == EFlockCodegenDrift::VersionChanged)
	{
		UE_LOG(LogFlockEditor, Warning,
			TEXT("Flock: generated schemas are for game_version_id='%s' but '%s' is baked into project settings. ")
			TEXT("Run Tools > Flock > Sync Schemas."), *Stored.GameVersionId, *Baked);
	}
}
