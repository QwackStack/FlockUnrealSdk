// Copyright 2022, Qwacks. All Rights Reserved.

#include "Codegen/FlockStructBinder.h"

#include "Http/FlockJsonUtils.h"
#include "JsonObjectConverter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"

namespace
{
	TSharedPtr<FJsonObject> ParseObject(const FString& Json)
	{
		if (Json.IsEmpty())
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Parsed;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
		return (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid()) ? Parsed : nullptr;
	}
}

int32 FFlockStructBinder::FillStruct(const UStruct* Struct, void* StructMemory, const FFlockStructuredData& Data)
{
	const TSharedPtr<FJsonObject> Source = ParseObject(Data.ToJsonString());
	if (!Source.IsValid())
	{
		return 0;
	}
	return FillStructFromObject(Struct, StructMemory, Source.ToSharedRef());
}

int32 FFlockStructBinder::FillStructFromObject(const UStruct* Struct, void* StructMemory, const TSharedRef<FJsonObject>& Source)
{
	if (!Struct || !StructMemory)
	{
		return 0;
	}

	// Index the source under each key's own spelling and its Pascal form, then look each member up under
	// both of its spellings too. Matching from one side only silently binds nothing whenever the wire and
	// the member disagree — and Pascal-casing a name that is already Pascal is a no-op, so the failure
	// looks exactly like a working lookup.
	TMap<FString, TSharedPtr<FJsonValue>> ByName;
	ByName.Reserve(Source->Values.Num() * 2);
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Source->Values)
	{
		ByName.Add(Pair.Key, Pair.Value);
		const FString Pascal = FFlockJsonUtils::SnakeToPascal(Pair.Key);
		if (!ByName.Contains(Pascal))
		{
			ByName.Add(Pascal, Pair.Value);
		}
	}

	int32 Filled = 0;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		FProperty* Property = *It;
		const FString MemberName = GetMemberName(Struct, Property);

		const TSharedPtr<FJsonValue>* Value = ByName.Find(MemberName);
		if (!Value)
		{
			Value = ByName.Find(FFlockJsonUtils::SnakeToPascal(MemberName));
		}
		if (!Value || !Value->IsValid() || (*Value)->Type == EJson::Null)
		{
			continue;
		}

		// The converter owns every pin/property type, including nested structs and containers, so an
		// unusual field degrades to "not bound" rather than to a wrong value.
		if (FJsonObjectConverter::JsonValueToUProperty(*Value, Property, Property->ContainerPtrToValuePtr<void>(StructMemory)))
		{
			++Filled;
		}
	}
	return Filled;
}

FFlockCommandData FFlockStructBinder::ToCommandData(const UStruct* Struct, const void* StructMemory)
{
	return ToCommandData(Struct, StructMemory, TMap<FString, FString>());
}

FFlockCommandData FFlockStructBinder::ToCommandData(const UStruct* Struct, const void* StructMemory,
	const TMap<FString, FString>& DeclaredNameByMember)
{
	FFlockCommandData Data;
	if (!Struct || !StructMemory)
	{
		return Data;
	}

	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		FProperty* Property = *It;
		const FString MemberName = GetMemberName(Struct, Property);

		// SkipStandardizeCase is not a nicety. Left on, the converter lower-cases the first letter of
		// every name it emits (`JsonObjectConverter.cpp:22`) — which this loop never sees, because it only
		// touches names the converter itself writes: the members of a *nested* struct, and the keys of a
		// TMap. So a top-level `Level` survived while the `Map` inside it went out as `map`, and the
		// server rejected the write for a missing required property that was right there. It also rewrites
		// `ID` to `Id` mid-name. Author names are data — the same rule as declared field names elsewhere.
		const TSharedPtr<FJsonValue> Value = FJsonObjectConverter::UPropertyToJsonValue(
			Property, Property->ContainerPtrToValuePtr<void>(StructMemory),
			/*CheckFlags*/ 0, /*SkipFlags*/ 0, /*ExportCb*/ nullptr, /*OuterProperty*/ nullptr,
			EJsonObjectConversionFlags::SkipStandardizeCase);
		if (!Value.IsValid())
		{
			continue;
		}

		// The declared name wins where one was supplied; otherwise the member is already named as the
		// template declares it.
		const FString* Declared = DeclaredNameByMember.Find(MemberName);
		Data.Set(Declared ? *Declared : MemberName, FFlockCommandValue::FromJsonValue(Value));
	}
	return Data;
}

FString FFlockStructBinder::GetMemberName(const UStruct* Struct, const FProperty* Property)
{
	if (!Property)
	{
		return FString();
	}
	// UUserDefinedStruct overrides this to strip the GUID suffix its properties carry; a C++ struct
	// returns the plain field name, so one call covers both flavours.
	return Struct ? Struct->GetAuthoredNameForField(Property) : Property->GetName();
}
