// Copyright 2022, Qwacks. All Rights Reserved.

#include "Blueprint/FlockJsonDataLibrary.h"

int32 UFlockJsonDataLibrary::GetJsonInt(const FFlockJsonData& Data, const FString& Path, int32 Fallback)
{
	int32 Value = 0;
	return Data.TryGetInt(Path, Value) ? Value : Fallback;
}

float UFlockJsonDataLibrary::GetJsonFloat(const FFlockJsonData& Data, const FString& Path, float Fallback)
{
	float Value = 0.f;
	return Data.TryGetFloat(Path, Value) ? Value : Fallback;
}

FString UFlockJsonDataLibrary::GetJsonString(const FFlockJsonData& Data, const FString& Path, const FString& Fallback)
{
	FString Value;
	return Data.TryGetString(Path, Value) ? Value : Fallback;
}

bool UFlockJsonDataLibrary::GetJsonBool(const FFlockJsonData& Data, const FString& Path, bool bFallback)
{
	bool bValue = false;
	return Data.TryGetBool(Path, bValue) ? bValue : bFallback;
}

TArray<FString> UFlockJsonDataLibrary::GetJsonStringArray(const FFlockJsonData& Data, const FString& Path)
{
	TArray<FString> Value;
	Data.TryGetStringArray(Path, Value);
	return Value;
}

bool UFlockJsonDataLibrary::HasJsonField(const FFlockJsonData& Data, const FString& Path)
{
	return Data.HasField(Path);
}

TArray<FString> UFlockJsonDataLibrary::GetJsonFieldNames(const FFlockJsonData& Data)
{
	return Data.GetFieldNames();
}

FString UFlockJsonDataLibrary::JsonDataToString(const FFlockJsonData& Data)
{
	return Data.ToJsonString();
}

bool UFlockJsonDataLibrary::IsJsonDataValid(const FFlockJsonData& Data)
{
	return Data.IsValid();
}
