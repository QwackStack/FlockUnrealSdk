// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

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

// The event-property nodes write through FFlockCommandData's own setters, for the same reason the metadata
// nodes write through FFlockMetadata: one place decides how a value reaches the wire, so a property set in a
// graph and the same property set in C++ cannot arrive as different types.
template <typename T>
static FFlockCommandData SetEventPropertyThrough(const FFlockCommandData& Properties, const FString& Key, T Value)
{
	FFlockCommandData Copy = Properties;
	Copy.Set(Key, Value);
	return Copy;
}

FFlockCommandData UFlockAnalyticsLibrary::MakeEventProperties()
{
	return FFlockCommandData();
}

FFlockCommandData UFlockAnalyticsLibrary::AddEventPropertyString(const FFlockCommandData& Properties,
	const FString& Key, const FString& Value)
{
	return SetEventPropertyThrough(Properties, Key, Value);
}

FFlockCommandData UFlockAnalyticsLibrary::AddEventPropertyInt(const FFlockCommandData& Properties,
	const FString& Key, int32 Value)
{
	return SetEventPropertyThrough(Properties, Key, Value);
}

FFlockCommandData UFlockAnalyticsLibrary::AddEventPropertyFloat(const FFlockCommandData& Properties,
	const FString& Key, float Value)
{
	return SetEventPropertyThrough(Properties, Key, Value);
}

FFlockCommandData UFlockAnalyticsLibrary::AddEventPropertyBool(const FFlockCommandData& Properties,
	const FString& Key, bool Value)
{
	return SetEventPropertyThrough(Properties, Key, Value);
}

FFlockCommandData UFlockAnalyticsLibrary::AddEventPropertyStringArray(const FFlockCommandData& Properties,
	const FString& Key, const TArray<FString>& Value)
{
	return SetEventPropertyThrough(Properties, Key, Value);
}
