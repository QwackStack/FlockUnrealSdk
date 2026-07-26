// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "JsonObjectConverter.h"
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
 * A config's resolved data as an opaque, read-only handle. The backend delivers `data` as a recursive
 * list[DataField] tree (each node typed object/list/dict/scalar); this collapses it once, at parse, into a
 * single flat JSON object — the UE analog of the Unity SDK's ToFlatObject — and holds that.
 *
 * The collapse is type-aware, which is the whole point: an object/list node's children are field names and
 * are snake->Pascal cased so a (future codegen) USTRUCT binds by reflection, while a dict node's children
 * are author-supplied data keys and are kept verbatim. A blind key transform cannot tell those apart and
 * would corrupt every dictionary — so this never routes config data through FFlockJsonUtils' wire transform.
 *
 * The flat object is stored as a JSON string (FlatJson) rather than a live object so the whole thing is
 * reflectable and survives a snapshot round-trip; it is parsed lazily on first read and the parse is cached.
 * Read values with GetDataAs<T> (C++) or UFlockGameConfigLibrary (Blueprint).
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockGameConfigData
{
	GENERATED_BODY()

	/** The flattened data as a JSON object string. Reflected so a snapshot round-trips; read via the API below. */
	UPROPERTY()
	FString FlatJson;

	/** Builds the handle from a wire `data` node (the list[DataField] tree, a legacy flat dict, or null). */
	static FFlockGameConfigData FromWireData(const TSharedPtr<FJsonValue>& DataValue);

	/** True when there is a parsed, non-empty data object. */
	bool IsValid() const;

	/** The flattened data as a JSON string (empty when there is none). */
	FString ToJsonString() const { return FlatJson; }

	/**
	 * Fills OutStruct from the flattened data by reflection. No key transform — the flatten already produced
	 * PascalCase field names, so a USTRUCT with matching fields binds directly. False when there is no data.
	 */
	template <typename T>
	bool GetDataAs(T& OutStruct) const
	{
		const TSharedPtr<FJsonObject> Object = ResolveObject();
		if (!Object.IsValid())
		{
			return false;
		}
		return FJsonObjectConverter::JsonObjectToUStruct(Object.ToSharedRef(), T::StaticStruct(), &OutStruct, 0, 0);
	}

	// Dotted-path reads (e.g. "stats.max_health"). Each segment resolves exact-first then Pascal-cased, so a
	// dashboard name ("max_health") finds the flattened key ("MaxHealth") and a Pascal name finds it directly.
	bool TryGetString(const FString& Path, FString& OutValue) const;
	bool TryGetInt(const FString& Path, int32& OutValue) const;
	bool TryGetFloat(const FString& Path, float& OutValue) const;
	bool TryGetBool(const FString& Path, bool& OutValue) const;
	bool TryGetStringArray(const FString& Path, TArray<FString>& OutValue) const;

	/** True when the dotted path resolves to a present (non-null) value. */
	bool HasField(const FString& Path) const;

	/** Top-level field names of the flattened data. */
	TArray<FString> GetFieldNames() const;

private:
	/** Parses FlatJson on first use and caches it. Null when FlatJson is empty or unparseable. */
	TSharedPtr<FJsonObject> ResolveObject() const;

	/** Walks the dotted path, returning the resolved value (invalid ptr on a miss). */
	TSharedPtr<FJsonValue> ResolvePath(const FString& Path) const;

	/** Cached parse of FlatJson. Mutable so const reads can populate it; shared on copy (data is immutable). */
	mutable TSharedPtr<FJsonObject> CachedObject;
	mutable bool bParsed = false;
};

/**
 * A game config record (OpenAPI GameConfigSchema). Its `data` becomes an FFlockGameConfigData; its `schema`
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
	FFlockGameConfigData Data;

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
	FFlockGameConfigData Data;

	/** Builds the model from a wire object, flattening `data`. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockGamePatchSchema& OutStruct, FString& OutError);
};
