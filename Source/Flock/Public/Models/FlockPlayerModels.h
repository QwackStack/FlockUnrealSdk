// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Models/FlockStructuredData.h"
#include "FlockPlayerModels.generated.h"

/**
 * A player template (OpenAPI PlayerTemplateSchema): the schema and default data for one kind of
 * per-player record (a currency wallet, an achievement set, ...). Its `data` is the same recursive
 * list[DataField] tree a game config carries, so it reuses the config data handle (FFlockStructuredData);
 * its `schema` is kept as verbatim JSON for the coming codegen. Neither can arrive by reflection, so the
 * model supplies its own FromWireObject (picked up by FFlockJsonUtils::WireObjectToStruct). Tag stays a
 * plain string — an unrecognized value must not fail an otherwise-good template.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockPlayerTemplateSchema
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString GameVersionId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Tag;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FFlockStructuredData Data;

	/** The template's `schema` array as verbatim JSON (empty when absent). For codegen; not transformed. */
	UPROPERTY()
	FString SchemaJson;

	/** Builds the model from a wire object, flattening `data` and keeping `schema` verbatim. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockPlayerTemplateSchema& OutStruct, FString& OutError);
};

/**
 * A single per-player data row (OpenAPI PlayerDataSchema): a player's values for one player template.
 * Its `data` is the same list[DataField] tree as a config, exposed through the shared data handle; a
 * custom FromWireObject keeps it off the reflection path (which would rewrite the flattened keys).
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockPlayerData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString PlayerTemplateId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString GameId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString PlayerId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FFlockStructuredData Data;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString CreatedAt;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString UpdatedAt;

	/** Builds the row from a wire object, flattening `data`. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockPlayerData& OutStruct, FString& OutError);
};

/**
 * One feature's ban entry inside a player ban's `data` map (OpenAPI FeatureBan). All scalar.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockFeatureBan
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Reason;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString BanDuration;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString EffectiveDatetime;

	/** Reads one feature-ban entry ({reason, ban_duration, effective_datetime}). */
	static FFlockFeatureBan FromWire(const TSharedPtr<FJsonObject>& Object);
};

/**
 * A player's ban record (OpenAPI PlayerBanSchema). The `data` map is keyed by feature name — author
 * data, kept verbatim — with a typed FFlockFeatureBan per entry, so the model parses it by hand rather
 * than through the reflection path (which would snake->Pascal the feature keys). An empty Id means the
 * player is not banned: the ban route answers 2xx with a null `result` in that case (see
 * FFlockPlayerBanResponse), and that null becomes an empty record here, not an error.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockPlayerBan
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString PlayerId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString GameId;

	/** Per-feature bans, keyed by the feature name kept verbatim from the wire. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	TMap<FString, FFlockFeatureBan> Data;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString CreatedAt;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString UpdatedAt;

	/** True when this is a real ban record (a non-empty Id) rather than the not-banned empty result. */
	bool IsBanned() const { return !Id.IsEmpty(); }

	/** Builds the record from a bare ban object, keeping `data` feature keys verbatim. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockPlayerBan& OutStruct, FString& OutError);
};

/**
 * Envelope wrapper for GET player-ban. That route is enveloped like the config routes, but its `result`
 * is Union[PlayerBanSchema, None]: a 2xx with `result: null` is a legitimate "not banned", which the
 * shared UnwrapResultToStruct would reject as a missing result. So the ban is fetched raw (GetRaw) into
 * this wrapper, whose custom parse reads `result` and tolerates null — yielding an empty (not-banned)
 * FFlockPlayerBan. Internal; not a Blueprint type.
 */
USTRUCT()
struct FLOCK_API FFlockPlayerBanResponse
{
	GENERATED_BODY()

	UPROPERTY()
	FFlockPlayerBan Ban;

	/** Reads the envelope, parsing `result` into Ban when present and leaving it empty when null. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockPlayerBanResponse& OutStruct, FString& OutError);
};

/**
 * One page of player-data rows ({items, total, page, limit}). A concrete USTRUCT rather than the client's
 * template TFlockPage<T>, for the same reasons as FFlockShopPage: it must be a Blueprint type and
 * round-trip by reflection, neither of which a template can be. The provider copies the client's
 * TFlockPage<FFlockPlayerData> into this.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockPlayerDataPage
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	TArray<FFlockPlayerData> Items;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 Total = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 Page = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 Limit = 0;
};
