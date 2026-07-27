// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "FlockCommandModels.generated.h"

/**
 * One typed value bound for a game command's `value` field (the wire types it as Any).
 *
 * The value is held as a one-field JSON object — {"value": 7} — rather than a bare fragment, because a
 * JSON writer can only emit inside an object or array: wrapping lets the serializer do every bit of
 * escaping instead of hand-rolling it, and keeps the whole thing a reflected FString so a queued command
 * round-trips through the offline store.
 *
 * Implicitly constructible from the scalar types a game actually writes, so C++ call sites read plainly:
 *
 *     Commands->UpdatePlayerDataField(RowId, TEXT("gold"), 100, OnDone);
 *
 * Blueprint builds one with the Flock Command Value (Int/Float/String/Bool/String Array) nodes on
 * UFlockCommandDataLibrary.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockCommandValue
{
	GENERATED_BODY()

	/** The value wrapped as {"value": ...}. Reflected so a queued command survives a snapshot round-trip. */
	UPROPERTY()
	FString WrappedJson;

	FFlockCommandValue() = default;

	FFlockCommandValue(int32 Value);
	FFlockCommandValue(int64 Value);
	FFlockCommandValue(float Value);
	FFlockCommandValue(double Value);
	FFlockCommandValue(bool Value);
	FFlockCommandValue(const FString& Value);

	/**
	 * Present so a string literal binds here rather than to the bool overload. Without it,
	 * UpdatePlayerDataField(Id, TEXT("title"), TEXT("Champion")) would silently write `true`.
	 */
	FFlockCommandValue(const TCHAR* Value);

	FFlockCommandValue(const TArray<FString>& Value);

	/** Adopts a caller-authored JSON fragment verbatim (an object, an array, a nested shape). */
	static FFlockCommandValue FromRawJson(const FString& Json);

	/** Wraps an already-built JSON value. */
	static FFlockCommandValue FromJsonValue(const TSharedPtr<FJsonValue>& Value);

	/** Unwraps to the JSON value to send; a null value when this was never assigned. */
	TSharedPtr<FJsonValue> ToJsonValue() const;

	/** The value as it will appear on the wire (e.g. `7`, `"Champion"`, `["a","b"]`). */
	FString ToJsonString() const;
};

/**
 * The field bag for a generic player-data mutation: the flat {field: value} object the update_player_data
 * command expects (the backend rejects the DataField array form).
 *
 * Keys are author-supplied template field names and are kept **verbatim** — never case-transformed — for
 * the same reason a dict node's keys are in FFlockStructuredData: only the author knows what the backend
 * stores them as. Values keep their JSON type, so an int stays an int.
 *
 *     Commands->UpdatePlayerData(RowId,
 *         FFlockCommandData().Set(TEXT("gold"), 250).Set(TEXT("prestige"), true), OnDone);
 *
 * Chained temporaries are safe to pass straight into a call. Blueprint builds the same bag with the
 * chainable Flock Command Data Set (Int/Float/String/Bool/String Array) nodes.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockCommandData
{
	GENERATED_BODY()

	/** The fields as a JSON object string. Reflected, so a queued command survives a snapshot round-trip. */
	UPROPERTY()
	FString FieldsJson;

	FFlockCommandData& Set(const FString& Key, const FFlockCommandValue& Value);

	FFlockCommandData& Set(const FString& Key, int32 Value) { return Set(Key, FFlockCommandValue(Value)); }
	FFlockCommandData& Set(const FString& Key, int64 Value) { return Set(Key, FFlockCommandValue(Value)); }
	FFlockCommandData& Set(const FString& Key, float Value) { return Set(Key, FFlockCommandValue(Value)); }
	FFlockCommandData& Set(const FString& Key, double Value) { return Set(Key, FFlockCommandValue(Value)); }
	FFlockCommandData& Set(const FString& Key, bool Value) { return Set(Key, FFlockCommandValue(Value)); }
	FFlockCommandData& Set(const FString& Key, const FString& Value) { return Set(Key, FFlockCommandValue(Value)); }

	/** Same literal-binding guard as FFlockCommandValue's — keeps TEXT("x") off the bool overload. */
	FFlockCommandData& Set(const FString& Key, const TCHAR* Value) { return Set(Key, FFlockCommandValue(Value)); }

	FFlockCommandData& Set(const FString& Key, const TArray<FString>& Value) { return Set(Key, FFlockCommandValue(Value)); }

	/** Sets a field from caller-authored JSON (a nested object or list the typed setters can't express). */
	FFlockCommandData& SetRawJson(const FString& Key, const FString& Json) { return Set(Key, FFlockCommandValue::FromRawJson(Json)); }

	/** True when no field has been set. */
	bool IsEmpty() const;

	/** The field names, in no particular order. */
	TArray<FString> GetFieldNames() const;

	/** The fields as a JSON object (never null — an empty bag yields an empty object). */
	TSharedRef<FJsonObject> ToJsonObject() const;

	/** The fields as the JSON object string that goes on the wire. */
	FString ToJsonString() const { return FieldsJson.IsEmpty() ? TEXT("{}") : FieldsJson; }

	/** Adopts a whole JSON object string (the inverse of ToJsonString). Invalid JSON yields an empty bag. */
	static FFlockCommandData FromJsonString(const FString& Json);
};

/**
 * One queued mutation awaiting replay: the endpoint it goes to and the exact body that would have been
 * posted, so a replay cannot drift from the live call. Persisted as a list under a player-scoped snapshot
 * key — a player's offline writes must never replay under another player's auth.
 *
 * Internal; not a Blueprint type. Money commands are never queued (see FFlockCommandProvider).
 */
USTRUCT()
struct FLOCK_API FFlockPendingCommand
{
	GENERATED_BODY()

	/** Endpoint path relative to the versioned API URL (a FlockEndpoints::Command* constant). */
	UPROPERTY()
	FString Path;

	/** The request body, serialized at enqueue time. */
	UPROPERTY()
	FString PayloadJson;

	/** Log context for the write ("Update player data"), carried so a replay logs like the original. */
	UPROPERTY()
	FString Context;

	/** The `player_data_id` the body targets; empty when it had none. Cached so a replay needn't re-parse. */
	UPROPERTY()
	FString PlayerDataId;
};
