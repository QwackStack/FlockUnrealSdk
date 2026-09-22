// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Models/FlockCommandModels.h"
#include "FlockAnalyticsLibrary.generated.h"

/**
 * The graph's way of building custom data, for both surfaces — and they are not the same container.
 *
 * A diagnostic entry (Flock Log Diagnostic Event) takes a `Map of Strings to Strings`: the backend stores
 * that surface's extra data as text. A gameplay event (Flock Track Event) takes Flock Event Properties,
 * where a number stays a number and can be charted. Handing a graph either raw pin is a poor deal:
 * dragging off one offers no way to build it, `Make Map` is buried under Utilities, and every value would
 * have to be converted by hand.
 *
 * So there are two chains, each named for the call it feeds, and each listed by Unreal's context menu the
 * moment you drag off that call's pin — because each node *returns* the pin's own type. That is the point:
 * the fix has to appear where the confusion happens.
 *
 *     Make Flock Metadata ─→ Flock Metadata (Int) "level" 3 ─→ Flock Metadata (Bool) "flawless" true ─→ Extra Data
 *     Make Flock Event Properties ─→ Flock Event Property (Int) "level" 3 ─→ Flock Event Property (Bool) ─→ Properties
 *
 * Both delegate their formatting — the first to FFlockMetadata, the second to FFlockCommandData — rather
 * than reimplementing it, so a value written from Blueprint and the same value written from C++ reach the
 * backend identically. The event-property nodes are the same struct the game-commands nodes write, named
 * for this surface: a graph recording a level completion should not have to reach for a node called
 * Set Command Int to say what happened.
 */
UCLASS()
class FLOCK_API UFlockAnalyticsLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Empty metadata to start a chain, so the first link needs nothing wired into it. */
	UFUNCTION(BlueprintPure, Category = "Flock|Diagnostics", meta = (DisplayName = "Make Flock Metadata"))
	static TMap<FString, FString> MakeMetadata();

	UFUNCTION(BlueprintPure, Category = "Flock|Diagnostics",
		meta = (AutoCreateRefTerm = "Metadata", DisplayName = "Flock Metadata (String)"))
	static TMap<FString, FString> AddMetadataString(const TMap<FString, FString>& Metadata,
		const FString& Key, const FString& Value);

	UFUNCTION(BlueprintPure, Category = "Flock|Diagnostics",
		meta = (AutoCreateRefTerm = "Metadata", DisplayName = "Flock Metadata (Integer)"))
	static TMap<FString, FString> AddMetadataInt(const TMap<FString, FString>& Metadata,
		const FString& Key, int32 Value);

	UFUNCTION(BlueprintPure, Category = "Flock|Diagnostics",
		meta = (AutoCreateRefTerm = "Metadata", DisplayName = "Flock Metadata (Float)"))
	static TMap<FString, FString> AddMetadataFloat(const TMap<FString, FString>& Metadata,
		const FString& Key, float Value);

	UFUNCTION(BlueprintPure, Category = "Flock|Diagnostics",
		meta = (AutoCreateRefTerm = "Metadata", DisplayName = "Flock Metadata (Boolean)"))
	static TMap<FString, FString> AddMetadataBool(const TMap<FString, FString>& Metadata,
		const FString& Key, bool Value);

	/** Empty properties to start a chain, so the first link needs nothing wired into it. */
	UFUNCTION(BlueprintPure, Category = "Flock|Analytics", meta = (DisplayName = "Make Flock Event Properties"))
	static FFlockCommandData MakeEventProperties();

	UFUNCTION(BlueprintPure, Category = "Flock|Analytics",
		meta = (AutoCreateRefTerm = "Properties", DisplayName = "Flock Event Property (String)"))
	static FFlockCommandData AddEventPropertyString(const FFlockCommandData& Properties,
		const FString& Key, const FString& Value);

	UFUNCTION(BlueprintPure, Category = "Flock|Analytics",
		meta = (AutoCreateRefTerm = "Properties", DisplayName = "Flock Event Property (Integer)"))
	static FFlockCommandData AddEventPropertyInt(const FFlockCommandData& Properties,
		const FString& Key, int32 Value);

	UFUNCTION(BlueprintPure, Category = "Flock|Analytics",
		meta = (AutoCreateRefTerm = "Properties", DisplayName = "Flock Event Property (Float)"))
	static FFlockCommandData AddEventPropertyFloat(const FFlockCommandData& Properties,
		const FString& Key, float Value);

	UFUNCTION(BlueprintPure, Category = "Flock|Analytics",
		meta = (AutoCreateRefTerm = "Properties", DisplayName = "Flock Event Property (Boolean)"))
	static FFlockCommandData AddEventPropertyBool(const FFlockCommandData& Properties,
		const FString& Key, bool Value);

	UFUNCTION(BlueprintPure, Category = "Flock|Analytics",
		meta = (AutoCreateRefTerm = "Properties", DisplayName = "Flock Event Property (String Array)"))
	static FFlockCommandData AddEventPropertyStringArray(const FFlockCommandData& Properties,
		const FString& Key, const TArray<FString>& Value);
};
