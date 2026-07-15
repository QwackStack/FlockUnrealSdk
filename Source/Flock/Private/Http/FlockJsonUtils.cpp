// Copyright 2022, Qwacks. All Rights Reserved.

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

TSharedRef<FJsonObject> FFlockJsonUtils::TransformObjectKeys(const TSharedRef<FJsonObject>& In, bool bToPascal)
{
	const TSharedPtr<FJsonValue> Transformed = TransformValue(MakeShared<FJsonValueObject>(In), bToPascal);
	const TSharedPtr<FJsonObject> Obj = Transformed.IsValid() ? Transformed->AsObject() : nullptr;
	return Obj.IsValid() ? Obj.ToSharedRef() : MakeShared<FJsonObject>();
}

FString FFlockJsonUtils::ParseCodedErrorCode(const FString& Body)
{
	TSharedPtr<FJsonObject> Root;
	if (!TryParseObject(Body, Root) || !Root.IsValid())
	{
		return FString();
	}

	// Prefer detail.code (CodedErrorResponse), fall back to error.code (GenericResponse.error).
	const TSharedPtr<FJsonObject>* Detail = nullptr;
	if (Root->TryGetObjectField(TEXT("detail"), Detail) && Detail->IsValid())
	{
		FString Code;
		if ((*Detail)->TryGetStringField(TEXT("code"), Code))
		{
			return Code;
		}
	}

	const TSharedPtr<FJsonObject>* ErrorObj = nullptr;
	if (Root->TryGetObjectField(TEXT("error"), ErrorObj) && ErrorObj->IsValid())
	{
		FString Code;
		if ((*ErrorObj)->TryGetStringField(TEXT("code"), Code))
		{
			return Code;
		}
	}

	return FString();
}
