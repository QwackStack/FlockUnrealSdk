// Shared JSON helpers used by Analytics and LogEvent serialization. Header-only because
// callers already pay the JSON module include cost, and inlining keeps the helpers cheap.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "JsonObjectConverter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace QwackJson
{
	// If Parent[Key] is a JSON-object string, replace it with the parsed object.
	// Empty string removes the field; non-JSON string is left as-is.
	inline void EmbedJsonStringField(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Key)
	{
		if (!Parent.IsValid()) return;
		FString StrVal;
		if (!Parent->TryGetStringField(Key, StrVal)) return;
		if (StrVal.IsEmpty()) { Parent->RemoveField(Key); return; }

		TSharedPtr<FJsonObject> AsObj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(StrVal);
		if (FJsonSerializer::Deserialize(Reader, AsObj) && AsObj.IsValid())
		{
			Parent->SetObjectField(Key, AsObj);
		}
	}

	inline FString JsonObjectToString(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	inline TSharedPtr<FJsonObject> StructToJsonObject(const UStruct* StructDef, const void* StructPtr)
	{
		const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!FJsonObjectConverter::UStructToJsonObject(StructDef, StructPtr, Obj, 0, 0))
		{
			return nullptr;
		}
		return Obj;
	}
}
