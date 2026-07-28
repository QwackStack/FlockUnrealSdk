// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "JsonObjectConverter.h"
#include "FlockStructuredData.generated.h"

/**
 * A provider's resolved structured data as an opaque, read-mostly handle (OverlayFields is the single write
 * path, for the optimistic row an offline command leaves). The backend delivers such data as a
 * recursive list[DataField] tree (each node typed object/list/dict/scalar); this collapses it once, at
 * parse, into a single flat JSON object — the UE analog of the canonical SDK's ToFlatObject — and holds
 * that. Shared across features: a game config's/patch's `data` and a player template's/row's `data` all use
 * it.
 *
 * The collapse is type-aware, which is the whole point: an object/list node's children are field names and
 * are snake->Pascal cased so a (future codegen) USTRUCT binds by reflection, while a dict node's children
 * are author-supplied data keys and are kept verbatim. A blind key transform cannot tell those apart and
 * would corrupt every dictionary — so this never routes the data through FFlockJsonUtils' wire transform.
 *
 * The flat object is stored as a JSON string (FlatJson) rather than a live object so the whole thing is
 * reflectable and survives a snapshot round-trip; it is parsed lazily on first read and the parse is cached.
 * Read values with GetDataAs<T> (C++) or UFlockStructuredDataLibrary (Blueprint).
 *
 * FFlockJsonData is the plain-object sibling (a flat blob with no DataField-tree flatten and no codegen
 * hooks); this one adds both.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockStructuredData
{
	GENERATED_BODY()

	/** The flattened data as a JSON object string. Reflected so a snapshot round-trips; read via the API below. */
	UPROPERTY()
	FString FlatJson;

	/** Builds the handle from a wire `data` node (the list[DataField] tree, a legacy flat dict, or null). */
	static FFlockStructuredData FromWireData(const TSharedPtr<FJsonValue>& DataValue);

	/** True when there is a parsed, non-empty data object. */
	bool IsValid() const;

	/**
	 * Overlays top-level fields onto the flattened data — the one write path on an otherwise read-only
	 * handle, used for the optimistic row a queued offline command leaves behind.
	 *
	 * Each incoming key replaces an existing field matched exact-first-then-Pascal (the same resolution the
	 * dotted reads use, so a caller's "max_health" updates the flattened "MaxHealth"), or is added verbatim
	 * when nothing matches. Rebuilds FlatJson rather than mutating the cached parse, because copies of a
	 * handle share that parse — overwriting it in place would rewrite data other copies still hold.
	 */
	void OverlayFields(const TSharedRef<FJsonObject>& Fields);

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
