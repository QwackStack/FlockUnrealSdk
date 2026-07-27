// Copyright 2022, Qwacks. All Rights Reserved.

#include "Blueprint/FlockStructuredDataLibrary.h"

int32 UFlockStructuredDataLibrary::GetDataInt(const FFlockStructuredData& Data, const FString& Path, int32 Fallback)
{
	int32 Value = 0;
	return Data.TryGetInt(Path, Value) ? Value : Fallback;
}

float UFlockStructuredDataLibrary::GetDataFloat(const FFlockStructuredData& Data, const FString& Path, float Fallback)
{
	float Value = 0.f;
	return Data.TryGetFloat(Path, Value) ? Value : Fallback;
}

FString UFlockStructuredDataLibrary::GetDataString(const FFlockStructuredData& Data, const FString& Path, const FString& Fallback)
{
	FString Value;
	return Data.TryGetString(Path, Value) ? Value : Fallback;
}

bool UFlockStructuredDataLibrary::GetDataBool(const FFlockStructuredData& Data, const FString& Path, bool bFallback)
{
	bool bValue = false;
	return Data.TryGetBool(Path, bValue) ? bValue : bFallback;
}

TArray<FString> UFlockStructuredDataLibrary::GetDataStringArray(const FFlockStructuredData& Data, const FString& Path)
{
	TArray<FString> Value;
	Data.TryGetStringArray(Path, Value); // empty on a miss
	return Value;
}

bool UFlockStructuredDataLibrary::HasDataField(const FFlockStructuredData& Data, const FString& Path)
{
	return Data.HasField(Path);
}

TArray<FString> UFlockStructuredDataLibrary::GetDataFieldNames(const FFlockStructuredData& Data)
{
	return Data.GetFieldNames();
}

FString UFlockStructuredDataLibrary::DataToJsonString(const FFlockStructuredData& Data)
{
	return Data.ToJsonString();
}

bool UFlockStructuredDataLibrary::IsValidData(const FFlockStructuredData& Data)
{
	return Data.IsValid();
}
