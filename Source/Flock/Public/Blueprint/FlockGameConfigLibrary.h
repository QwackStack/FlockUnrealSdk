// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Models/FlockConfigModels.h"
#include "FlockGameConfigLibrary.generated.h"

/**
 * Blueprint reads over an FFlockGameConfigData. Each getter takes a dotted path ("stats.max_health") and a
 * fallback returned on a miss or a type mismatch, so the common read is a single pure node. Path segments
 * resolve exact-first then Pascal-cased, so a dashboard's snake_case name finds the flattened key.
 *
 * These delegate straight to FFlockGameConfigData so Blueprint and C++ cannot drift. Codegen will later
 * layer typed wrappers over the same handle.
 */
UCLASS()
class FLOCK_API UFlockGameConfigLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "Flock|Config", meta = (DisplayName = "Get Config Int"))
	static int32 GetConfigInt(const FFlockGameConfigData& Data, const FString& Path, int32 Fallback = 0);

	UFUNCTION(BlueprintPure, Category = "Flock|Config", meta = (DisplayName = "Get Config Float"))
	static float GetConfigFloat(const FFlockGameConfigData& Data, const FString& Path, float Fallback = 0.f);

	UFUNCTION(BlueprintPure, Category = "Flock|Config", meta = (DisplayName = "Get Config String"))
	static FString GetConfigString(const FFlockGameConfigData& Data, const FString& Path, const FString& Fallback = TEXT(""));

	UFUNCTION(BlueprintPure, Category = "Flock|Config", meta = (DisplayName = "Get Config Bool"))
	static bool GetConfigBool(const FFlockGameConfigData& Data, const FString& Path, bool bFallback = false);

	UFUNCTION(BlueprintPure, Category = "Flock|Config", meta = (DisplayName = "Get Config String Array"))
	static TArray<FString> GetConfigStringArray(const FFlockGameConfigData& Data, const FString& Path);

	UFUNCTION(BlueprintPure, Category = "Flock|Config", meta = (DisplayName = "Has Config Field"))
	static bool HasConfigField(const FFlockGameConfigData& Data, const FString& Path);

	UFUNCTION(BlueprintPure, Category = "Flock|Config", meta = (DisplayName = "Get Config Field Names"))
	static TArray<FString> GetConfigFieldNames(const FFlockGameConfigData& Data);

	UFUNCTION(BlueprintPure, Category = "Flock|Config", meta = (DisplayName = "Config Data To Json String"))
	static FString ConfigDataToJsonString(const FFlockGameConfigData& Data);

	UFUNCTION(BlueprintPure, Category = "Flock|Config", meta = (DisplayName = "Is Config Data Valid"))
	static bool IsConfigDataValid(const FFlockGameConfigData& Data);
};
