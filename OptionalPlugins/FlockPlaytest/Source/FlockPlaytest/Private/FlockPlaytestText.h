// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

namespace FlockPlaytestText
{
	/**
	 * True when Text holds a space, tab, line break or any other whitespace character anywhere. Values that must not
	 * hold any are refused with this rather than trimmed, so the mistake is fixed where it was made.
	 */
	inline bool ContainsWhitespace(const FString& Text)
	{
		for (const TCHAR Character : Text)
		{
			if (FChar::IsWhitespace(Character))
			{
				return true;
			}
		}
		return false;
	}
}
