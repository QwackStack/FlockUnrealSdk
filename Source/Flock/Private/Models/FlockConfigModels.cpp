// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Models/FlockConfigModels.h"

#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	/** Serializes a JSON value to a condensed string — for keeping `schema` verbatim. */
	FString SerializeValue(const TSharedRef<FJsonValue>& Value)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Value, FString(), Writer);
		return Out;
	}
}

FString FlockConfigTagToWire(EFlockConfigTag Tag)
{
	switch (Tag)
	{
	case EFlockConfigTag::Gameplay:    return TEXT("gameplay");
	case EFlockConfigTag::Currency:    return TEXT("currency");
	case EFlockConfigTag::Asset:       return TEXT("asset");
	case EFlockConfigTag::Feature:     return TEXT("feature");
	case EFlockConfigTag::Achievement: return TEXT("achievement");
	case EFlockConfigTag::Any:
	default:                           return FString();
	}
}

bool FFlockGameConfigSchema::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockGameConfigSchema& OutStruct, FString& OutError)
{
	Object->TryGetStringField(TEXT("id"), OutStruct.Id);
	Object->TryGetStringField(TEXT("name"), OutStruct.Name);
	Object->TryGetStringField(TEXT("game_id"), OutStruct.GameId);
	Object->TryGetStringField(TEXT("game_version_id"), OutStruct.GameVersionId);
	Object->TryGetStringField(TEXT("tag"), OutStruct.Tag);
	Object->TryGetStringField(TEXT("created_at"), OutStruct.CreatedAt);
	Object->TryGetStringField(TEXT("updated_at"), OutStruct.UpdatedAt);

	OutStruct.Data = FFlockStructuredData::FromWireData(Object->TryGetField(TEXT("data")));

	const TSharedPtr<FJsonValue> SchemaValue = Object->TryGetField(TEXT("schema"));
	if (SchemaValue.IsValid() && SchemaValue->Type != EJson::Null)
	{
		OutStruct.SchemaJson = SerializeValue(SchemaValue.ToSharedRef());
	}
	return true;
}

bool FFlockGamePatchSchema::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockGamePatchSchema& OutStruct, FString& OutError)
{
	Object->TryGetStringField(TEXT("id"), OutStruct.Id);
	Object->TryGetStringField(TEXT("name"), OutStruct.Name);
	Object->TryGetStringField(TEXT("game_config_id"), OutStruct.GameConfigId);
	Object->TryGetStringField(TEXT("created_at"), OutStruct.CreatedAt);
	Object->TryGetStringField(TEXT("updated_at"), OutStruct.UpdatedAt);

	OutStruct.Data = FFlockStructuredData::FromWireData(Object->TryGetField(TEXT("data")));
	return true;
}
