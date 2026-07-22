// Copyright 2022, Qwacks. All Rights Reserved.

#include "Analytics/FlockAnalyticsLibrary.h"

#include "Analytics/FlockMetadata.h"

namespace
{
	/**
	 * Every helper routes through FFlockMetadata rather than formatting inline. Two implementations
	 * of "how does a bool become a string" would drift, and the drift would only show up as
	 * Blueprint and C++ writing different values for the same input.
	 */
	template <typename T>
	TMap<FString, FString> AddThrough(const TMap<FString, FString>& Metadata, const FString& Key, T Value)
	{
		FFlockMetadata Builder;
		Builder.Values = Metadata;
		Builder.Add(Key, Value);
		return MoveTemp(Builder.Values);
	}
}

TMap<FString, FString> UFlockAnalyticsLibrary::MakeMetadata()
{
	return TMap<FString, FString>();
}

TMap<FString, FString> UFlockAnalyticsLibrary::AddMetadataString(const TMap<FString, FString>& Metadata,
	const FString& Key, const FString& Value)
{
	return AddThrough(Metadata, Key, Value);
}

TMap<FString, FString> UFlockAnalyticsLibrary::AddMetadataInt(const TMap<FString, FString>& Metadata,
	const FString& Key, int32 Value)
{
	return AddThrough(Metadata, Key, Value);
}

TMap<FString, FString> UFlockAnalyticsLibrary::AddMetadataFloat(const TMap<FString, FString>& Metadata,
	const FString& Key, float Value)
{
	return AddThrough(Metadata, Key, Value);
}

TMap<FString, FString> UFlockAnalyticsLibrary::AddMetadataBool(const TMap<FString, FString>& Metadata,
	const FString& Key, bool Value)
{
	return AddThrough(Metadata, Key, Value);
}
