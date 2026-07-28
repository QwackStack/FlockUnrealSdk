// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Codegen/FlockCodegenManifest.h"
#include "Codegen/FlockSchemaSnapshot.h"

/**
 * Runs a sync: fetch the backend schema, emit everything from it, and record what was emitted.
 *
 * The order is not arbitrary. Structs come before the function library because the library bakes struct
 * *types* into its pins and cannot do that from a name; the manifest is written last, because it is the
 * claim that a sync completed and must not exist if one did not.
 *
 * Failure is all-or-nothing at the fetch, and best-effort after it. A fetch that fails writes nothing —
 * the previous generated output stays intact, which matters because emitting wipes as it goes. Once
 * emitting starts, a single asset failing is reported rather than aborting the rest: a designer is better
 * served by four of five assets plus a clear message than by nothing.
 */
class FLOCKEDITOR_API FFlockCodegenRunner
{
public:
	struct FRunResult
	{
		bool bSucceeded = false;

		/** Empty on success; the first thing that went irrecoverably wrong otherwise. */
		FString Error;

		/** Non-fatal problems — a field that could not be typed, an asset that could not be written. */
		TArray<FString> Warnings;

		FString GameVersionId;
		int32 StructCount = 0;
		int32 EnumCount = 0;
		int32 FunctionCount = 0;
		int32 MacroCount = 0;
		int32 TemplateCount = 0;
		int32 ConfigCount = 0;
		int32 ShopCount = 0;

		/** True when the backend cut a new version under the configured name since the last bake. */
		bool bBakeStale = false;

		// ── C++ target only ──

		/** True when this run emitted a generated C++ module rather than Blueprint assets. */
		bool bCppTarget = false;

		/** The generated module's name, so the summary can name what needs rebuilding. */
		FString ModuleName;

		/** True when the .uproject gained the module entry on this run — the first sync after switching. */
		bool bProjectFilePatched = false;

		/** A one-line summary for a toast; the detail goes to the log. */
		FString Describe() const;
	};

	struct FCleanResult
	{
		bool bSucceeded = false;
		FString Error;
		int32 AssetsDeleted = 0;
		bool bManifestRemoved = false;

		FString Describe() const;
	};

	DECLARE_DELEGATE_OneParam(FOnSyncComplete, const FRunResult&);

	/** Fetches and emits, driven by project settings. Asynchronous: the fetch is a network round trip. */
	static void Sync(FOnSyncComplete OnComplete);

	/**
	 * Removes everything a sync wrote: the generated assets, then the manifest.
	 *
	 * Deletes through `ObjectTools::DeleteAssets` rather than the file system, because that raises the
	 * engine's own referencer prompt — a Blueprint holding a hard reference to a generated struct is
	 * exactly the case a clean must not break silently. `bShowConfirmation` drives that prompt; the menu
	 * passes true, a headless caller passes false and accepts the consequences.
	 *
	 * The manifest goes **last and unconditionally**, mirroring how a sync writes it last: it is the claim
	 * that generated output exists, so leaving it behind would have the drift check vouch for an empty
	 * folder. (A generated C++ module would need its skeleton kept instead — there is none in this tier.)
	 */
	static FCleanResult Clean(bool bShowConfirmation = true);

	/**
	 * Clean against explicit paths rather than project settings — the testable half, mirroring the
	 * BuildLibrary/Emit split the emitters use. `GeneratedRoot` may be empty to leave the manifest alone.
	 */
	static FCleanResult CleanAt(const FString& ContentPath, const FString& GeneratedRoot, bool bShowConfirmation);

	/** Emits from an already-fetched snapshot. Synchronous, and the half worth testing. */
	static FRunResult EmitAll(const FFlockSchemaSnapshot& Snapshot, const FString& ContentPath,
		const FString& GeneratedCodeRoot);

	// ── Drift, without a network round trip ──

	/**
	 * Compares the id a sync generated for against the one baked into project settings. Cheap enough to
	 * run on editor startup, and it catches the common case — someone re-baked the version, or pulled a
	 * branch whose generated code is for a different one — without contacting the backend.
	 *
	 * A full drift check (has the schema *content* changed within the same version?) needs a fetch and
	 * lives on the manifest itself.
	 */
	static EFlockCodegenDrift CompareBakedVersion(const FString& BakedGameVersionId,
		const FFlockCodegenManifest& Stored, bool bHasStoredManifest);

	/** Logs a warning when generated code has drifted from the baked version. No-op when it has not. */
	static void WarnIfBakedVersionDrifted();
};
