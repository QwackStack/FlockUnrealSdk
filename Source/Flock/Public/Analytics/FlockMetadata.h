// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

/**
 * Whether Key may join Values: false, with a warning naming both spellings, when Values already holds Key spelled with
 * other letter cases. Keys are compared letter for letter, and this engine's map would otherwise replace the first
 * spelling with the second without a word. Not inline, so the warning is written by this module.
 */
FLOCK_API bool FlockMetadataKeyMayJoin(const TMap<FString, FString>& Values, const FString& Key);

/**
 * Builds the string map a diagnostic entry's Extra Data wants, without an FString::FromInt at every call site.
 *
 * The backend stores that surface's extra data as strings, but the things games actually attach — levels,
 * counts, durations, flags — are not strings. Converting by hand at each call is noise that adds up:
 *
 *   Sdk->LogDiagnosticEvent(TEXT("matchmaking started"),
 *       FFlockMetadata().Add(TEXT("queue"), TEXT("ranked")).Add(TEXT("party_size"), 3).Add(TEXT("ranked"), true));
 *
 * This is the diagnostics container. A gameplay event's properties are FFlockCommandData, where a number
 * stays a number so the Game Metrics dashboards can chart it — see TrackAnalyticsEvent.
 *
 * Converts implicitly to the map, so it drops into any call taking metadata. Blueprint gets the same
 * builders as chainable nodes on UFlockAnalyticsLibrary.
 *
 * Pass a chain straight into a call, as above — the temporary lives to the end of the statement.
 * Do NOT bind a reference to its Values:
 *
 *     const TMap<FString, FString>& Bad = FFlockMetadata().Add(...).Values;  // dangles
 *     const TMap<FString, FString>  Good = FFlockMetadata().Add(...).Values; // copies
 *
 * Reaching a member through a returned reference gets no lifetime extension, so the first form reads
 * freed memory.
 *
 * Keys are compared letter for letter: `Score` and `score` are two keys. The engine's map cannot keep both (it
 * would replace one with the other without a word), so the second spelling is not added and a warning names both;
 * the first stays. Adding a key spelled exactly as one already there replaces its value, as a map does.
 */
struct FFlockMetadata
{
	TMap<FString, FString> Values;

	FFlockMetadata& Add(const FString& Key, const FString& Value)
	{
		if (FlockMetadataKeyMayJoin(Values, Key))
		{
			Values.Add(Key, Value);
		}
		return *this;
	}

	/**
	 * Present so a literal binds here rather than to the bool overload. Without it,
	 * Add(TEXT("k"), TEXT("v")) would silently record "true".
	 */
	FFlockMetadata& Add(const FString& Key, const TCHAR* Value)
	{
		return Add(Key, FString(Value));
	}

	FFlockMetadata& Add(const FString& Key, int32 Value)
	{
		return Add(Key, FString::FromInt(Value));
	}

	FFlockMetadata& Add(const FString& Key, int64 Value)
	{
		return Add(Key, LexToString(Value));
	}

	FFlockMetadata& Add(const FString& Key, float Value)
	{
		return Add(Key, FString::SanitizeFloat(Value));
	}

	FFlockMetadata& Add(const FString& Key, bool Value)
	{
		return Add(Key, FString(Value ? TEXT("true") : TEXT("false")));
	}

	operator const TMap<FString, FString>&() const { return Values; }
};
