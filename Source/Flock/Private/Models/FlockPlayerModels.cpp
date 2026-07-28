// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Models/FlockPlayerModels.h"

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

bool FFlockPlayerTemplateSchema::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockPlayerTemplateSchema& OutStruct, FString& OutError)
{
	Object->TryGetStringField(TEXT("id"), OutStruct.Id);
	Object->TryGetStringField(TEXT("name"), OutStruct.Name);
	Object->TryGetStringField(TEXT("game_version_id"), OutStruct.GameVersionId);
	Object->TryGetStringField(TEXT("tag"), OutStruct.Tag);

	OutStruct.Data = FFlockStructuredData::FromWireData(Object->TryGetField(TEXT("data")));

	const TSharedPtr<FJsonValue> SchemaValue = Object->TryGetField(TEXT("schema"));
	if (SchemaValue.IsValid() && SchemaValue->Type != EJson::Null)
	{
		OutStruct.SchemaJson = SerializeValue(SchemaValue.ToSharedRef());
	}
	return true;
}

bool FFlockPlayerData::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockPlayerData& OutStruct, FString& OutError)
{
	Object->TryGetStringField(TEXT("id"), OutStruct.Id);
	Object->TryGetStringField(TEXT("player_template_id"), OutStruct.PlayerTemplateId);
	Object->TryGetStringField(TEXT("game_id"), OutStruct.GameId);
	Object->TryGetStringField(TEXT("player_id"), OutStruct.PlayerId);
	Object->TryGetStringField(TEXT("created_at"), OutStruct.CreatedAt);
	Object->TryGetStringField(TEXT("updated_at"), OutStruct.UpdatedAt);

	OutStruct.Data = FFlockStructuredData::FromWireData(Object->TryGetField(TEXT("data")));
	return true;
}

FFlockFeatureBan FFlockFeatureBan::FromWire(const TSharedPtr<FJsonObject>& Object)
{
	FFlockFeatureBan Result;
	if (Object.IsValid())
	{
		Object->TryGetStringField(TEXT("reason"), Result.Reason);
		Object->TryGetStringField(TEXT("ban_duration"), Result.BanDuration);
		Object->TryGetStringField(TEXT("effective_datetime"), Result.EffectiveDatetime);
	}
	return Result;
}

bool FFlockPlayerBan::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockPlayerBan& OutStruct, FString& OutError)
{
	Object->TryGetStringField(TEXT("id"), OutStruct.Id);
	Object->TryGetStringField(TEXT("player_id"), OutStruct.PlayerId);
	Object->TryGetStringField(TEXT("game_id"), OutStruct.GameId);
	Object->TryGetStringField(TEXT("created_at"), OutStruct.CreatedAt);
	Object->TryGetStringField(TEXT("updated_at"), OutStruct.UpdatedAt);

	const TSharedPtr<FJsonObject>* DataObject = nullptr;
	if (Object->TryGetObjectField(TEXT("data"), DataObject) && DataObject != nullptr)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*DataObject)->Values)
		{
			// Feature keys are author data — kept verbatim (never case-transformed). Guard the type before
			// AsObject(): a non-object would otherwise deserialize as a valid-but-empty entry.
			if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Object)
			{
				OutStruct.Data.Add(Pair.Key, FFlockFeatureBan::FromWire(Pair.Value->AsObject()));
			}
		}
	}
	return true;
}

bool FFlockPlayerBanResponse::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockPlayerBanResponse& OutStruct, FString& OutError)
{
	// Enveloped, but `result` may be null (Union[PlayerBanSchema, None]) — a null means "not banned",
	// which stays an empty Ban rather than an error. A present object is parsed as a bare ban record.
	const TSharedPtr<FJsonObject>* ResultObject = nullptr;
	if (Object->TryGetObjectField(TEXT("result"), ResultObject) && ResultObject != nullptr && ResultObject->IsValid())
	{
		return FFlockPlayerBan::FromWireObject((*ResultObject).ToSharedRef(), OutStruct.Ban, OutError);
	}
	return true; // absent / null result -> not banned
}
