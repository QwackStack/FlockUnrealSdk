// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockCommandDataLibrary.h"

FFlockCommandData UFlockCommandDataLibrary::SetCommandInt(const FFlockCommandData& Data, const FString& Key, int32 Value)
{
	return FFlockCommandData(Data).Set(Key, Value);
}

FFlockCommandData UFlockCommandDataLibrary::SetCommandFloat(const FFlockCommandData& Data, const FString& Key, float Value)
{
	return FFlockCommandData(Data).Set(Key, Value);
}

FFlockCommandData UFlockCommandDataLibrary::SetCommandString(const FFlockCommandData& Data, const FString& Key, const FString& Value)
{
	return FFlockCommandData(Data).Set(Key, Value);
}

FFlockCommandData UFlockCommandDataLibrary::SetCommandBool(const FFlockCommandData& Data, const FString& Key, bool bValue)
{
	return FFlockCommandData(Data).Set(Key, bValue);
}

FFlockCommandData UFlockCommandDataLibrary::SetCommandStringArray(const FFlockCommandData& Data, const FString& Key,
	const TArray<FString>& Value)
{
	return FFlockCommandData(Data).Set(Key, Value);
}

FFlockCommandData UFlockCommandDataLibrary::SetCommandJson(const FFlockCommandData& Data, const FString& Key, const FString& Json)
{
	return FFlockCommandData(Data).SetRawJson(Key, Json);
}

TArray<FString> UFlockCommandDataLibrary::GetCommandFieldNames(const FFlockCommandData& Data)
{
	return Data.GetFieldNames();
}

FString UFlockCommandDataLibrary::CommandDataToJsonString(const FFlockCommandData& Data)
{
	return Data.ToJsonString();
}

bool UFlockCommandDataLibrary::IsEmptyCommandData(const FFlockCommandData& Data)
{
	return Data.IsEmpty();
}

FFlockCommandValue UFlockCommandDataLibrary::CommandValueInt(int32 Value)
{
	return FFlockCommandValue(Value);
}

FFlockCommandValue UFlockCommandDataLibrary::CommandValueFloat(float Value)
{
	return FFlockCommandValue(Value);
}

FFlockCommandValue UFlockCommandDataLibrary::CommandValueString(const FString& Value)
{
	return FFlockCommandValue(Value);
}

FFlockCommandValue UFlockCommandDataLibrary::CommandValueBool(bool bValue)
{
	return FFlockCommandValue(bValue);
}

FFlockCommandValue UFlockCommandDataLibrary::CommandValueStringArray(const TArray<FString>& Value)
{
	return FFlockCommandValue(Value);
}

FFlockCommandValue UFlockCommandDataLibrary::CommandValueJson(const FString& Json)
{
	return FFlockCommandValue::FromRawJson(Json);
}

FString UFlockCommandDataLibrary::CommandValueToJsonString(const FFlockCommandValue& Value)
{
	return Value.ToJsonString();
}
