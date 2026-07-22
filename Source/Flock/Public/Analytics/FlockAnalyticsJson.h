// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Models/FlockAnalyticsModels.h"

/**
 * Builds and reads the `log_event` wire bodies, and is also the on-disk format for the offline
 * spool — one shape for both so a spooled entry and a live one can never drift.
 *
 * Why this exists instead of FFlockJsonUtils::StructToWireJson: that path runs every key through
 * ToSnakeCase, recursing into nested objects. The `error_data` and `extra_data` members are
 * game-authored free-form maps, so a caller's `playerLevel` key would silently ship as
 * `player_level` (and come back as `PlayerLevel`). Here the SDK's own keys are written literally
 * and the caller's maps are spliced in untouched.
 *
 * Members are omitted when empty rather than sent blank, matching the rest of the SDK's bodies.
 */
class FLOCK_API FFlockAnalyticsJson
{
public:
	/** `exception` / `logic_error` / `debug` — the wire spellings, which are not the enum names. */
	static FString LogEventTypeToWire(EFlockLogEventType Type);
	/** Unknown or empty input falls back to Debug, so a hand-edited spool file can't fail a flush. */
	static EFlockLogEventType WireToLogEventType(const FString& Wire);

	static TSharedRef<FJsonObject> ToJson(const FFlockLogEventRequest& Event);
	static bool FromJson(const TSharedRef<FJsonObject>& Object, FFlockLogEventRequest& OutEvent);

	/** Condensed body for `POST log_event/single`. */
	static FString SerializeEvent(const FFlockLogEventRequest& Event);
	/** Condensed `{"events":[...]}` body for `POST log_event`. */
	static FString SerializeEvents(const TArray<FFlockLogEventRequest>& Events);
	static bool DeserializeEvent(const FString& Json, FFlockLogEventRequest& OutEvent);
};
