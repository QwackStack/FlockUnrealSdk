// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FlockAnalyticsLibrary.generated.h"

/**
 * Blueprint metadata builders — the graph equivalent of FFlockMetadata.
 *
 * The analytics calls take a `Map of Strings to Strings`, which is a poor pin to be handed in a
 * graph: dragging off it offers no way to build one, `Make Map` is buried under Utilities, and every
 * value has to be converted to a string by hand first.
 *
 * Each of these takes a metadata map and returns one, so they chain — and because they *return* the
 * pin's own type, Unreal's context-sensitive menu lists them the moment you drag off an Extra Data
 * pin. That is the point: the fix has to appear where the confusion happens.
 *
 *     Make Flock Metadata ─→ Flock Metadata (Int) "level" 3 ─→ Flock Metadata (Bool) "flawless" true ─→ Extra Data
 *
 * Formatting is delegated to FFlockMetadata rather than reimplemented, so a value written from
 * Blueprint and the same value written from C++ reach the backend identically.
 */
UCLASS()
class FLOCK_API UFlockAnalyticsLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Empty metadata to start a chain, so the first link needs nothing wired into it. */
	UFUNCTION(BlueprintPure, Category = "Flock|Analytics", meta = (DisplayName = "Make Flock Metadata"))
	static TMap<FString, FString> MakeMetadata();

	UFUNCTION(BlueprintPure, Category = "Flock|Analytics",
		meta = (AutoCreateRefTerm = "Metadata", DisplayName = "Flock Metadata (String)"))
	static TMap<FString, FString> AddMetadataString(const TMap<FString, FString>& Metadata,
		const FString& Key, const FString& Value);

	UFUNCTION(BlueprintPure, Category = "Flock|Analytics",
		meta = (AutoCreateRefTerm = "Metadata", DisplayName = "Flock Metadata (Integer)"))
	static TMap<FString, FString> AddMetadataInt(const TMap<FString, FString>& Metadata,
		const FString& Key, int32 Value);

	UFUNCTION(BlueprintPure, Category = "Flock|Analytics",
		meta = (AutoCreateRefTerm = "Metadata", DisplayName = "Flock Metadata (Float)"))
	static TMap<FString, FString> AddMetadataFloat(const TMap<FString, FString>& Metadata,
		const FString& Key, float Value);

	UFUNCTION(BlueprintPure, Category = "Flock|Analytics",
		meta = (AutoCreateRefTerm = "Metadata", DisplayName = "Flock Metadata (Boolean)"))
	static TMap<FString, FString> AddMetadataBool(const TMap<FString, FString>& Metadata,
		const FString& Key, bool Value);
};
