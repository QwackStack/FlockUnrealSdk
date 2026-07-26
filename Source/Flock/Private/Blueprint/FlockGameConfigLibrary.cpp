// Copyright 2022, Qwacks. All Rights Reserved.

#include "Blueprint/FlockGameConfigLibrary.h"

int32 UFlockGameConfigLibrary::GetConfigInt(const FFlockGameConfigData& Data, const FString& Path, int32 Fallback)
{
	int32 Value = 0;
	return Data.TryGetInt(Path, Value) ? Value : Fallback;
}

float UFlockGameConfigLibrary::GetConfigFloat(const FFlockGameConfigData& Data, const FString& Path, float Fallback)
{
	float Value = 0.f;
	return Data.TryGetFloat(Path, Value) ? Value : Fallback;
}

FString UFlockGameConfigLibrary::GetConfigString(const FFlockGameConfigData& Data, const FString& Path, const FString& Fallback)
{
	FString Value;
	return Data.TryGetString(Path, Value) ? Value : Fallback;
}

bool UFlockGameConfigLibrary::GetConfigBool(const FFlockGameConfigData& Data, const FString& Path, bool bFallback)
{
	bool bValue = false;
	return Data.TryGetBool(Path, bValue) ? bValue : bFallback;
}

TArray<FString> UFlockGameConfigLibrary::GetConfigStringArray(const FFlockGameConfigData& Data, const FString& Path)
{
	TArray<FString> Value;
	Data.TryGetStringArray(Path, Value); // empty on a miss
	return Value;
}

bool UFlockGameConfigLibrary::HasConfigField(const FFlockGameConfigData& Data, const FString& Path)
{
	return Data.HasField(Path);
}

TArray<FString> UFlockGameConfigLibrary::GetConfigFieldNames(const FFlockGameConfigData& Data)
{
	return Data.GetFieldNames();
}

FString UFlockGameConfigLibrary::ConfigDataToJsonString(const FFlockGameConfigData& Data)
{
	return Data.ToJsonString();
}

bool UFlockGameConfigLibrary::IsConfigDataValid(const FFlockGameConfigData& Data)
{
	return Data.IsValid();
}
