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

	/** Registered wire names, keyed by struct. See the header for why this is data and not metadata. */
	TMap<const UScriptStruct*, FFlockStructBinder::FWireNameMap>& WireNameRegistry()
	{
		static TMap<const UScriptStruct*, FFlockStructBinder::FWireNameMap> Registry;
		return Registry;
	}

	/** The struct behind a property, when it is one — a plain struct, or a container's element. */
	const UScriptStruct* StructOf(const FProperty* Property)
	{
		const FStructProperty* AsStruct = CastField<FStructProperty>(Property);
		return AsStruct ? AsStruct->Struct : nullptr;
	}
}

void FFlockStructBinder::RegisterWireNames(const UScriptStruct* Struct, FWireNameMap Names)
{
	if (Struct)
	{
		WireNameRegistry().Add(Struct, MoveTemp(Names));
	}
}

void FFlockStructBinder::UnregisterWireNames(const UScriptStruct* Struct)
{
	WireNameRegistry().Remove(Struct);
}

const FFlockStructBinder::FWireNameMap* FFlockStructBinder::FindWireNames(const UScriptStruct* Struct)
{
	return Struct ? WireNameRegistry().Find(Struct) : nullptr;
}

void FFlockStructBinder::ResetWireNames()
{
	WireNameRegistry().Empty();
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
	// every name it could answer to. Matching from one side only silently binds nothing whenever the wire
	// and the member disagree — and Pascal-casing a name that is already Pascal is a no-op, so the failure
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

	const FWireNameMap* WireNames = FindWireNames(Cast<UScriptStruct>(Struct));

	int32 Filled = 0;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		FProperty* Property = *It;
		const FString MemberName = GetMemberName(Struct, Property);

		// A generated C++ member is `GameCurrencies` where the wire says `game_currencies`, so the
		// registered name is tried first — it is the authoritative one — then the member's own spellings.
		const FString* Wire = WireNames ? WireNames->Find(MemberName) : nullptr;
		const TSharedPtr<FJsonValue>* Value = Wire ? ByName.Find(*Wire) : nullptr;
		if (!Value)
		{
			Value = ByName.Find(MemberName);
		}
		if (!Value)
		{
			Value = ByName.Find(FFlockJsonUtils::SnakeToPascal(MemberName));
		}
		if (!Value || !Value->IsValid() || (*Value)->Type == EJson::Null)
		{
			continue;
		}

		// A nested struct recurses here rather than going to the converter, so its members get the same
		// name resolution this loop just did. The converter matches case-insensitively but cannot bridge a
		// spelling difference, which is exactly what a registered wire name is.
		void* MemberPtr = Property->ContainerPtrToValuePtr<void>(StructMemory);
		if (const UScriptStruct* Nested = StructOf(Property))
		{
			const TSharedPtr<FJsonObject>* NestedObject;
			if ((*Value)->TryGetObject(NestedObject) && NestedObject->IsValid())
			{
				FillStructFromObject(Nested, MemberPtr, NestedObject->ToSharedRef());
				++Filled;
			}
			continue;
		}

		// Everything else — scalars, containers, containers of structs — is the converter's job. A
		// container of structs still resolves its element members through the recursion above only if it
		// is a plain struct member; see the note in the header on what is not covered.
		if (FJsonObjectConverter::JsonValueToUProperty(*Value, Property, MemberPtr))
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

	const FWireNameMap* WireNames = FindWireNames(Cast<UScriptStruct>(Struct));

	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		FProperty* Property = *It;
		const FString MemberName = GetMemberName(Struct, Property);
		const void* MemberPtr = Property->ContainerPtrToValuePtr<void>(StructMemory);

		// Precedence: the caller's explicit map, then the registry, then the member's own name — which is
		// already the declared name for a Blueprint struct.
		const FString* Explicit = DeclaredNameByMember.Find(MemberName);
		const FString* Registered = WireNames ? WireNames->Find(MemberName) : nullptr;
		const FString OutName = Explicit ? *Explicit : (Registered ? *Registered : MemberName);

		// A nested struct is built by recursion so its members are wire-named too. Handing it to the
		// converter instead would emit the C++ spelling one level down and the server would reject the
		// write for a property the caller can see is set — the failure this whole mechanism exists for.
		if (const UScriptStruct* Nested = StructOf(Property))
		{
			const FFlockCommandData NestedData = ToCommandData(Nested, MemberPtr);
			Data.SetRawJson(OutName, NestedData.ToJsonString());
			continue;
		}

		// SkipStandardizeCase is not a nicety: without it the converter lower-cases the first letter of
		// every name it writes (`JsonObjectConverter.cpp:22`), which reaches a container element's members
		// and a TMap's keys.
		const TSharedPtr<FJsonValue> Value = FJsonObjectConverter::UPropertyToJsonValue(
			Property, MemberPtr, /*CheckFlags*/ 0, /*SkipFlags*/ 0, /*ExportCb*/ nullptr,
			/*OuterProperty*/ nullptr, EJsonObjectConversionFlags::SkipStandardizeCase);
		if (!Value.IsValid())
		{
			continue;
		}
		Data.Set(OutName, FFlockCommandValue::FromJsonValue(Value));
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
