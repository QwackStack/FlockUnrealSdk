// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Models/FlockJsonData.h"
#include "FlockNotificationModels.generated.h"

/**
 * One notification in a player's inbox.
 *
 * `Type` and `Severity` are plain strings, not enums: the API declares both as unconstrained strings, so
 * an enum here would have to invent a closed set the server does not promise. That is the opposite call
 * from the leaderboard enums, which the spec does constrain — and the reason is the spec, not taste.
 *
 * `ReadAt` doubles as the read flag. It is a nullable ISO-8601 timestamp on the wire; absent arrives as an
 * empty string, never the literal "null". Read it through IsRead() rather than comparing the string.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockNotification
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString StudioId;

	/** Nullable on the wire — a studio-wide notification carries no game. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString GameId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString RecipientType;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString RecipientId;

	/** Author-defined category, e.g. "reward" or "friend_request". Unconstrained by the API. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Type;

	/** Usually info/warning/critical, but the API types this as a plain string, so it is not an enum. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Severity;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Title;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Body;

	/** Free-form payload the sender attached — author keys kept verbatim, read with the JSON Data library. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FFlockJsonData Data;

	/** Raw ISO-8601 timestamp, or empty when unread. Kept as the server sent it (same as an asset's UpdatedAt). */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString ReadAt;

	/** Set when this notification came from a campaign rather than a direct send. Nullable on the wire. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString CampaignId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString CreatedAt;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString UpdatedAt;

	/** True once the server has stamped read_at. The only supported way to ask. */
	bool IsRead() const { return !ReadAt.IsEmpty(); }

	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockNotification& OutStruct, FString& OutError);
};

/**
 * One page of the inbox ({items, total, page, limit}). A concrete USTRUCT rather than the template
 * TFlockPage<T>: it must round-trip the offline snapshot by reflection and be a Blueprint type, neither of
 * which a template can be. The provider copies the client's TFlockPage<FFlockNotification> into this.
 *
 * Total is the player's full inbox count for the current filter, not Items.Num().
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockNotificationPage
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	TArray<FFlockNotification> Items;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 Total = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 Page = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 Limit = 0;
};

/**
 * The bell-icon payload: how many are unread, plus the most recent few. One call instead of a count and a
 * first page, which is why it exists alongside both.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockNotificationSummary
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 UnreadCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	TArray<FFlockNotification> Items;

	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockNotificationSummary& OutStruct, FString& OutError);
};

/** How many rows a mark-all-read actually flipped. Zero is a success, not a failure — nothing was unread. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockMarkAllReadResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 Updated = 0;
};

/** The unread-count route's payload. Reflection-parsed: `count` maps straight onto Count. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockUnreadCount
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 Count = 0;
};

/**
 * A schedule this install created that has not reached its delivery time yet.
 *
 * **Not a wire model** — nothing sends or receives this, and it is not a server query. The id handed back
 * by a schedule call is the only handle on a pending reminder, and `/v1` has no route to list or read one
 * back, so the SDK persists what it scheduled. That also bounds what this can know: it sees only what
 * *this install* scheduled, and it infers delivery from the clock because there is nothing to ask.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockPendingSchedule
{
	GENERATED_BODY()

	/** The scheduled id, which is what `CancelScheduled` takes. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Id;

	/** The template name that was scheduled, kept so a caller can tell entries apart without another call. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString TemplateName;

	/** The id that name resolved to when the schedule was created. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString TemplateId;

	/** Raw ISO-8601, stored exactly as the server echoed it — never the value the caller asked for. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString DeliverAt;
};

/**
 * How far the SDK has already announced this player's inbox, so a read can tell a genuinely new
 * notification from one the game was told about on a previous run.
 *
 * **Not a wire model** — nothing sends or receives this. It is persisted through the snapshot store's
 * plain-JSON path and is *state, not cache*: `ClearCache()` deliberately preserves it, because losing it
 * would make the next read either re-announce a whole inbox or swallow everything up to that point.
 *
 * Bool named without the `b` prefix to match the serialized-model convention used by the wire structs it
 * sits beside, since it round-trips through the same reflection path.
 */
USTRUCT()
struct FLOCK_API FFlockNotificationWatermark
{
	GENERATED_BODY()

	/** False until the first fetch for this player, which seeds silently rather than replaying the inbox. */
	UPROPERTY()
	bool Seeded = false;

	/** Newest `created_at` surfaced so far; empty when the inbox was empty at seed time, which makes everything later new. */
	UPROPERTY()
	FString NewestCreatedAt;
};

/**
 * A notification template a game can schedule against, as the client sees it.
 *
 * Deliberately thin — the client route exposes only what a game needs to *pick* a template. The body,
 * placeholders and channel defaults live server-side and are the dashboard's business, not the SDK's.
 *
 * All three members are single words on the wire, so this takes the reflection path rather than a custom
 * `FromWireObject`.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockNotificationTemplate
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Id;

	/** What a designer schedules against, and what the by-name route takes. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Category;
};

/**
 * Where a scheduled notification gets delivered.
 *
 * This one *is* an enum, unlike FFlockNotification's Type and Severity: the schedule request's `channels`
 * is declared as a closed set (in_app / email / push) in the spec, so the set is the server's promise
 * rather than this SDK's guess. The rule throughout is that the spec decides.
 */
UENUM(BlueprintType)
enum class EFlockNotificationChannel : uint8
{
	InApp,
	Email,
	Push
};

/** The wire spelling for a channel. */
FLOCK_API const TCHAR* FlockNotificationChannelToWire(EFlockNotificationChannel Channel);

/**
 * A notification the game asked the backend to deliver later.
 *
 * `Channels` is TArray<FString> rather than the enum, deliberately: the *request* declares a closed set,
 * but the *response* types the same field as plain strings. Parsing a response into an enum that fails on
 * an unrecognized value would turn a server-side addition into a client parse failure, so the read side
 * stays verbatim while the write side stays typed.
 *
 * Delivery state is read off the timestamps, not the loose `status` string — a status the server adds
 * later would silently break a string comparison, whereas the timestamps are structural.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockScheduledNotification
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString GameId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString StudioId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString PlayerId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString TemplateId;

	/** The values that fill the template's placeholders. Author keys kept verbatim. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FFlockJsonData Variables;

	/** Verbatim wire strings — see the note above on why this is not the enum. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	TArray<FString> Channels;

	/** Raw ISO-8601 timestamp for when the server will deliver this. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString DeliverAt;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Status;

	/** Who created it — an SDK-scheduled reminder reads differently from a campaign fan-out. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Source;

	/** Set once delivered: the inbox row this became. Nullable on the wire. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString NotificationId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString DeliveredAt;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString CanceledAt;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString CreatedAt;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString UpdatedAt;

	/** Delivered already — read from the timestamp, not the status string. */
	bool IsDelivered() const { return !DeliveredAt.IsEmpty(); }

	/** Canceled — likewise structural rather than a string compare. */
	bool IsCanceled() const { return !CanceledAt.IsEmpty(); }

	/** Still waiting to fire: neither delivered nor canceled. */
	bool IsPending() const { return !IsDelivered() && !IsCanceled(); }

	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockScheduledNotification& OutStruct, FString& OutError);
};

/**
 * A platform the push backend can deliver to.
 *
 * Closed set in the spec, so it is an enum — and a deliberately short one. There is no Windows, Mac,
 * Linux or console member because the backend accepts none of them, and inventing one would let a caller
 * register a token under a platform that silently never delivers.
 */
UENUM(BlueprintType)
enum class EFlockDevicePlatform : uint8
{
	Android,
	IOS,
	Web
};

/** The wire spelling for a device platform. */
FLOCK_API const TCHAR* FlockDevicePlatformToWire(EFlockDevicePlatform Platform);

/**
 * Maps an engine platform name (FPlatformProperties::IniPlatformName) onto a platform the push backend
 * accepts. Returns false for anything it does not — Windows, Mac, Linux, console, and the editor.
 *
 * Pure and string-in so the mapping is testable without running on a device, which is the only way to
 * cover it at all: the real call sites can never execute in an automation run.
 */
FLOCK_API bool FlockTryResolveDevicePlatform(const FString& IniPlatformName, EFlockDevicePlatform& OutPlatform);

/**
 * A device registered to receive push for the signed-in player.
 *
 * `Platform` is an FString, not the enum: the *request* declares a closed set but the *response* types the
 * same field as a plain string — the same asymmetry as a scheduled notification's channels, handled the
 * same way.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockDeviceToken
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString PlayerId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString GameId;

	/** Verbatim wire string — see the note above on why this is not the enum. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Platform;

	/** False once unregistered; the row is deactivated rather than deleted. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	bool IsActive = false;

	/** Raw ISO-8601 timestamp, or empty when the server has not stamped one. Nullable on the wire. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString LastSeenAt;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString CreatedAt;

	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockDeviceToken& OutStruct, FString& OutError);
};

/** Whether an unregister actually deactivated a row. False means there was nothing to deactivate. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockUnregisterDeviceTokenResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	bool Deactivated = false;
};
