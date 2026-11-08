// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

/**
 * Unicode's white space, the same on every platform. FChar::IsWhitespace asks the C runtime, whose answer for a no-break or
 * an ideographic space depends on the platform and its locale, so a value refused on one platform was sent from another.
 */
namespace FlockWhiteSpace
{
	inline bool IsWhiteSpace(TCHAR Character)
	{
		const uint32 Code = static_cast<uint32>(Character);
		return (Code >= 0x0009 && Code <= 0x000D) || Code == 0x0020 || Code == 0x0085 || Code == 0x00A0 || Code == 0x1680
			|| (Code >= 0x2000 && Code <= 0x200A) || Code == 0x2028 || Code == 0x2029 || Code == 0x202F || Code == 0x205F
			|| Code == 0x3000;
	}

	/** True when Value starts or ends with white space: a value to refuse, never to trim, since trimming hides where it came from. */
	inline bool IsAtEitherEnd(const FString& Value)
	{
		return !Value.IsEmpty() && (IsWhiteSpace(Value[0]) || IsWhiteSpace(Value[Value.Len() - 1]));
	}
}
