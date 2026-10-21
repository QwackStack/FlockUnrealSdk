// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"

class UPackage;

/**
 * The one owner of which assets Flock codegen made, and so of which it may remove.
 *
 * Every asset an emitter writes carries a mark in its package's metadata, and the asset registry reads the mark
 * back without loading the asset. A switch to the C++ target and a Clean delete only marked assets, so a game's
 * own asset in the generated folder, or in a folder below it, stays whatever Generated Content Path is set to.
 *
 * Assets made by an SDK before the mark existed (1.38.2) carry none, so they are kept and named. The next sync on
 * the Blueprint target remakes every asset it still makes, and marks it.
 */
class FLOCKEDITOR_API FFlockGeneratedAssets
{
public:
	/** The metadata key on every generated asset, and the asset registry tag it is read back from. */
	static const FName MarkKey;

	/** The value stored under MarkKey. */
	static const TCHAR* const MarkValue;

	/**
	 * Has the asset registry read the mark, for a loaded asset and in what a save writes to disk. Called when the
	 * editor module starts, before anything is generated.
	 */
	static void RegisterMark();
	static void UnregisterMark();

	/** Marks every asset in a package an emitter has just written into, nested structs included. */
	static void MarkEveryAssetIn(UPackage* Package);

	/** True when codegen made the asset. */
	static bool IsMarked(const FAssetData& Asset);

	/**
	 * The package an emitter writes AssetName into, under an already-resolved content root, loaded so whatever is
	 * there already is in memory. Null, with OutProblem saying what is wrong and how to fix it, when the package
	 * cannot be made, or already holds an asset of that name and another class: that asset is not codegen's to
	 * replace, and the engine stops the editor rather than replace an object with one of another class.
	 */
	static UPackage* OpenPackageFor(const FString& ContentRoot, const FString& AssetName, const UClass* AssetClass,
		FString& OutProblem);

	struct FDeleteResult
	{
		/** Set when the content path was refused; nothing was looked at or deleted. */
		FString Error;

		int32 Deleted = 0;

		/** Marked assets still there afterwards: the engine's referencer check refused, or the prompt was declined. */
		int32 GeneratedLeft = 0;

		/** The assets under the path that carry no mark. Every one is kept. */
		TArray<FAssetData> Kept;
	};

	/**
	 * Deletes every marked asset under ContentPath, at any depth, and nothing else; marked assets of KindsToKeep stay
	 * too (a switch of target keeps the catalog both targets share). Through ObjectTools rather than the file system,
	 * because that raises the engine's own referencer prompt: a Blueprint holding a hard reference to a generated
	 * struct is exactly the case this must not break silently.
	 */
	static FDeleteResult DeleteUnder(const FString& ContentPath, bool bShowConfirmation,
		const TArray<const UClass*>& KindsToKeep = {});

	/** A sentence naming the kept assets and what to do about them, for a summary or a warning. Empty when none were kept. */
	static FString DescribeKept(const TArray<FAssetData>& Kept);
};
