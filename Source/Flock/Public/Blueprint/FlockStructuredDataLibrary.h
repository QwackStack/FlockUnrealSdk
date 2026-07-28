// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Models/FlockStructuredData.h"
#include "FlockStructuredDataLibrary.generated.h"

/**
 * Blueprint reads over an FFlockStructuredData (a config's/patch's or a player template's/row's flattened
 * data). Each getter takes a dotted path ("stats.max_health") and a fallback returned on a miss or a type
 * mismatch, so the common read is a single pure node. Path segments resolve exact-first then Pascal-cased,
 * so a dashboard's snake_case name finds the flattened key.
 *
 * These delegate straight to FFlockStructuredData so Blueprint and C++ cannot drift. Codegen will later
 * layer typed wrappers over the same handle.
 */
UCLASS()
class FLOCK_API UFlockStructuredDataLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "Flock|Data", meta = (DisplayName = "Get Data Int"))
	static int32 GetDataInt(const FFlockStructuredData& Data, const FString& Path, int32 Fallback = 0);

	UFUNCTION(BlueprintPure, Category = "Flock|Data", meta = (DisplayName = "Get Data Float"))
	static float GetDataFloat(const FFlockStructuredData& Data, const FString& Path, float Fallback = 0.f);

	UFUNCTION(BlueprintPure, Category = "Flock|Data", meta = (DisplayName = "Get Data String"))
	static FString GetDataString(const FFlockStructuredData& Data, const FString& Path, const FString& Fallback = TEXT(""));

	UFUNCTION(BlueprintPure, Category = "Flock|Data", meta = (DisplayName = "Get Data Bool"))
	static bool GetDataBool(const FFlockStructuredData& Data, const FString& Path, bool bFallback = false);

	UFUNCTION(BlueprintPure, Category = "Flock|Data", meta = (DisplayName = "Get Data String Array"))
	static TArray<FString> GetDataStringArray(const FFlockStructuredData& Data, const FString& Path);

	UFUNCTION(BlueprintPure, Category = "Flock|Data", meta = (DisplayName = "Has Data Field"))
	static bool HasDataField(const FFlockStructuredData& Data, const FString& Path);

	UFUNCTION(BlueprintPure, Category = "Flock|Data", meta = (DisplayName = "Get Data Field Names"))
	static TArray<FString> GetDataFieldNames(const FFlockStructuredData& Data);

	UFUNCTION(BlueprintPure, Category = "Flock|Data", meta = (DisplayName = "Data To Json String"))
	static FString DataToJsonString(const FFlockStructuredData& Data);

	// Named IsValidData (not IsDataValid) to avoid hiding UObject::IsDataValid; the BP label stays "Is Data Valid".
	UFUNCTION(BlueprintPure, Category = "Flock|Data", meta = (DisplayName = "Is Data Valid"))
	static bool IsValidData(const FFlockStructuredData& Data);
};
