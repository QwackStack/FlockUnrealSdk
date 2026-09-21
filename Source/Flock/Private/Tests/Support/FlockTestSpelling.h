// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

#include "Dom/JsonObject.h"

/**
 * Checks that read a spelling letter for letter.
 *
 * Unreal's ordinary string checks ignore letter case: FString's ==, Contains and Equals by default, TestEqual on strings,
 * TArray<FString>::Contains, a TMap<FString, ...> key and FJsonObject's HasField. A test built on them cannot tell
 * "MaxHealth" from "maxHealth" -- which is the whole difference a test of a key's spelling exists for, and the one a
 * server reading keys letter for letter acts on.
 */
namespace FlockTestSpelling
{
	/** Whether Names holds Name spelled exactly. */
	inline bool HoldsExactly(const TArray<FString>& Names, const FString& Name)
	{
		return Names.ContainsByPredicate([&Name](const FString& Held) { return Held.Equals(Name, ESearchCase::CaseSensitive); });
	}

	/** Whether Map has a key spelled exactly Name. */
	template <typename ValueType>
	bool HasKeySpelled(const TMap<FString, ValueType>& Map, const FString& Name)
	{
		for (const TPair<FString, ValueType>& Pair : Map)
		{
			if (Pair.Key.Equals(Name, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * Whether Object has a top-level member spelled exactly Name. Read from the object's own keys, never through its
	 * lookup, which ignores case -- and never through the SDK's own name listing, so a defect there cannot hide here.
	 */
	inline bool HasMemberSpelled(const TSharedPtr<FJsonObject>& Object, const FString& Name)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		// `auto` and `*Pair.Key`: the key's type differs across engines (FString, then an interned string), and
		// dereferencing gives a TCHAR pointer on every one.
		for (const auto& Pair : Object->Values)
		{
			if (Name.Equals(FString(*Pair.Key), ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}
}

#endif // WITH_AUTOMATION_TESTS
