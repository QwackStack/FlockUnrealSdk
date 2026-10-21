// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "Algo/Count.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Codegen/FlockGeneratedAssets.h"
#include "Codegen/FlockSchemaSnapshot.h"
#include "Config/FlockConfig.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/UserDefinedEnum.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/FlockEngineCompat.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include FLOCK_USER_DEFINED_STRUCT_HEADER
#include "UObject/Package.h"

/**
 * What the codegen tests that delete need: a throwaway content folder, assets a game made in it, and a throwaway C++
 * project. All of it lands under Saved/, never in the project's Content, Source or .uproject.
 */
namespace FlockCodegenTestContent
{
	inline IAssetRegistry& AssetRegistry()
	{
		return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	}

	inline bool SaveAsset(UObject* Asset)
	{
		if (!Asset)
		{
			return false;
		}
		FAssetRegistryModule::AssetCreated(Asset);
		Asset->MarkPackageDirty();
		return UEditorLoadingAndSavingUtils::SavePackages({ Asset->GetPackage() }, /*bOnlyDirty*/ false);
	}

	/** A new package, fully loaded as the emitters' are, or the engine will not save it. */
	inline UPackage* NewPackage(const FString& PackageName)
	{
		UPackage* Package = CreatePackage(*PackageName);
		if (Package)
		{
			Package->FullyLoad();
		}
		return Package;
	}

	/** A struct the game made, which codegen never marks. False when it could not be made and saved. */
	inline bool PlantGameStruct(const FString& PackageName)
	{
		UPackage* Package = NewPackage(PackageName);
		return Package && SaveAsset(FStructureEditorUtils::CreateUserDefinedStruct(
			Package, FName(*FPackageName::GetShortName(PackageName)), RF_Public | RF_Standalone));
	}

	/** An enum the game made. False when it could not be made and saved. */
	inline bool PlantGameEnum(const FString& PackageName)
	{
		UPackage* Package = NewPackage(PackageName);
		return Package && SaveAsset(FEnumEditorUtils::CreateUserDefinedEnum(
			Package, FName(*FPackageName::GetShortName(PackageName)), RF_Public | RF_Standalone));
	}

	/** An Actor Blueprint the game made: a Blueprint, but no library. False when it could not be made and saved. */
	inline bool PlantGameActorBlueprint(const FString& PackageName)
	{
		UPackage* Package = NewPackage(PackageName);
		return Package && SaveAsset(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package,
			FName(*FPackageName::GetShortName(PackageName)), BPTYPE_Normal, UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass()));
	}

	/** True when an asset is saved under that package name. */
	inline bool IsOnDisk(const FString& PackageName)
	{
		return FPackageName::DoesPackageExist(PackageName);
	}

	/** Every asset under the root, at any depth, scanned from disk first. */
	inline TArray<FAssetData> AssetsUnder(const FString& Root)
	{
		IAssetRegistry& Registry = AssetRegistry();
		Registry.ScanPathsSynchronous({ Root }, /*bForceRescan*/ true);
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(*Root), Assets, /*bRecursive*/ true);
		return Assets;
	}

	/**
	 * Every asset under the root as the asset registry reads it from the saved files, never from a loaded object:
	 * what a fresh editor session sees before anything is loaded.
	 */
	inline TArray<FAssetData> AssetsOnDiskUnder(const FString& Root)
	{
		IAssetRegistry& Registry = AssetRegistry();
		Registry.ScanPathsSynchronous({ Root }, /*bForceRescan*/ true);
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(*Root), Assets, /*bRecursive*/ true, /*bIncludeOnlyOnDiskAssets*/ true);
		return Assets;
	}

	inline int32 CountMarked(const TArray<FAssetData>& Assets)
	{
		return Algo::CountIf(Assets, [](const FAssetData& Asset) { return FFlockGeneratedAssets::IsMarked(Asset); });
	}

	inline bool AnyNamed(const TArray<FAssetData>& Assets, const FString& PackageName)
	{
		return Assets.ContainsByPredicate([&PackageName](const FAssetData& Asset)
			{
				return Asset.PackageName.ToString().Equals(PackageName, ESearchCase::CaseSensitive);
			});
	}

	/** Deletes everything a test left under its root, generated or not, and the root's folder on disk. */
	inline void RemoveContentRoot(const FString& Root)
	{
		const TArray<FAssetData> Assets = AssetsUnder(Root);
		if (!Assets.IsEmpty())
		{
			ObjectTools::DeleteAssets(Assets, /*bShowConfirmation*/ false);
		}
		FString Folder;
		if (FPackageName::TryConvertLongPackageNameToFilename(Root, Folder))
		{
			IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists*/ false, /*Tree*/ true);
		}
	}

	/**
	 * A content folder of its own for one test, mounted onto a folder under Saved/ and removed with everything in it
	 * when the test ends. Not /Temp: the engine saves nothing there and the asset registry does not scan it, so a test
	 * of what goes from disk would only ever see memory.
	 */
	struct FScratchContentFolder
	{
		/** The mount, like /Game: a whole content folder, which codegen refuses. */
		const FString MountRoot;
		const FString DiskFolder;

		/** The folder a test points Generated Content Path at, one below the mount, as a project's is. */
		const FString Root;

		explicit FScratchContentFolder(const TCHAR* TestName)
			: MountRoot(FString::Printf(TEXT("/%s_%s"), TestName, *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
			, DiskFolder(FPaths::Combine(FPaths::ProjectSavedDir(), MountRoot.RightChop(1)) + TEXT("/"))
			, Root(MountRoot + TEXT("/Generated"))
		{
			FPackageName::RegisterMountPoint(MountRoot + TEXT("/"), DiskFolder);
		}

		~FScratchContentFolder()
		{
			RemoveContentRoot(MountRoot);
			FPackageName::UnRegisterMountPoint(MountRoot + TEXT("/"), DiskFolder);
			IFileManager::Get().DeleteDirectory(*DiskFolder, /*RequireExists*/ false, /*Tree*/ true);
		}
	};

	/** Sets the project's codegen target for one test, and puts the project's own back when it ends. */
	struct FCodegenTargetForThisTest
	{
		EFlockCodegenTarget Before;

		explicit FCodegenTargetForThisTest(EFlockCodegenTarget Target)
		{
			UFlockConfig* Settings = GetMutableDefault<UFlockConfig>();
			Before = Settings->CodegenTarget;
			Settings->CodegenTarget = Target;
		}

		~FCodegenTargetForThisTest()
		{
			GetMutableDefault<UFlockConfig>()->CodegenTarget = Before;
		}
	};

	/**
	 * A throwaway project that looks like a C++ one: a .uproject plus a *.Target.cs, which is what the module emitter
	 * actually tests for. The real project's file is never touched by a test. Returns the .uproject's path.
	 */
	inline FString MakeScratchCppProject(const FString& ProjectDir)
	{
		IFileManager::Get().DeleteDirectory(*ProjectDir, /*RequireExists*/ false, /*Tree*/ true);
		IFileManager::Get().MakeDirectory(*FPaths::Combine(ProjectDir, TEXT("Source")), /*Tree*/ true);

		FFileHelper::SaveStringToFile(
			TEXT("using UnrealBuildTool;\npublic class ScratchTarget : TargetRules {}\n"),
			*FPaths::Combine(ProjectDir, TEXT("Source"), TEXT("Scratch.Target.cs")));

		const FString ProjectFile = FPaths::Combine(ProjectDir, TEXT("Scratch.uproject"));
		FFileHelper::SaveStringToFile(
			TEXT("{\n\t\"FileVersion\": 3,\n\t\"EngineAssociation\": \"5.5\",\n\t\"Modules\": [\n")
			TEXT("\t\t{\n\t\t\t\"Name\": \"Scratch\",\n\t\t\t\"Type\": \"Runtime\",\n")
			TEXT("\t\t\t\"LoadingPhase\": \"Default\"\n\t\t}\n\t]\n}\n"),
			*ProjectFile);
		return ProjectFile;
	}

	/** Just a config, for the tests that only care about the read-only path. */
	inline FFlockSchemaSnapshot ConfigOnlySnapshot()
	{
		FFlockSchemaSnapshot Result;
		Result.GameVersionId = TEXT("ver-1");
		FFlockGameConfigSchema Config;
		Config.Id = TEXT("cfg-1");
		Config.Name = TEXT("Gameplay");
		Config.SchemaJson = TEXT("[{\"type\":\"int\",\"field_name\":\"move_speed\",\"type_name\":\"int\"}]");
		Result.GameConfigs.Add(Config);
		return Result;
	}

	/**
	 * A game with something for every Blueprint emitter: a config (a struct), a writable template, the
	 * `currency`-tagged wallet whose id `Add Funds` bakes, the `achievement`-tagged row the achievement enum is
	 * built from, and a shop to give the item and currency enums members. The function library, the macro library
	 * and the catalog follow from these.
	 */
	inline FFlockSchemaSnapshot EverythingSnapshot()
	{
		FFlockSchemaSnapshot Result = ConfigOnlySnapshot();

		FFlockPlayerTemplateSchema Wallet;
		Wallet.Id = TEXT("tmpl-wallet");
		Wallet.Name = TEXT("Wallet");
		Wallet.Tag = TEXT("currency");
		Wallet.SchemaJson = TEXT("[{\"type\":\"int\",\"field_name\":\"gold\"},{\"type\":\"int\",\"field_name\":\"shards\"}]");
		Result.PlayerTemplates.Add(Wallet);

		FFlockPlayerTemplateSchema Trophies;
		Trophies.Id = TEXT("tmpl-trophies");
		Trophies.Name = TEXT("Trophies");
		Trophies.Tag = TEXT("achievement");
		Trophies.SchemaJson =
			TEXT("[{\"type\":\"bool\",\"field_name\":\"first_win\"},{\"type\":\"bool\",\"field_name\":\"flawless_run\"}]");
		Result.PlayerTemplates.Add(Trophies);

		FFlockShopItem Item;
		Item.Id = TEXT("item-1");
		Item.Name = TEXT("Gem Pack");
		Item.Currency = TEXT("Gold");
		FFlockShop Shop;
		Shop.Id = TEXT("shop-1");
		Shop.Name = TEXT("Starter");
		Shop.ShopItems.Add(Item);
		Result.Shops.Add(Shop);

		return Result;
	}
}
