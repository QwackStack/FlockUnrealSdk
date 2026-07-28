// Copyright 2022, Qwacks. All Rights Reserved.

#include "Codegen/FlockSchemaTypes.h"

#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

EFlockSchemaKind FFlockSchemaTypes::Classify(const FString& WireType)
{
	FString Type = WireType.TrimStartAndEnd().ToLower();
	// See the header: the dashboard's nullable marker is not part of the type.
	Type.RemoveFromEnd(TEXT("?"));

	if (Type == TEXT("string") || Type == TEXT("str") || Type == TEXT("text")
		|| Type == TEXT("datetime") || Type == TEXT("date") || Type == TEXT("timestamp"))
	{
		return EFlockSchemaKind::String;
	}
	if (Type == TEXT("int") || Type == TEXT("integer"))
	{
		return EFlockSchemaKind::Int;
	}
	if (Type == TEXT("long") || Type == TEXT("int64"))
	{
		return EFlockSchemaKind::Int64;
	}
	if (Type == TEXT("float"))
	{
		return EFlockSchemaKind::Float;
	}
	if (Type == TEXT("double") || Type == TEXT("number"))
	{
		return EFlockSchemaKind::Double;
	}
	if (Type == TEXT("bool") || Type == TEXT("boolean"))
	{
		return EFlockSchemaKind::Bool;
	}
	if (Type == TEXT("object"))
	{
		return EFlockSchemaKind::Object;
	}
	if (Type == TEXT("list") || Type == TEXT("array"))
	{
		return EFlockSchemaKind::List;
	}
	if (Type == TEXT("dict"))
	{
		return EFlockSchemaKind::Dict;
	}
	return EFlockSchemaKind::Unknown;
}

TArray<TSharedPtr<FJsonValue>> FFlockSchemaTypes::ParseSchemaArray(const FString& SchemaJson)
{
	TArray<TSharedPtr<FJsonValue>> Entries;
	if (!SchemaJson.IsEmpty())
	{
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(SchemaJson);
		FJsonSerializer::Deserialize(Reader, Entries);
	}
	return Entries;
}

TArray<TSharedPtr<FJsonValue>> FFlockSchemaTypes::ChildEntries(const TSharedRef<FJsonObject>& Field)
{
	TArray<TSharedPtr<FJsonValue>> Children;
	const TSharedPtr<FJsonValue> Child = Field->TryGetField(TEXT("schema"));
	if (!Child.IsValid())
	{
		return Children;
	}
	if (Child->Type == EJson::Array)
	{
		Children = Child->AsArray();
	}
	else if (Child->Type == EJson::Object)
	{
		Children.Add(Child);
	}
	return Children;
}
