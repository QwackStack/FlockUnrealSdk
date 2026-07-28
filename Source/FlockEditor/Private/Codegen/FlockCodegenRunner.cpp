// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Codegen/FlockCodegenRunner.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Codegen/FlockCatalogBuilder.h"
#include "Codegen/FlockCodegenPaths.h"
#include "Codegen/FlockCppModuleEmitter.h"
#include "Codegen/FlockCppAccessorEmitter.h"
#include "Codegen/FlockCppTypeEmitter.h"
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

namespace
{
	/**
	 * Deletes every asset under a generated content path.
	 *
	 * Through ObjectTools rather than the file system, because that raises the engine's own referencer
	 * prompt — a Blueprint holding a hard reference to a generated struct is exactly the case this must
	 * not break silently.
	 */
	/** How many assets are still under a path, to tell a completed delete from a refused one. */
	int32 CountContentAssets(const FString& PackageRoot)
	{
		FAssetRegistryModule& RegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		TArray<FAssetData> Assets;
		RegistryModule.Get().GetAssetsByPath(FName(*PackageRoot), Assets, /*bRecursive*/ true);
		return Assets.Num();
	}

	int32 DeleteContentAssets(const FString& PackageRoot, bool bShowConfirmation)
	{
		FAssetRegistryModule& RegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& Registry = RegistryModule.Get();
		// Nothing forces a scan of a path no one has browsed, and an unscanned root reports zero assets.
		Registry.ScanPathsSynchronous({ PackageRoot }, /*bForceRescan*/ true);

		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(*PackageRoot), Assets, /*bRecursive*/ true);
		return Assets.Num() > 0 ? ObjectTools::DeleteAssets(Assets, bShowConfirmation) : 0;
	}
}

FString FFlockCodegenRunner::FRunResult::Describe() const
{
	if (!bSucceeded)
	{
		return FString::Printf(TEXT("Flock schema sync failed: %s"), *Error);
	}
	const FString WarningSuffix = Warnings.Num() > 0
		? FString::Printf(TEXT(" (%d warning(s))"), Warnings.Num())
		: FString();

	if (bCppTarget)
	{
		// Says what to do next rather than implying it is usable now. The editor cannot adopt new
		// reflection data, so the types in this module do not exist until the project is rebuilt and
		// reopened — promising otherwise would be a lie the first compile error exposes.
		return FString::Printf(
			TEXT("Flock schemas synced for %s — %d template(s), %d config(s), %d shop(s) -> module '%s'%s. ")
			TEXT("Rebuild the project and restart the editor to use the generated types.%s"),
			*GameVersionId, TemplateCount, ConfigCount, ShopCount, *ModuleName,
			bProjectFilePatched ? TEXT(" (registered in the .uproject)") : TEXT(""), *WarningSuffix);
	}

	return FString::Printf(
		TEXT("Flock schemas synced for %s — %d template(s), %d config(s), %d shop(s) -> %d struct(s), %d enum(s), %d function(s), %d macro(s)%s"),
		*GameVersionId, TemplateCount, ConfigCount, ShopCount, StructCount, EnumCount, FunctionCount, MacroCount,
		*WarningSuffix);
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

	const UFlockConfig* Settings = GetDefault<UFlockConfig>();
	const EFlockCodegenTarget Target = Settings ? Settings->CodegenTarget : EFlockCodegenTarget::Blueprint;

	// The C++ skeleton goes first, and a failure here aborts before anything else runs. A generated module
	// that will not compile breaks the whole project's build rather than one feature, so there is no
	// "best effort" version of this step — unlike an asset emitter, where four of five is still useful.
	if (Target == EFlockCodegenTarget::Cpp)
	{
		const FFlockCppModuleEmitter::FEmitResult Module =
			FFlockCppModuleEmitter::EmitForProject(GeneratedCodeRoot, FFlockCodegenManifest::FromSnapshot(Snapshot));
		Result.Warnings.Append(Module.Warnings);
		if (!Module.bSucceeded)
		{
			Result.Error = Module.Error;
			return Result;
		}
		Result.bCppTarget = true;
		Result.ModuleName = Module.ModuleName;
		Result.bProjectFilePatched = Module.bProjectFilePatched;

		// Clear the other target's output. The two overlap in the action menu, so leaving the Blueprint
		// assets behind would show two entries for every entity — and the stale one would keep working,
		// which is worse than it failing. Done before the catalog is written, which both targets share.
		//
		// Reported rather than assumed: deleting an asset runs the engine's referencer check, which needs
		// a real editor — headlessly (the CI commandlet) it refuses, and silently leaving the duplicates
		// behind would be the worst outcome of the three.
		DeleteContentAssets(ContentPath, /*bShowConfirmation*/ false);
		if (CountContentAssets(ContentPath) > 0)
		{
			Result.Warnings.Add(FString::Printf(
				TEXT("The Blueprint target's generated assets are still in '%s' and will appear in the action ")
				TEXT("menu alongside the C++ types. Run Tools > Flock > Clean Generated from the editor to ")
				TEXT("remove them — deleting assets needs the editor's referencer check, so it cannot be done ")
				TEXT("from a commandlet."), *ContentPath));
		}

		// Types into a module already known to compile. Warnings here name a field that degraded or was
		// skipped, which is the same contract as the Blueprint emitters — the headers still build.
		const FFlockCppTypeEmitter::FEmitResult Types =
			FFlockCppTypeEmitter::Emit(Snapshot, GeneratedCodeRoot, Module.ModuleName);
		Result.Warnings.Append(Types.Warnings);
		Result.StructCount = Types.StructCount;
		Result.EnumCount = Types.EnumCount;
		if (!Types.bSucceeded)
		{
			Result.Error = Types.Error;
			return Result;
		}

		// Accessors after the types, because they name those struct types in their signatures — and after
		// the type emitter's wipe, which owns every header in Public and would otherwise delete this one.
		const FFlockCppAccessorEmitter::FEmitResult Accessors =
			FFlockCppAccessorEmitter::Emit(Snapshot, GeneratedCodeRoot, Module.ModuleName);
		Result.Warnings.Append(Accessors.Warnings);
		Result.FunctionCount = Accessors.FunctionCount;
		if (!Accessors.bSucceeded)
		{
			Result.Error = Accessors.Error;
			return Result;
		}
	}

	// The catalog first: it is the browsable record of what this sync saw, and is useful even if a later
	// emitter has trouble.
	FString CatalogError;
	if (!FFlockCatalogBuilder::Save(Snapshot, ContentPath, CatalogError))
	{
		Result.Warnings.Add(FString::Printf(TEXT("Content catalog: %s"), *CatalogError));
	}

	// The Blueprint asset emitters below run only for the Blueprint target. The two targets overlap in
	// the action menu — a generated UFUNCTION and a generated Blueprint library function appear under the
	// same name — so a project gets one or the other, never both.
	if (Target == EFlockCodegenTarget::Blueprint)
	{
	// The C++ types go, for the same reason and with the same asymmetry: the module *skeleton* stays, so
	// a project that switched away still compiles without a .uproject edit or a rebuild.
	{
		FString SwitchError;
		if (!FFlockCppModuleEmitter::RemoveGeneratedTypes(GeneratedCodeRoot,
			FFlockCppModuleEmitter::ModuleNameFromRoot(GeneratedCodeRoot), /*bResetManifest*/ true, SwitchError))
		{
			Result.Warnings.Add(SwitchError);
		}
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
	} // Blueprint target

	// The manifest last, and only if something was actually emitted: it is the claim that generated output
	// exists and matches this schema, so writing it after a failed run would make a later drift check
	// report "up to date" about assets that were never written.
	if (!Result.bCppTarget && Result.StructCount == 0 && Result.EnumCount == 0 && Result.FunctionCount == 0)
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

	Result.AssetsDeleted = DeleteContentAssets(Root, bShowConfirmation);

	// The C++ side too, whichever target is currently selected — a Clean should leave nothing generated
	// behind, and a project that has switched targets may hold output from both.
	if (!GeneratedRoot.IsEmpty())
	{
		const FString ModuleName = FFlockCppModuleEmitter::ModuleNameFromRoot(GeneratedRoot);
		FString TypeError;
		// The skeleton stays: deleting the .Build.cs would leave a registered module with no sources, so a
		// cleaned project would not build at all.
		if (!FFlockCppModuleEmitter::RemoveGeneratedTypes(GeneratedRoot, ModuleName,
			/*bResetManifest*/ true, TypeError))
		{
			Result.Error = TypeError;
			return Result;
		}

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
