// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Builds the string map the analytics wire wants, without an FString::FromInt at every call site.
 *
 * The backend stores metadata as strings, but the things games actually attach — levels, counts,
 * durations, flags — are not strings. Converting by hand at each call is noise that adds up:
 *
 *   Sdk->LogAnalyticsEvent(TEXT("level_complete"),
 *       FFlockMetadata().Add(TEXT("level"), 7).Add(TEXT("deaths"), 2).Add(TEXT("flawless"), true));
 *
 * Converts implicitly to the map, so it drops into any call taking metadata. C++ only — Blueprint
 * has Make Map, which is already ergonomic there.
 */
struct FFlockMetadata
{
	TMap<FString, FString> Values;

	FFlockMetadata& Add(const FString& Key, const FString& Value)
	{
		Values.Add(Key, Value);
		return *this;
	}

	/**
	 * Present so a literal binds here rather than to the bool overload. Without it,
	 * Add(TEXT("k"), TEXT("v")) would silently record "true".
	 */
	FFlockMetadata& Add(const FString& Key, const TCHAR* Value)
	{
		Values.Add(Key, FString(Value));
		return *this;
	}

	FFlockMetadata& Add(const FString& Key, int32 Value)
	{
		Values.Add(Key, FString::FromInt(Value));
		return *this;
	}

	FFlockMetadata& Add(const FString& Key, int64 Value)
	{
		Values.Add(Key, LexToString(Value));
		return *this;
	}

	FFlockMetadata& Add(const FString& Key, float Value)
	{
		Values.Add(Key, FString::SanitizeFloat(Value));
		return *this;
	}

	FFlockMetadata& Add(const FString& Key, bool Value)
	{
		Values.Add(Key, Value ? TEXT("true") : TEXT("false"));
		return *this;
	}

	operator const TMap<FString, FString>&() const { return Values; }
};
