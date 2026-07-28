// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockAnalyticsModels.generated.h"

/**
 * Device facts captured once per session (the wire `device_info` object). Field names map from the
 * snake_case wire via FFlockJsonUtils (operating_system -> OperatingSystem). Populated by the
 * analytics feature when it lands; declared here so FFlockSessionSnapshot is complete.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockDeviceInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Platform;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString OperatingSystem;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString DeviceModel;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString DeviceType;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString AppVersion;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 ScreenWidth = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 ScreenHeight = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	float ScreenDpi = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString SystemLanguage;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString GraphicsDeviceName;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 SystemMemoryMb = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString SdkVersion;
};

/**
 * Final metrics for one gameplay/analytics session (duration, screens, pauses, FPS). Carried by
 * UFlockEvents::OnSessionEnded and persisted for crash recovery once the session feature lands.
 * Dates are raw ISO-8601 strings for now (empty when absent), matching the other wire models.
 *
 * Bool fields have no `b` prefix on purpose: these are wire models, and the snake_case mapping is
 * bijective on the plain name (is_active <-> IsActive); the JSON layer has no per-field overrides yet.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockSessionSnapshot
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString SessionId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString ServerSessionId;

	/** Lets recovery register a session that never obtained a server id. Empty in older snapshots. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString PlayerId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 SessionNumber = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString StartTimeUtc;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString EndTimeUtc;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString LastHeartbeatUtc;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	float DurationSeconds = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	float TotalPauseDurationSeconds = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 PauseCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 ScreensViewed = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	TArray<FString> ScreenNames;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	float AverageFps = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	float MinFps = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	float MaxFps = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FFlockDeviceInfo DeviceInfo;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	bool IsActive = false;

	/** True when the session was shorter than the configured bounce threshold. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	bool IsBounce = false;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	bool IsFirstSession = false;
};

/**
 * Which of the three log channels an entry belongs to. The wire spelling is snake_case and is NOT
 * the enum name, so it is mapped explicitly by FFlockAnalyticsJson rather than by the generic
 * struct exporter (which would emit "LogicError").
 */
UENUM(BlueprintType)
enum class EFlockLogEventType : uint8
{
	/** An unhandled or reported exception, with a stack trace. Wire: `exception`. */
	Exception,
	/** A recoverable logic fault the game chose to report. Wire: `logic_error`. */
	LogicError,
	/** A plain diagnostic message. Wire: `debug`. */
	Debug
};

/**
 * The `data` member of a log event. Only Type is required; every other member is omitted from the
 * body when empty.
 *
 * ErrorData/ExtraData are caller-supplied free-form maps. Their keys are game-authored and must
 * reach the backend byte-for-byte, so these two members deliberately bypass the snake_case key
 * transform — see the note on FFlockAnalyticsJson.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockLogEventData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	EFlockLogEventType Type = EFlockLogEventType::Debug;

	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString GameVersion;

	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString LogicalExpression;

	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString ErrorMessage;

	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString ErrorCode;

	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	TMap<FString, FString> ErrorData;

	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString ErrorTraceback;

	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	TArray<FString> ErrorTracebackLines;

	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	TMap<FString, FString> ExtraData;
};

/**
 * The optional detail on a logged error or exception — one named argument instead of four
 * positional ones.
 *
 * Two same-typed maps sitting side by side in a parameter list are trivially swapped and the
 * compiler cannot catch it, which is why these are named fields. It also gives Blueprint a single
 * pin instead of four.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockLogDetails
{
	GENERATED_BODY()

	/** The invariant that failed, e.g. `hp > 0`. Meaningful on errors. */
	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString LogicalExpression;

	/** Your own code for this fault, e.g. `INV_DESYNC`. Meaningful on errors. */
	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString ErrorCode;

	/** Structured facts about the fault itself — *what* was wrong (expected vs actual, ids, counts). */
	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	TMap<FString, FString> ErrorData;

	/** Context about the moment — *where* the player was and what they were doing. */
	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	TMap<FString, FString> ExtraData;
};

/** One entry for `POST log_event/single`, and one element of the batch for `POST log_event`. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockLogEventRequest
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString Message;

	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FFlockLogEventData Data;

	/** ISO-8601 UTC. Stamped when the entry is created, not when it is finally delivered. */
	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString Timestamp;
};

/** Batch body for `POST log_event`. */
USTRUCT()
struct FLOCK_API FFlockLogEventsRequest
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FFlockLogEventRequest> Events;
};

// ── Session wire models ──

/** Body for `POST analytics/sessions`. PlayerId is the only member the backend requires. */
USTRUCT()
struct FLOCK_API FFlockSessionStartRequest
{
	GENERATED_BODY()

	UPROPERTY() FString PlayerId;
	UPROPERTY() FString Platform;
	UPROPERTY() FString DeviceType;
	UPROPERTY() FString GameVersionId;
	UPROPERTY() FString StartedAt;
};

/**
 * Response of `POST analytics/sessions` — the one analytics route that returns a typed body. It is
 * NOT enveloped (no `result` member), so it is read with the raw verbs.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockSessionStartResponse
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString SessionId;
};

/** Body for `PATCH analytics/sessions/{session_id}`. Every member is optional to the backend. */
USTRUCT()
struct FLOCK_API FFlockSessionEndRequest
{
	GENERATED_BODY()

	UPROPERTY() int32 DurationSeconds = 0;
	UPROPERTY() int32 ScreensViewed = 0;
	UPROPERTY() bool IsBounce = false;
	UPROPERTY() FString EndedAt;
};

/**
 * Body for `POST analytics/transactions` — a purchase recorded for revenue/LTV metrics. PlayerId and
 * Amount are the only members the backend requires; the optional strings are dropped from the body when
 * empty (StructToWireJson with bOmitEmptyStrings). Fixed-field, so the snake_case wire transform is safe
 * here (unlike a log event's caller-supplied maps). The shop provider drives this around a purchase; it
 * can also be sent directly. See FFlockAnalyticsProvider::RecordTransaction.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockAnalyticsTransactionRequest
{
	GENERATED_BODY()

	/** Resolved to the signed-in player when left empty. */
	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString PlayerId;

	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	double Amount = 0.0;

	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString CurrencyId;

	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString CurrencyCode = TEXT("USD");

	/** Resolved to the running session when left empty. */
	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString SessionId;

	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString ShopItemId;

	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	int32 Quantity = 1;

	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString TransactionType = TEXT("purchase");

	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString Status = TEXT("completed");

	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString PaymentProvider;

	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString ExternalTransactionId;

	/** ISO-8601 UTC. Stamped when the record is created when left empty. */
	UPROPERTY(BlueprintReadWrite, Category = "Flock")
	FString CreatedAt;
};

/**
 * Stand-in for the analytics routes that answer with a free-form object nobody reads (session end,
 * log event delivery). Deserializing into an empty struct succeeds against any object body, which
 * is what makes these calls fire-and-forget: success is the 2xx, not the payload.
 */
USTRUCT()
struct FLOCK_API FFlockAnalyticsAck
{
	GENERATED_BODY()
};
