// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Http/FlockJsonUtils.h"

namespace
{
	/** Recursively transforms the keys of objects (and objects nested in arrays). Primitives are shared as-is. */
	TSharedPtr<FJsonValue> TransformValue(const TSharedPtr<FJsonValue>& In, bool bToPascal)
	{
		if (!In.IsValid())
		{
			return In;
		}

		switch (In->Type)
		{
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject> Obj = In->AsObject();
			const TSharedRef<FJsonObject> NewObj = MakeShared<FJsonObject>();
			if (Obj.IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
				{
					const FString NewKey = bToPascal
						? FFlockJsonUtils::SnakeToPascal(Pair.Key)
						: FFlockJsonUtils::ToSnakeCase(Pair.Key);
					NewObj->SetField(NewKey, TransformValue(Pair.Value, bToPascal));
				}
			}
			return MakeShared<FJsonValueObject>(NewObj);
		}
		case EJson::Array:
		{
			TArray<TSharedPtr<FJsonValue>> NewArr;
			for (const TSharedPtr<FJsonValue>& Elem : In->AsArray())
			{
				NewArr.Add(TransformValue(Elem, bToPascal));
			}
			return MakeShared<FJsonValueArray>(NewArr);
		}
		default:
			return In;
		}
	}

	/** At most this many field errors are spelled out; the rest collapse into a "(+n more)" tail. */
	constexpr int32 MaxFieldErrorsShown = 3;

	/** "body.player_data" — the dotted path the framework reports a rejected field at. */
	FString JoinLocation(const TArray<TSharedPtr<FJsonValue>>* Location)
	{
		FString Path;
		if (Location == nullptr)
		{
			return Path;
		}
		for (const TSharedPtr<FJsonValue>& Part : *Location)
		{
			if (!Part.IsValid())
			{
				continue;
			}
			if (!Path.IsEmpty())
			{
				Path += TEXT(".");
			}
			Path += Part->AsString();
		}
		return Path;
	}

	/** "body.player_data: Input should be a valid dictionary" — names the field so the caller can fix the payload. */
	FString DescribeFieldErrors(const TArray<TSharedPtr<FJsonValue>>& Errors)
	{
		FString Text;
		int32 Shown = 0;
		for (const TSharedPtr<FJsonValue>& Entry : Errors)
		{
			if (Shown == MaxFieldErrorsShown)
			{
				Text += FString::Printf(TEXT("; (+%d more)"), Errors.Num() - Shown);
				break;
			}

			const TSharedPtr<FJsonObject>* Error = nullptr;
			if (!Entry.IsValid() || !Entry->TryGetObject(Error) || Error == nullptr || !Error->IsValid())
			{
				continue;
			}

			FString Why;
			if (!(*Error)->TryGetStringField(TEXT("msg"), Why) || Why.IsEmpty())
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Location = nullptr;
			(*Error)->TryGetArrayField(TEXT("loc"), Location);
			const FString Where = JoinLocation(Location);

			if (Shown > 0)
			{
				Text += TEXT("; ");
			}
			Text += Where.IsEmpty() ? Why : FString::Printf(TEXT("%s: %s"), *Where, *Why);
			++Shown;
		}
		return Text;
	}
}

FString FFlockJsonUtils::SnakeToPascal(const FString& In)
{
	TArray<FString> Parts;
	In.ParseIntoArray(Parts, TEXT("_"), /*InCullEmpty*/ true);

	FString Out;
	Out.Reserve(In.Len());
	for (const FString& Part : Parts)
	{
		if (Part.IsEmpty())
		{
			continue;
		}
		Out += Part.Left(1).ToUpper();
		if (Part.Len() > 1)
		{
			Out += Part.RightChop(1);
		}
	}
	// No underscores and already capitalized keys fall through unchanged.
	return Out.IsEmpty() ? In : Out;
}

FString FFlockJsonUtils::ToSnakeCase(const FString& In)
{
	FString Out;
	Out.Reserve(In.Len() * 2);
	for (int32 Index = 0; Index < In.Len(); ++Index)
	{
		const TCHAR Ch = In[Index];
		if (FChar::IsUpper(Ch))
		{
			if (Index > 0)
			{
				Out.AppendChar(TEXT('_'));
			}
			Out.AppendChar(FChar::ToLower(Ch));
		}
		else
		{
			Out.AppendChar(Ch);
		}
	}
	return Out;
}

bool FFlockJsonUtils::TryParseObject(const FString& Json, TSharedPtr<FJsonObject>& OutObject)
{
	if (Json.IsEmpty())
	{
		return false;
	}
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
}

TArray<FString> FFlockJsonUtils::GetFieldNames(const TSharedPtr<FJsonObject>& Object)
{
	TArray<FString> Names;
	if (!Object.IsValid())
	{
		return Names;
	}

	// `auto`, and `*Pair.Key` rather than the key itself: the map's key type changes between engines
	// (FString, then an interned shared string), and dereference is the one spelling both answer with a
	// `const TCHAR*`. Naming the type or copying the key directly compiles on one engine only.
	Names.Reserve(Object->Values.Num());
	for (const auto& Pair : Object->Values)
	{
		Names.Emplace(*Pair.Key);
	}
	return Names;
}

TSharedRef<FJsonObject> FFlockJsonUtils::TransformObjectKeys(const TSharedRef<FJsonObject>& In, bool bToPascal)
{
	const TSharedPtr<FJsonValue> Transformed = TransformValue(MakeShared<FJsonValueObject>(In), bToPascal);
	const TSharedPtr<FJsonObject> Obj = Transformed.IsValid() ? Transformed->AsObject() : nullptr;
	return Obj.IsValid() ? Obj.ToSharedRef() : MakeShared<FJsonObject>();
}

void FFlockJsonUtils::ParseCodedError(const FString& Body, FString& OutCode, FString& OutMessage)
{
	OutCode.Reset();
	OutMessage.Reset();

	TSharedPtr<FJsonObject> Root;
	if (!TryParseObject(Body, Root) || !Root.IsValid())
	{
		return;
	}

	// Two shapes share `detail`: the game routes' coded {code,message} object, and the framework's own
	// 422 array of field errors. Reading only the object left that class of failure with no reason at all.
	const TSharedPtr<FJsonValue> Detail = Root->TryGetField(TEXT("detail"));
	if (Detail.IsValid() && Detail->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> DetailObject = Detail->AsObject();
		if (DetailObject.IsValid())
		{
			DetailObject->TryGetStringField(TEXT("code"), OutCode);
			DetailObject->TryGetStringField(TEXT("message"), OutMessage);
		}
	}
	else if (Detail.IsValid() && Detail->Type == EJson::Array)
	{
		OutMessage = DescribeFieldErrors(Detail->AsArray());
	}
	else if (Detail.IsValid() && Detail->Type == EJson::String)
	{
		OutMessage = Detail->AsString();
	}

	if (OutCode.IsEmpty())
	{
		const TSharedPtr<FJsonObject>* ErrorObj = nullptr;
		if (Root->TryGetObjectField(TEXT("error"), ErrorObj) && ErrorObj->IsValid())
		{
			(*ErrorObj)->TryGetStringField(TEXT("code"), OutCode);
		}
	}
}
