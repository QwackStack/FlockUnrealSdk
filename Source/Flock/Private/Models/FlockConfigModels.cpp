// Copyright 2022, Qwacks. All Rights Reserved.

#include "Models/FlockConfigModels.h"

#include "Http/FlockJsonUtils.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	FString SerializeValue(const TSharedRef<FJsonValue>& Value)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Value, FString(), Writer);
		return Out;
	}

	FString SerializeObject(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	// Forward declaration — the flatten is mutually recursive across the three container node types.
	TSharedPtr<FJsonValue> FlattenField(const TSharedPtr<FJsonObject>& Field);

	/**
	 * Collapses a list[DataField] into a flat object. Keys are the children's field_name, snake->Pascal
	 * cased: these are field names (an object body or the top level), so a reflected/codegen USTRUCT binds.
	 */
	TSharedRef<FJsonObject> FlattenFieldList(const TArray<TSharedPtr<FJsonValue>>& Fields)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		for (const TSharedPtr<FJsonValue>& Element : Fields)
		{
			const TSharedPtr<FJsonObject> Field = Element.IsValid() ? Element->AsObject() : nullptr;
			if (!Field.IsValid())
			{
				continue;
			}
			FString FieldName;
			if (!Field->TryGetStringField(TEXT("field_name"), FieldName) || FieldName.IsEmpty())
			{
				continue;
			}
			Object->SetField(FFlockJsonUtils::SnakeToPascal(FieldName), FlattenField(Field));
		}
		return Object;
	}

	/** Collapses one DataField {type, field_name, type_name, value} to its plain JSON value. */
	TSharedPtr<FJsonValue> FlattenField(const TSharedPtr<FJsonObject>& Field)
	{
		const TSharedPtr<FJsonValue> Value = Field->TryGetField(TEXT("value"));
		if (!Value.IsValid() || Value->Type == EJson::Null)
		{
			return MakeShared<FJsonValueNull>();
		}

		FString Type;
		Field->TryGetStringField(TEXT("type"), Type);
		Type.ToLowerInline();

		if (Type == TEXT("object") && Value->Type == EJson::Array)
		{
			return MakeShared<FJsonValueObject>(FlattenFieldList(Value->AsArray()));
		}
		if ((Type == TEXT("list") || Type == TEXT("array")) && Value->Type == EJson::Array)
		{
			// List items are positional DataFields — no field names, so flatten each to its value.
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const TSharedPtr<FJsonValue>& Element : Value->AsArray())
			{
				const TSharedPtr<FJsonObject> ItemField = Element.IsValid() ? Element->AsObject() : nullptr;
				if (ItemField.IsValid())
				{
					Items.Add(FlattenField(ItemField));
				}
			}
			return MakeShared<FJsonValueArray>(Items);
		}
		if (Type == TEXT("dict") && Value->Type == EJson::Object)
		{
			// Dict keys are author-supplied data, NOT field names — kept verbatim, never case-transformed.
			TSharedRef<FJsonObject> DictObject = MakeShared<FJsonObject>();
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Value->AsObject()->Values)
			{
				const TSharedPtr<FJsonObject> EntryField = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
				if (EntryField.IsValid())
				{
					DictObject->SetField(Pair.Key, FlattenField(EntryField));
				}
			}
			return MakeShared<FJsonValueObject>(DictObject);
		}

		// Scalar, or a declared container whose value didn't match its type: take the value as-is.
		return Value;
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

FFlockGameConfigData FFlockGameConfigData::FromWireData(const TSharedPtr<FJsonValue>& DataValue)
{
	FFlockGameConfigData Result;
	if (!DataValue.IsValid() || DataValue->Type == EJson::Null)
	{
		return Result; // no data — a valid-but-empty handle
	}

	if (DataValue->Type == EJson::Array)
	{
		// New form: list[DataField] — collapse it type-aware.
		Result.FlatJson = SerializeObject(FlattenFieldList(DataValue->AsArray()));
	}
	else if (DataValue->Type == EJson::Object)
	{
		// Legacy form: dict[str, Any] is already flat — adopt verbatim. (Unity's deserializer would throw here.)
		const TSharedPtr<FJsonObject> LegacyObject = DataValue->AsObject();
		if (LegacyObject.IsValid())
		{
			Result.FlatJson = SerializeObject(LegacyObject.ToSharedRef());
		}
	}
	// A scalar `data` is not a shape we can flatten — leave the handle empty.

	return Result;
}

bool FFlockGameConfigData::IsValid() const
{
	return ResolveObject().IsValid();
}

TSharedPtr<FJsonObject> FFlockGameConfigData::ResolveObject() const
{
	if (!bParsed)
	{
		bParsed = true;
		if (!FlatJson.IsEmpty())
		{
			TSharedPtr<FJsonObject> Parsed;
			const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(FlatJson);
			if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
			{
				CachedObject = Parsed;
			}
		}
	}
	return CachedObject;
}

TSharedPtr<FJsonValue> FFlockGameConfigData::ResolvePath(const FString& Path) const
{
	const TSharedPtr<FJsonObject> Root = ResolveObject();
	if (!Root.IsValid() || Path.IsEmpty())
	{
		return nullptr;
	}

	TArray<FString> Segments;
	Path.ParseIntoArray(Segments, TEXT("."), /*CullEmpty*/ true);

	TSharedPtr<FJsonObject> Current = Root;
	TSharedPtr<FJsonValue> Value;
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		if (!Current.IsValid())
		{
			return nullptr;
		}
		const FString& Segment = Segments[Index];
		// Exact first (matches a Pascal name or a legacy verbatim key), then Pascal (matches a flattened key
		// when the caller typed the dashboard's snake_case name).
		Value = Current->TryGetField(Segment);
		if (!Value.IsValid())
		{
			Value = Current->TryGetField(FFlockJsonUtils::SnakeToPascal(Segment));
		}
		if (!Value.IsValid())
		{
			return nullptr;
		}
		if (Index + 1 < Segments.Num())
		{
			Current = (Value->Type == EJson::Object) ? Value->AsObject() : nullptr;
		}
	}
	return Value;
}

bool FFlockGameConfigData::TryGetString(const FString& Path, FString& OutValue) const
{
	const TSharedPtr<FJsonValue> Value = ResolvePath(Path);
	return Value.IsValid() && Value->TryGetString(OutValue);
}

bool FFlockGameConfigData::TryGetInt(const FString& Path, int32& OutValue) const
{
	const TSharedPtr<FJsonValue> Value = ResolvePath(Path);
	double Number = 0.0;
	if (!Value.IsValid() || !Value->TryGetNumber(Number))
	{
		return false;
	}
	OutValue = static_cast<int32>(Number);
	return true;
}

bool FFlockGameConfigData::TryGetFloat(const FString& Path, float& OutValue) const
{
	const TSharedPtr<FJsonValue> Value = ResolvePath(Path);
	double Number = 0.0;
	if (!Value.IsValid() || !Value->TryGetNumber(Number))
	{
		return false;
	}
	OutValue = static_cast<float>(Number);
	return true;
}

bool FFlockGameConfigData::TryGetBool(const FString& Path, bool& OutValue) const
{
	const TSharedPtr<FJsonValue> Value = ResolvePath(Path);
	return Value.IsValid() && Value->TryGetBool(OutValue);
}

bool FFlockGameConfigData::TryGetStringArray(const FString& Path, TArray<FString>& OutValue) const
{
	const TSharedPtr<FJsonValue> Value = ResolvePath(Path);
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (!Value.IsValid() || !Value->TryGetArray(Array))
	{
		return false;
	}
	OutValue.Reset();
	for (const TSharedPtr<FJsonValue>& Element : *Array)
	{
		FString Item;
		if (Element.IsValid() && Element->TryGetString(Item))
		{
			OutValue.Add(Item);
		}
	}
	return true;
}

bool FFlockGameConfigData::HasField(const FString& Path) const
{
	const TSharedPtr<FJsonValue> Value = ResolvePath(Path);
	return Value.IsValid() && Value->Type != EJson::Null;
}

TArray<FString> FFlockGameConfigData::GetFieldNames() const
{
	TArray<FString> Names;
	const TSharedPtr<FJsonObject> Object = ResolveObject();
	if (Object.IsValid())
	{
		Object->Values.GetKeys(Names);
	}
	return Names;
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

	OutStruct.Data = FFlockGameConfigData::FromWireData(Object->TryGetField(TEXT("data")));

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

	OutStruct.Data = FFlockGameConfigData::FromWireData(Object->TryGetField(TEXT("data")));
	return true;
}
