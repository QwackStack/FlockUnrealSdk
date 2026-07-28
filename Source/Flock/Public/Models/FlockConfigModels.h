// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Models/FlockStructuredData.h"
#include "FlockConfigModels.generated.h"

/**
 * A config tag on the request side. Any means "no tag filter" — the query parameter is omitted. Named Any
 * rather than None because it reads correctly at a call site and sidesteps the FName `None`. The response
 * model keeps its tag as a plain FString: the backend types that field as a string, and an unrecognized
 * value must not fail an otherwise-good config.
 */
UENUM(BlueprintType)
enum class EFlockConfigTag : uint8
{
	Any,
	Gameplay,
	Currency,
	Asset,
	Feature,
	Achievement
};

/** The wire value for a tag ("" for Any, so the caller omits the query parameter). */
FLOCK_API FString FlockConfigTagToWire(EFlockConfigTag Tag);

/**
 * A game config record (OpenAPI GameConfigSchema). Its `data` becomes an FFlockStructuredData; its `schema`
 * is kept as verbatim JSON (SchemaJson) — inert now, and the exact payload the coming codegen consumes, so
 * it must not be mangled. Neither member can arrive through reflection, so the model supplies its own
 * FromWireObject (picked up by FFlockJsonUtils::WireObjectToStruct). Tag stays a plain string (see above).
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockGameConfigSchema
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString GameId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString GameVersionId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Tag;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString CreatedAt;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString UpdatedAt;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FFlockStructuredData Data;

	/** The config's `schema` array as verbatim JSON (empty when absent). For codegen; not transformed. */
	UPROPERTY()
	FString SchemaJson;

	/** Builds the model from a wire object, flattening `data` and keeping `schema` verbatim. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockGameConfigSchema& OutStruct, FString& OutError);
};

/**
 * A game patch record (OpenAPI GamePatchSchema): a config's overriding values for one game version. Same
 * opaque data handle as a config, keyed by GameConfigId, with no schema of its own.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockGamePatchSchema
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString GameConfigId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString CreatedAt;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString UpdatedAt;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FFlockStructuredData Data;

	/** Builds the model from a wire object, flattening `data`. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockGamePatchSchema& OutStruct, FString& OutError);
};
