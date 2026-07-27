// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Models/FlockCommandModels.h"
#include "FlockCommandDataLibrary.generated.h"

/**
 * Blueprint builders for the values a game command writes.
 *
 * The Set nodes take a command-data bag and *return* one, so dragging off an Update Player Data pin chains
 * them without a variable in sight:
 *
 *     Set Command Int ("gold", 250) -> Set Command Bool ("prestige", true) -> Flock Update Player Data
 *
 * Returning the bag (rather than mutating a reference) is what makes the editor's context menu offer them
 * off a Data pin — the same reason the analytics metadata nodes are shaped this way. Every node delegates
 * to FFlockCommandData / FFlockCommandValue so Blueprint and C++ cannot drift.
 *
 * Field names are sent verbatim: type them exactly as the player template declares them.
 */
UCLASS()
class FLOCK_API UFlockCommandDataLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// ── Command data (a bag of fields for Update Player Data) ──

	UFUNCTION(BlueprintPure, Category = "Flock|Commands", meta = (DisplayName = "Set Command Int"))
	static FFlockCommandData SetCommandInt(const FFlockCommandData& Data, const FString& Key, int32 Value);

	UFUNCTION(BlueprintPure, Category = "Flock|Commands", meta = (DisplayName = "Set Command Float"))
	static FFlockCommandData SetCommandFloat(const FFlockCommandData& Data, const FString& Key, float Value);

	UFUNCTION(BlueprintPure, Category = "Flock|Commands", meta = (DisplayName = "Set Command String"))
	static FFlockCommandData SetCommandString(const FFlockCommandData& Data, const FString& Key, const FString& Value);

	UFUNCTION(BlueprintPure, Category = "Flock|Commands", meta = (DisplayName = "Set Command Bool"))
	static FFlockCommandData SetCommandBool(const FFlockCommandData& Data, const FString& Key, bool bValue);

	UFUNCTION(BlueprintPure, Category = "Flock|Commands", meta = (DisplayName = "Set Command String Array"))
	static FFlockCommandData SetCommandStringArray(const FFlockCommandData& Data, const FString& Key,
		const TArray<FString>& Value);

	/** Sets a field from raw JSON — the escape hatch for a nested object or list. Invalid JSON writes null. */
	UFUNCTION(BlueprintPure, Category = "Flock|Commands", meta = (DisplayName = "Set Command Json"))
	static FFlockCommandData SetCommandJson(const FFlockCommandData& Data, const FString& Key, const FString& Json);

	UFUNCTION(BlueprintPure, Category = "Flock|Commands", meta = (DisplayName = "Get Command Field Names"))
	static TArray<FString> GetCommandFieldNames(const FFlockCommandData& Data);

	UFUNCTION(BlueprintPure, Category = "Flock|Commands", meta = (DisplayName = "Command Data To Json String"))
	static FString CommandDataToJsonString(const FFlockCommandData& Data);

	// Named IsEmptyCommandData rather than IsEmpty for the same reason the data library avoids IsDataValid:
	// short generic names collide with UObject members when a node is called on a Blueprint's own class.
	UFUNCTION(BlueprintPure, Category = "Flock|Commands", meta = (DisplayName = "Is Command Data Empty"))
	static bool IsEmptyCommandData(const FFlockCommandData& Data);

	// ── Command value (the single value for Update Player Data Field) ──

	UFUNCTION(BlueprintPure, Category = "Flock|Commands", meta = (DisplayName = "Command Value (Int)"))
	static FFlockCommandValue CommandValueInt(int32 Value);

	UFUNCTION(BlueprintPure, Category = "Flock|Commands", meta = (DisplayName = "Command Value (Float)"))
	static FFlockCommandValue CommandValueFloat(float Value);

	UFUNCTION(BlueprintPure, Category = "Flock|Commands", meta = (DisplayName = "Command Value (String)"))
	static FFlockCommandValue CommandValueString(const FString& Value);

	UFUNCTION(BlueprintPure, Category = "Flock|Commands", meta = (DisplayName = "Command Value (Bool)"))
	static FFlockCommandValue CommandValueBool(bool bValue);

	UFUNCTION(BlueprintPure, Category = "Flock|Commands", meta = (DisplayName = "Command Value (String Array)"))
	static FFlockCommandValue CommandValueStringArray(const TArray<FString>& Value);

	/** Wraps raw JSON as a value — the escape hatch for a nested shape. Invalid JSON yields null. */
	UFUNCTION(BlueprintPure, Category = "Flock|Commands", meta = (DisplayName = "Command Value (Json)"))
	static FFlockCommandValue CommandValueJson(const FString& Json);

	UFUNCTION(BlueprintPure, Category = "Flock|Commands", meta = (DisplayName = "Command Value To Json String"))
	static FString CommandValueToJsonString(const FFlockCommandValue& Value);
};
