// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Http/FlockErrorCode.h"
#include "UObject/Class.h"

EFlockErrorCode FFlockErrorCodes::Parse(const FString& Code)
{
	if (Code.IsEmpty())
	{
		return EFlockErrorCode::Unknown;
	}

	// "namespace.reason_words" -> split on '.'/'_' -> PascalCase each segment -> concat.
	FString Normalized = Code.Replace(TEXT("."), TEXT("_"));
	TArray<FString> Parts;
	Normalized.ParseIntoArray(Parts, TEXT("_"), /*InCullEmpty*/ true);

	FString Name;
	Name.Reserve(Code.Len());
	for (const FString& Part : Parts)
	{
		if (Part.IsEmpty())
		{
			continue;
		}
		Name += Part.Left(1).ToUpper();
		if (Part.Len() > 1)
		{
			Name += Part.RightChop(1);
		}
	}

	if (Name.IsEmpty())
	{
		return EFlockErrorCode::Unknown;
	}

	// Reflection lookup by the fully-qualified enumerator name (how enum-class members are stored).
	const UEnum* EnumPtr = StaticEnum<EFlockErrorCode>();
	const int64 Value = EnumPtr ? EnumPtr->GetValueByNameString(FString::Printf(TEXT("EFlockErrorCode::%s"), *Name)) : INDEX_NONE;
	if (Value == INDEX_NONE)
	{
		return EFlockErrorCode::Unknown;
	}
	return static_cast<EFlockErrorCode>(Value);
}
