// Copyright 2022, Qwacks. All Rights Reserved.

#include "Models/FlockJsonData.h"

#include "Http/FlockJsonUtils.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FFlockJsonData FFlockJsonData::FromJson(const TSharedPtr<FJsonValue>& Value)
{
	FFlockJsonData Result;
	if (!Value.IsValid() || Value->Type != EJson::Object)
	{
		return Result; // no object — a valid-but-empty handle
	}

	const TSharedPtr<FJsonObject> Object = Value->AsObject();
	if (Object.IsValid())
	{
		// Serialized verbatim — author keys are kept as-is, never case-transformed.
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		Result.Json = Out;
	}
	return Result;
}

bool FFlockJsonData::IsValid() const
{
	return ResolveObject().IsValid();
}

TSharedPtr<FJsonObject> FFlockJsonData::ResolveObject() const
{
	if (!bParsed)
	{
		bParsed = true;
		if (!Json.IsEmpty())
		{
			TSharedPtr<FJsonObject> Parsed;
			const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
			if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
			{
				CachedObject = Parsed;
			}
		}
	}
	return CachedObject;
}

TSharedPtr<FJsonValue> FFlockJsonData::ResolvePath(const FString& Path) const
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
		// Exact first (matches a verbatim key), then Pascal (matches when the caller typed snake_case).
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

bool FFlockJsonData::TryGetString(const FString& Path, FString& OutValue) const
{
	const TSharedPtr<FJsonValue> Value = ResolvePath(Path);
	return Value.IsValid() && Value->TryGetString(OutValue);
}

bool FFlockJsonData::TryGetInt(const FString& Path, int32& OutValue) const
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

bool FFlockJsonData::TryGetFloat(const FString& Path, float& OutValue) const
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

bool FFlockJsonData::TryGetBool(const FString& Path, bool& OutValue) const
{
	const TSharedPtr<FJsonValue> Value = ResolvePath(Path);
	return Value.IsValid() && Value->TryGetBool(OutValue);
}

bool FFlockJsonData::TryGetStringArray(const FString& Path, TArray<FString>& OutValue) const
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

bool FFlockJsonData::HasField(const FString& Path) const
{
	const TSharedPtr<FJsonValue> Value = ResolvePath(Path);
	return Value.IsValid() && Value->Type != EJson::Null;
}

TArray<FString> FFlockJsonData::GetFieldNames() const
{
	TArray<FString> Names;
	const TSharedPtr<FJsonObject> Object = ResolveObject();
	if (Object.IsValid())
	{
		Object->Values.GetKeys(Names);
	}
	return Names;
}
