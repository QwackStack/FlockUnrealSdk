// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Engine/BlueprintGeneratedClass.h"

/**
 * The caller a Blueprint node's provider calls are logged under. The world context is the graph's `self`: a Blueprint-generated
 * class for a Blueprint caller (its "_C" suffix belongs to the generated class, not the asset's name); anything else means the
 * node was driven from C++, logged as "Blueprint node".
 */
namespace FlockBlueprintCallOrigin
{
	inline FString Of(const UObject* WorldContextObject)
	{
		if (const UBlueprintGeneratedClass* BlueprintClass =
			WorldContextObject ? Cast<UBlueprintGeneratedClass>(WorldContextObject->GetClass()) : nullptr)
		{
			FString AssetName = BlueprintClass->GetName();
			AssetName.RemoveFromEnd(TEXT("_C"));
			return FString::Printf(TEXT("Blueprint '%s'"), *AssetName);
		}
		return TEXT("Blueprint node");
	}
}
