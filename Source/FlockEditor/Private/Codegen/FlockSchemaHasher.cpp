// Copyright 2022, Qwacks. All Rights Reserved.

#include "Codegen/FlockSchemaHasher.h"

#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	/** Appends a length-prefixed string, so distinct field content cannot alias to the same canonical text. */
	void AppendString(FStringBuilderBase& Builder, const FString& Value)
	{
		Builder.Appendf(TEXT("%d:"), Value.Len());
		Builder.Append(Value);
	}

	void AppendCanonicalValue(FStringBuilderBase& Builder, const TSharedPtr<FJsonValue>& Value);

	void AppendCanonicalObject(FStringBuilderBase& Builder, const TSharedRef<FJsonObject>& Object)
	{
		// Sorted keys: their order on the wire is a serialization artifact and changes no output.
		TArray<FString> Keys;
		Object->Values.GetKeys(Keys);
		Keys.Sort();

		Builder.Append(TEXT("{"));
		for (int32 Index = 0; Index < Keys.Num(); ++Index)
		{
			if (Index > 0)
			{
				Builder.Append(TEXT(","));
			}
			AppendString(Builder, Keys[Index]);
			Builder.Append(TEXT(":"));
			AppendCanonicalValue(Builder, Object->Values[Keys[Index]]);
		}
		Builder.Append(TEXT("}"));
	}

	void AppendCanonicalValue(FStringBuilderBase& Builder, const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			Builder.Append(TEXT("null"));
			return;
		}
		switch (Value->Type)
		{
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (Object.IsValid())
			{
				AppendCanonicalObject(Builder, Object.ToSharedRef());
			}
			break;
		}
		case EJson::Array:
			// Array order is preserved: it becomes member order in the generated struct, so a reorder is
			// a genuine output change.
			Builder.Append(TEXT("["));
			{
				const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
				for (int32 Index = 0; Index < Items.Num(); ++Index)
				{
					if (Index > 0)
					{
						Builder.Append(TEXT(","));
					}
					AppendCanonicalValue(Builder, Items[Index]);
				}
			}
			Builder.Append(TEXT("]"));
			break;
		case EJson::String:
			AppendString(Builder, Value->AsString());
			break;
		case EJson::Number:
			Builder.Appendf(TEXT("#%s"), *FString::SanitizeFloat(Value->AsNumber()));
			break;
		case EJson::Boolean:
			Builder.Append(Value->AsBool() ? TEXT("true") : TEXT("false"));
			break;
		default:
			Builder.Append(TEXT("null"));
			break;
		}
	}

	/** Sorts by id then name — the same ordering the emitters use, so files and hash agree on identity. */
	template <typename T>
	TArray<const T*> SortedById(const TArray<T>& Items)
	{
		TArray<const T*> Sorted;
		Sorted.Reserve(Items.Num());
		for (const T& Item : Items)
		{
			Sorted.Add(&Item);
		}
		Sorted.Sort([](const T& A, const T& B)
		{
			return A.Id == B.Id ? A.Name < B.Name : A.Id < B.Id;
		});
		return Sorted;
	}
}

FString FFlockSchemaHasher::Canonicalize(const FString& Json)
{
	if (Json.IsEmpty())
	{
		return FString();
	}

	TStringBuilder<1024> Builder;

	// A schema is an array at the top level; tolerate an object too rather than dropping content, and fall
	// back to the raw text when it parses as neither — an unparseable schema still has to hash to
	// *something* stable, or drift detection would silently stop working for it.
	TArray<TSharedPtr<FJsonValue>> AsArray;
	TSharedRef<TJsonReader<TCHAR>> ArrayReader = TJsonReaderFactory<TCHAR>::Create(Json);
	if (FJsonSerializer::Deserialize(ArrayReader, AsArray))
	{
		AppendCanonicalValue(Builder, MakeShared<FJsonValueArray>(AsArray));
		return Builder.ToString();
	}

	TSharedPtr<FJsonObject> AsObject;
	TSharedRef<TJsonReader<TCHAR>> ObjectReader = TJsonReaderFactory<TCHAR>::Create(Json);
	if (FJsonSerializer::Deserialize(ObjectReader, AsObject) && AsObject.IsValid())
	{
		AppendCanonicalObject(Builder, AsObject.ToSharedRef());
		return Builder.ToString();
	}

	return Json;
}

FString FFlockSchemaHasher::ComputeContentHash(const FFlockSchemaSnapshot& Snapshot)
{
	TStringBuilder<4096> Builder;

	for (const FFlockPlayerTemplateSchema* Template : SortedById(Snapshot.PlayerTemplates))
	{
		Builder.Append(TEXT("T"));
		AppendString(Builder, Template->Id);
		AppendString(Builder, Template->Name);
		AppendString(Builder, Template->Tag);
		AppendString(Builder, Canonicalize(Template->SchemaJson));
	}

	for (const FFlockGameConfigSchema* Config : SortedById(Snapshot.GameConfigs))
	{
		Builder.Append(TEXT("C"));
		AppendString(Builder, Config->Id);
		AppendString(Builder, Config->Name);
		AppendString(Builder, Config->Tag);
		AppendString(Builder, Canonicalize(Config->SchemaJson));
	}

	for (const FFlockShop* Shop : SortedById(Snapshot.Shops))
	{
		Builder.Append(TEXT("S"));
		AppendString(Builder, Shop->Id);
		AppendString(Builder, Shop->Name);
		Builder.Append(TEXT("["));
		// Only the fields the generated ids and enums are built from. Price and status are read live, so
		// including them would report drift for a regen that changed nothing.
		for (const FFlockShopItem* Item : SortedById(Shop->ShopItems))
		{
			Builder.Append(TEXT("I"));
			AppendString(Builder, Item->Id);
			AppendString(Builder, Item->Name);
			AppendString(Builder, Item->Currency);
		}
		Builder.Append(TEXT("]"));
	}

	const FTCHARToUTF8 Utf8(Builder.ToString());
	uint8 Digest[20];
	FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);

	FString Hex;
	Hex.Reserve(40);
	for (const uint8 Byte : Digest)
	{
		Hex.Appendf(TEXT("%02x"), Byte);
	}
	return Hex;
}
