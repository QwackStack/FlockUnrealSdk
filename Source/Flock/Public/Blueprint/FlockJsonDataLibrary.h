// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Models/FlockJsonData.h"
#include "FlockJsonDataLibrary.generated.h"

/**
 * Blueprint reads over an FFlockJsonData (a shop item's/shop's free-form `data`/`stats`, and any other
 * provider's opaque JSON blob). Each getter takes a dotted path ("stats.visits") and a fallback returned
 * on a miss or a type mismatch, so the common read is a single pure node. Path segments resolve
 * exact-first then Pascal-cased, so a dashboard's snake_case key is found either way.
 *
 * These delegate straight to FFlockJsonData so Blueprint and C++ cannot drift.
 */
UCLASS()
class FLOCK_API UFlockJsonDataLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "Flock|Json", meta = (DisplayName = "Get Json Int"))
	static int32 GetJsonInt(const FFlockJsonData& Data, const FString& Path, int32 Fallback = 0);

	UFUNCTION(BlueprintPure, Category = "Flock|Json", meta = (DisplayName = "Get Json Float"))
	static float GetJsonFloat(const FFlockJsonData& Data, const FString& Path, float Fallback = 0.f);

	UFUNCTION(BlueprintPure, Category = "Flock|Json", meta = (DisplayName = "Get Json String"))
	static FString GetJsonString(const FFlockJsonData& Data, const FString& Path, const FString& Fallback = TEXT(""));

	UFUNCTION(BlueprintPure, Category = "Flock|Json", meta = (DisplayName = "Get Json Bool"))
	static bool GetJsonBool(const FFlockJsonData& Data, const FString& Path, bool bFallback = false);

	UFUNCTION(BlueprintPure, Category = "Flock|Json", meta = (DisplayName = "Get Json String Array"))
	static TArray<FString> GetJsonStringArray(const FFlockJsonData& Data, const FString& Path);

	UFUNCTION(BlueprintPure, Category = "Flock|Json", meta = (DisplayName = "Has Json Field"))
	static bool HasJsonField(const FFlockJsonData& Data, const FString& Path);

	UFUNCTION(BlueprintPure, Category = "Flock|Json", meta = (DisplayName = "Get Json Field Names"))
	static TArray<FString> GetJsonFieldNames(const FFlockJsonData& Data);

	UFUNCTION(BlueprintPure, Category = "Flock|Json", meta = (DisplayName = "Json Data To String"))
	static FString JsonDataToString(const FFlockJsonData& Data);

	UFUNCTION(BlueprintPure, Category = "Flock|Json", meta = (DisplayName = "Is Json Data Valid"))
	static bool IsJsonDataValid(const FFlockJsonData& Data);
};
