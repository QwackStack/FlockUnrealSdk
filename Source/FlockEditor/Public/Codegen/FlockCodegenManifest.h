// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Codegen/FlockSchemaSnapshot.h"

/** How committed generated code stands against the backend right now. */
enum class EFlockCodegenDrift : uint8
{
	/** Same version, same content — the generated code matches the backend. */
	Current,
	/** No manifest on disk: codegen has never run here, or the generated folder was cleaned. */
	NeverGenerated,
	/** The backend cut a new game version since the last sync. */
	VersionChanged,
	/** Same version, but its schemas were edited — the case an id comparison cannot see. */
	ContentChanged,
};

/**
 * What a sync recorded about itself: which version it generated for, and a fingerprint of the schema it
 * generated from. Comparing this against a fresh snapshot is the whole of drift detection.
 *
 * **Written as JSON on disk, not as a generated header.** The canonical SDK reads its manifest back out of
 * compiled constants by reflection, which Unreal cannot copy: the editor has to answer "is this stale?"
 * *before* the generated module has ever been compiled, and possibly when it does not exist at all. A
 * sidecar file removes that chicken-and-egg entirely. The C++ tier additionally emits a header carrying
 * the same values for code that wants them at runtime; this file stays the source of truth for tooling.
 */
struct FLOCKEDITOR_API FFlockCodegenManifest
{
	FString GameVersionId;
	FString ContentHash;
	FString GeneratedAtUtc;
	FString SdkVersion;

	int32 PlayerTemplateCount = 0;
	int32 GameConfigCount = 0;
	int32 ShopCount = 0;

	/** Records what a sync of this snapshot produced, stamping the time and the SDK version. */
	static FFlockCodegenManifest FromSnapshot(const FFlockSchemaSnapshot& Snapshot);

	FString ToJson() const;
	static bool TryParse(const FString& Json, FFlockCodegenManifest& OutManifest);

	/** Writes the manifest into a resolved generated root, creating the directory when absent. */
	static bool Write(const FString& GeneratedRoot, const FFlockCodegenManifest& Manifest, FString& OutError);

	/** False when there is no manifest, or it cannot be read — both mean "never generated". */
	static bool TryRead(const FString& GeneratedRoot, FFlockCodegenManifest& OutManifest);

	/** Drift of what was generated against what the backend now reports. */
	static EFlockCodegenDrift Compare(const FFlockCodegenManifest& Stored, const FFlockSchemaSnapshot& Fresh);

	/** A sentence naming the drift and what to do about it, for a log line or a toast. */
	static FString Describe(EFlockCodegenDrift Drift, const FFlockCodegenManifest& Stored,
		const FFlockSchemaSnapshot& Fresh);
};
