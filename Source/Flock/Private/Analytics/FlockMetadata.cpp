// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Analytics/FlockMetadata.h"

#include "Flock.h"
#include "Http/FlockJsonUtils.h"

bool FlockMetadataKeyMayJoin(const TMap<FString, FString>& Values, const FString& Key)
{
	TArray<FString> Keys;
	Keys.Reserve(Values.Num());
	for (const TPair<FString, FString>& Pair : Values)
	{
		Keys.Add(Pair.Key);
	}
	const FString OtherSpelling = FFlockJsonUtils::OtherSpellingOf(Keys, Key);
	if (OtherSpelling.IsEmpty())
	{
		return true;
	}
	UE_LOG(LogFlock, Warning, TEXT("[Flock SDK] Metadata key '%s' is not added: '%s' is already there, and the two are different keys (compared letter ")
		TEXT("for letter), which this engine's map cannot keep apart. The first stays."), *Key, *OtherSpelling);
	return false;
}
