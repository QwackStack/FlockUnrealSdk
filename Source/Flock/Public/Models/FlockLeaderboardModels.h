// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "FlockLeaderboardModels.generated.h"

/**
 * What a board's scores measure. Duration scores are in seconds — the enumerator name says so because the
 * wire value ("duration") does not, and a caller with a bare number has no other way to learn the unit.
 */
UENUM(BlueprintType)
enum class EFlockLeaderboardValueType : uint8
{
	Integer,
	Float,
	DurationSeconds
};

/** Which end of the scale wins — high score or best time. */
UENUM(BlueprintType)
enum class EFlockLeaderboardDirection : uint8
{
	Higher,
	Lower
};

/** How repeated writes to the source field fold into one score. */
UENUM(BlueprintType)
enum class EFlockLeaderboardAggregation : uint8
{
	Best,
	Latest,
	Sum
};

/**
 * How a board buckets over time. This is board *configuration* — not the window you read with. Reading
 * takes an FFlockLeaderboardWindow, and these three values are never sent as one.
 */
UENUM(BlueprintType)
enum class EFlockLeaderboardWindowType : uint8
{
	Never,
	Weekly,
	Seasonal
};

/** Whether the board ranks everyone together or per country. */
UENUM(BlueprintType)
enum class EFlockLeaderboardScope : uint8
{
	Global,
	Country
};

// ── Wire conversion ──
//
// Parsing returns false on an unrecognized value rather than falling back to the first enumerator. A board
// whose direction silently read as Higher would sort a best-time board backwards, and nothing downstream
// could tell that from a correctly-parsed board.

FLOCK_API bool FlockLeaderboardValueTypeFromWire(const FString& Wire, EFlockLeaderboardValueType& OutValue);
FLOCK_API bool FlockLeaderboardDirectionFromWire(const FString& Wire, EFlockLeaderboardDirection& OutValue);
FLOCK_API bool FlockLeaderboardAggregationFromWire(const FString& Wire, EFlockLeaderboardAggregation& OutValue);
FLOCK_API bool FlockLeaderboardWindowTypeFromWire(const FString& Wire, EFlockLeaderboardWindowType& OutValue);
FLOCK_API bool FlockLeaderboardScopeFromWire(const FString& Wire, EFlockLeaderboardScope& OutValue);

FLOCK_API FString FlockLeaderboardValueTypeToWire(EFlockLeaderboardValueType Value);
FLOCK_API FString FlockLeaderboardDirectionToWire(EFlockLeaderboardDirection Value);
FLOCK_API FString FlockLeaderboardAggregationToWire(EFlockLeaderboardAggregation Value);
FLOCK_API FString FlockLeaderboardWindowTypeToWire(EFlockLeaderboardWindowType Value);
FLOCK_API FString FlockLeaderboardScopeToWire(EFlockLeaderboardScope Value);

/**
 * Which window of a board to read — a window *key*, not the board's EFlockLeaderboardWindowType.
 *
 * Current() sends no `window` parameter at all, which the API reads as the board's live window: all-time,
 * the current week, or the current season depending on how the board buckets. Season() builds the
 * `season:{id}` key a finished season needs; Period() passes a raw period key ("2026-W31") verbatim.
 *
 * A wrapped string rather than an enum because the API's key space is open — a period key is data, not a
 * value the SDK can enumerate. Held as one FString so the omit-when-empty rule lives in one place.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockLeaderboardWindow
{
	GENERATED_BODY()

	/** The wire value for `window`. Empty means "the board's live window" and is never sent. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Key;

	FFlockLeaderboardWindow() = default;

	/** The board's live window — all-time, current week, or current season. Sends nothing. */
	static FFlockLeaderboardWindow Current();

	/** One finished season on a seasonal board. */
	static FFlockLeaderboardWindow Season(const FString& SeasonId);

	/** A raw period key, e.g. "2026-W31" on a weekly board. Sent verbatim. */
	static FFlockLeaderboardWindow Period(const FString& PeriodKey);

	/** True when no `window` parameter should be sent. */
	bool IsCurrent() const { return Key.IsEmpty(); }
};

/**
 * A board's public configuration, looked up by name.
 *
 * The player-data field a board projects over (`source_template` / `source_field`) is deliberately absent:
 * the client schema omits it, and which field feeds a board is not the game client's business.
 *
 * Enum members cannot arrive through reflection from their wire spellings, so this supplies its own
 * FromWireObject (picked up by FFlockJsonUtils::WireObjectToStruct). The reflection path still handles the
 * snapshot round trip, where enums travel as their enumerator names.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockLeaderboard
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	EFlockLeaderboardValueType ValueType = EFlockLeaderboardValueType::Integer;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	EFlockLeaderboardDirection Direction = EFlockLeaderboardDirection::Higher;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	EFlockLeaderboardAggregation Aggregation = EFlockLeaderboardAggregation::Best;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	EFlockLeaderboardWindowType WindowType = EFlockLeaderboardWindowType::Never;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	EFlockLeaderboardScope Scope = EFlockLeaderboardScope::Global;

	/** True when a bigger number ranks better. Sort and label from this rather than re-reading Direction. */
	bool IsHigherBetter() const { return Direction == EFlockLeaderboardDirection::Higher; }

	/**
	 * Formats a score the way this board measures it. An unranked player (bRanked false) and a non-finite
	 * score both format as empty — there is no number to show, and "nan" is not an answer a player should see.
	 */
	FString FormatScore(double Score, bool bRanked = true) const;

	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockLeaderboard& OutStruct, FString& OutError);
};

/** One row of a board's standings. A null `player_name` or `country` arrives as an empty string. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockStandingEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 Rank = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString PlayerId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString PlayerName;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	double Score = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Country;

	/** Raw ISO-8601 timestamp, kept as the server sent it (same convention as an asset's UpdatedAt). */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString AchievedAt;

	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockStandingEntry& OutStruct, FString& OutError);
};

/**
 * A slice of a board for one window. Total is the board's full entry count for that window, not the size
 * of Items — paging a leaderboard UI reads Total, and confusing the two caps every board at one page.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockStandings
{
	GENERATED_BODY()

	/** The window actually served, which is what the board resolved Current() to. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Window;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 Total = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	TArray<FFlockStandingEntry> Items;

	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockStandings& OutStruct, FString& OutError);
};

/**
 * The signed-in player's placement on one board.
 *
 * Rank and Score are null on the wire when the player has no entry yet — a valid result, not an error.
 * UE has no nullable scalar, and no sentinel is safe here: rank 0 and score 0 are both legitimate, and a
 * negative score is legitimate on a lower-is-better board. So the absence is carried by Ranked, and
 * Rank/Score stay at 0 when it is false. Check Ranked before showing either.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockPlayerRank
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString PlayerId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Window;

	/** False when the player has no entry on this board yet; Rank and Score are meaningless then. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	bool Ranked = false;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 Rank = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	double Score = 0.0;

	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockPlayerRank& OutStruct, FString& OutError);
};
