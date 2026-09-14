// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Models/FlockAnalyticsModels.h"

/**
 * One spooled gameplay event: the wire event plus what only the device knows about it.
 *
 * Not a wire model. LocalSessionId lets a session's server id, learned after the event was recorded, still be
 * attached when it is sent. FailedSends counts answered failed sends, and is persisted so the limit spans relaunches.
 */
struct FFlockSpooledAnalyticsEvent
{
	FFlockAnalyticsEventRequest Event;
	FString LocalSessionId;
	int32 FailedSends = 0;
};

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

	/**
	 * A spooled log entry's answered failed sends. Kept in the spool payload under a key the wire serializer
	 * never writes, so the count survives relaunches without ever reaching the server. 0 when absent.
	 */
	static int32 ReadFailedSendCount(const FString& Payload);
	/** The same payload with its attempt count set. Returns the payload unchanged when it is not a JSON object. */
	static FString WithFailedSendCount(const FString& Payload, int32 FailedSends);

	// ── Analytics events ──
	// Same reasoning as the log format: properties are game-authored, so they are spliced in verbatim rather
	// than passed through the snake_case exporter.

	/** One wire event. `properties` is always an object; empty category, session and timestamp are omitted. */
	static TSharedRef<FJsonObject> AnalyticsEventToJson(const FFlockAnalyticsEventRequest& Event);
	/** Condensed `{"events":[...]}` body for `POST analytics/events`. Device-only fields never appear here. */
	static FString SerializeAnalyticsEvents(const TArray<FFlockAnalyticsEventRequest>& Events);

	/** The spool's on-disk format: the wire event plus the local session id and the attempt count. */
	static FString SerializeSpooledAnalyticsEvent(const FFlockSpooledAnalyticsEvent& Entry);
	/** False for anything that is not a spooled event with a name — an entry that can never be delivered. */
	static bool DeserializeSpooledAnalyticsEvent(const FString& Json, FFlockSpooledAnalyticsEvent& OutEntry);

	// ── Session snapshots ──
	// The session-end spool's on-disk format, and the live-session record inside the session state
	// file. Both formats live here for the same reason the log-event one does: a spooled record and
	// the code that reads it can never drift when there is only one place to change.

	/**
	 * Unlike a log event, a snapshot is a fixed-field struct with no caller-authored maps, so it can
	 * go through the generic snake_case exporter without a game-authored key being rewritten.
	 */
	static FString SerializeSnapshot(const FFlockSessionSnapshot& Snapshot);
	static bool DeserializeSnapshot(const FString& Json, FFlockSessionSnapshot& OutSnapshot);

	/** The same format as an object, for a snapshot nested inside a larger document. */
	static TSharedPtr<FJsonObject> SnapshotToJson(const FFlockSessionSnapshot& Snapshot);
	static bool SnapshotFromJson(const TSharedRef<FJsonObject>& Object, FFlockSessionSnapshot& OutSnapshot);
};
