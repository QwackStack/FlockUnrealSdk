// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FlockGameModels.generated.h"

/**
 * A game version record from the backend (OpenAPI `GameVersionSchema`). The `Id` is what the editor
 * resolve step bakes into UFlockConfig. Dates are kept as raw ISO-8601 strings for now; codegen can
 * type them later. Field names map from the snake_case wire via FFlockJsonUtils (release_type ->
 * ReleaseType, created_at -> CreatedAt).
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockGameVersionSchema
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString ReleaseType;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Env;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString CreatedAt;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString UpdatedAt;
};
