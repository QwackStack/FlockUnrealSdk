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
