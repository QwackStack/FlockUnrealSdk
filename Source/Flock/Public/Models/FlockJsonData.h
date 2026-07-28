// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "JsonObjectConverter.h"
#include "FlockJsonData.generated.h"

/**
 * A read-only handle over an opaque JSON object — the SDK's answer to a wire field that is a free-form
 * dictionary (a game-authored `data`/`stats` blob) rather than a fixed schema.
 *
 * UE reflection has no equivalent of a heterogeneous map-of-anything, so the object is held as a JSON
 * string (reflectable, so it round-trips the offline snapshot untouched) and read through typed
 * dotted-path accessors — no manual JSON parsing at the call site, and Blueprint gets the same reads via
 * UFlockJsonDataLibrary. This is the general-purpose sibling of the config handle FFlockStructuredData:
 * that one additionally flattens a config's DataField tree and carries codegen hooks; this one is a plain
 * pass-through for any provider's free-form data.
 *
 * Author-supplied keys are kept verbatim (never snake<->Pascal transformed): a model carrying one of
 * these builds it inside its own FromWireObject, so the wire key transform never reaches the blob. Read
 * values with TryGet* (C++) or the Blueprint library; a nested object can be pulled straight into a
 * matching USTRUCT with GetAs<T>.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockJsonData
{
	GENERATED_BODY()

	/** The JSON object as a string. Reflected so it survives a snapshot round-trip; read via the API below. */
	UPROPERTY()
	FString Json;

	/** Builds the handle from a wire JSON value — an object is kept verbatim; anything else yields an empty handle. */
	static FFlockJsonData FromJson(const TSharedPtr<FJsonValue>& Value);

	/** True when there is a parsed, non-empty object. */
	bool IsValid() const;

	/** The backing JSON as a string (empty when there is none). */
	FString ToJsonString() const { return Json; }

	// Dotted-path reads (e.g. "stats.visits"). Each segment resolves exact-first then Pascal-cased, so a
	// dashboard's snake_case key is found whether the caller types it in snake_case or PascalCase.
	bool TryGetString(const FString& Path, FString& OutValue) const;
	bool TryGetInt(const FString& Path, int32& OutValue) const;
	bool TryGetFloat(const FString& Path, float& OutValue) const;
	bool TryGetBool(const FString& Path, bool& OutValue) const;
	bool TryGetStringArray(const FString& Path, TArray<FString>& OutValue) const;

	/** True when the dotted path resolves to a present (non-null) value. */
	bool HasField(const FString& Path) const;

	/** Top-level field names of the object. */
	TArray<FString> GetFieldNames() const;

	/**
	 * Fills OutStruct from the object by reflection. No key transform — field names are matched as stored,
	 * so a USTRUCT whose PascalCase fields match the object's keys binds directly. False when there is no data.
	 */
	template <typename T>
	bool GetAs(T& OutStruct) const
	{
		const TSharedPtr<FJsonObject> Object = ResolveObject();
		if (!Object.IsValid())
		{
			return false;
		}
		return FJsonObjectConverter::JsonObjectToUStruct(Object.ToSharedRef(), T::StaticStruct(), &OutStruct, 0, 0);
	}

private:
	/** Parses Json on first use and caches it. Null when Json is empty or unparseable. */
	TSharedPtr<FJsonObject> ResolveObject() const;

	/** Walks the dotted path, returning the resolved value (invalid ptr on a miss). */
	TSharedPtr<FJsonValue> ResolvePath(const FString& Path) const;

	/** Cached parse of Json. Mutable so const reads can populate it; shared on copy (data is immutable). */
	mutable TSharedPtr<FJsonObject> CachedObject;
	mutable bool bParsed = false;
};
