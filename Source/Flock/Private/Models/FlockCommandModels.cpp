// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Models/FlockCommandModels.h"

#include "Http/FlockJsonUtils.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	/** The fixed key FFlockCommandValue wraps its payload under. */
	const TCHAR* const ValueKey = TEXT("value");

	FString SerializeObject(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	TSharedPtr<FJsonObject> ParseObject(const FString& Json)
	{
		if (Json.IsEmpty())
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Parsed;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
		if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
		{
			return Parsed;
		}
		return nullptr;
	}

	/** Wraps a JSON value as {"value": ...} so the serializer handles all escaping. */
	FString Wrap(const TSharedPtr<FJsonValue>& Value)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetField(ValueKey, Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
		return SerializeObject(Object);
	}
}

// ─────────────────────────────── FFlockCommandValue ────────────────────────────

FFlockCommandValue::FFlockCommandValue(int32 Value)
	: WrappedJson(Wrap(MakeShared<FJsonValueNumber>(static_cast<double>(Value))))
{
}

FFlockCommandValue::FFlockCommandValue(int64 Value)
	: WrappedJson(Wrap(MakeShared<FJsonValueNumber>(static_cast<double>(Value))))
{
}

FFlockCommandValue::FFlockCommandValue(float Value)
	: WrappedJson(Wrap(MakeShared<FJsonValueNumber>(static_cast<double>(Value))))
{
}

FFlockCommandValue::FFlockCommandValue(double Value)
	: WrappedJson(Wrap(MakeShared<FJsonValueNumber>(Value)))
{
}

FFlockCommandValue::FFlockCommandValue(bool Value)
	: WrappedJson(Wrap(MakeShared<FJsonValueBoolean>(Value)))
{
}

FFlockCommandValue::FFlockCommandValue(const FString& Value)
	: WrappedJson(Wrap(MakeShared<FJsonValueString>(Value)))
{
}

FFlockCommandValue::FFlockCommandValue(const TCHAR* Value)
	: WrappedJson(Wrap(MakeShared<FJsonValueString>(FString(Value))))
{
}

FFlockCommandValue::FFlockCommandValue(const TArray<FString>& Value)
{
	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(Value.Num());
	for (const FString& Item : Value)
	{
		Items.Add(MakeShared<FJsonValueString>(Item));
	}
	WrappedJson = Wrap(MakeShared<FJsonValueArray>(Items));
}

FFlockCommandValue FFlockCommandValue::FromRawJson(const FString& Json)
{
	// Parsed through the same wrapper so an unparseable fragment degrades to null rather than corrupting
	// the body it would be spliced into.
	FFlockCommandValue Result;
	const TSharedPtr<FJsonObject> Wrapper = ParseObject(FString::Printf(TEXT("{\"%s\":%s}"), ValueKey, *Json));
	if (Wrapper.IsValid())
	{
		Result.WrappedJson = SerializeObject(Wrapper.ToSharedRef());
	}
	return Result;
}

FFlockCommandValue FFlockCommandValue::FromJsonValue(const TSharedPtr<FJsonValue>& Value)
{
	FFlockCommandValue Result;
	Result.WrappedJson = Wrap(Value);
	return Result;
}

TSharedPtr<FJsonValue> FFlockCommandValue::ToJsonValue() const
{
	const TSharedPtr<FJsonObject> Wrapper = ParseObject(WrappedJson);
	if (!Wrapper.IsValid())
	{
		return MakeShared<FJsonValueNull>();
	}
	const TSharedPtr<FJsonValue> Value = Wrapper->TryGetField(ValueKey);
	return Value.IsValid() ? Value : MakeShared<FJsonValueNull>();
}

FString FFlockCommandValue::ToJsonString() const
{
	const TSharedPtr<FJsonValue> Value = ToJsonValue();
	if (!Value.IsValid() || Value->Type == EJson::Null)
	{
		return TEXT("null");
	}
	// Round-trips through the wrapper rather than re-serializing by hand: strip the {"value": ... } shell.
	const FString Wrapped = Wrap(Value);
	const FString Prefix = FString::Printf(TEXT("{\"%s\":"), ValueKey);
	if (Wrapped.StartsWith(Prefix) && Wrapped.EndsWith(TEXT("}")))
	{
		return Wrapped.Mid(Prefix.Len(), Wrapped.Len() - Prefix.Len() - 1);
	}
	return Wrapped;
}

// ──────────────────────────────── FFlockCommandData ────────────────────────────

FFlockCommandData& FFlockCommandData::Set(const FString& Key, const FFlockCommandValue& Value)
{
	if (Key.IsEmpty())
	{
		return *this;
	}
	// Re-parsed per Set so the JSON library owns every bit of escaping. A command bag holds a handful of
	// fields, so the cost is noise next to the request it becomes.
	const TSharedRef<FJsonObject> Object = ToJsonObject();
	Object->SetField(Key, Value.ToJsonValue());
	FieldsJson = SerializeObject(Object);
	return *this;
}

bool FFlockCommandData::IsEmpty() const
{
	return ToJsonObject()->Values.Num() == 0;
}

TArray<FString> FFlockCommandData::GetFieldNames() const
{
	return FFlockJsonUtils::GetFieldNames(ToJsonObject());
}

TSharedRef<FJsonObject> FFlockCommandData::ToJsonObject() const
{
	const TSharedPtr<FJsonObject> Parsed = ParseObject(FieldsJson);
	return Parsed.IsValid() ? Parsed.ToSharedRef() : MakeShared<FJsonObject>();
}

FFlockCommandData FFlockCommandData::FromJsonString(const FString& Json)
{
	FFlockCommandData Result;
	const TSharedPtr<FJsonObject> Parsed = ParseObject(Json);
	if (Parsed.IsValid())
	{
		Result.FieldsJson = SerializeObject(Parsed.ToSharedRef());
	}
	return Result;
}
