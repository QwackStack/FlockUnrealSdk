// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/** Fields of a JSON answer read as their writer spelled them: the engine's own lookups ignore letter case. */
namespace FlockWireFields
{
	/** The field whose key is spelled exactly so (letter case counts), or null. */
	inline TSharedPtr<FJsonValue> SpelledExactly(const FJsonObject& Object, const TCHAR* Key)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object.Values)
		{
			if (Pair.Key.Equals(Key, ESearchCase::CaseSensitive))
			{
				return Pair.Value;
			}
		}
		return nullptr;
	}

	/** A text value as it is; empty for anything else, null included. */
	inline FString TextOf(const TSharedPtr<FJsonValue>& Value)
	{
		FString Text;
		if (Value.IsValid() && Value->Type == EJson::String)
		{
			Value->TryGetString(Text);
		}
		return Text;
	}
}
