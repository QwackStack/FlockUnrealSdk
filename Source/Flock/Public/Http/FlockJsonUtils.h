// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockResult.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "JsonObjectConverter.h"

/**
 * JSON (de)serialization for the SDK wire format. Owns the one concern that UE's reflection JSON does
 * not handle: the backend speaks snake_case (`game_version_id`, `detail.code`) while USTRUCT fields are
 * PascalCase. Keys are transformed in exactly one place here (the UE analog of Newtonsoft's contract
 * resolver), so models stay idiomatic PascalCase.
 *
 * The transform is bijective for one-capital-per-word field names (GameVersionId <-> game_version_id).
 * An irregular wire name (embedded acronym/number) would need an explicit per-field override; the coming
 * codegen can emit exact maps.
 */
class FLOCK_API FFlockJsonUtils
{
public:
	// ── Case conversion (also used by tests) ──
	/** "game_version_id" -> "GameVersionId". */
	static FString SnakeToPascal(const FString& In);
	/** "GameVersionId"/"gameVersionId" -> "game_version_id". */
	static FString ToSnakeCase(const FString& In);

	/** Parses a JSON object string; returns false on malformed input. */
	static bool TryParseObject(const FString& Json, TSharedPtr<FJsonObject>& OutObject);

	/** Recursively rebuilds an object with its keys (and nested keys) transformed. */
	static TSharedRef<FJsonObject> TransformObjectKeys(const TSharedRef<FJsonObject>& In, bool bToPascal);

	/**
	 * Reads the server's coded-error body ({"detail":{"code","message"}}) into its machine-readable
	 * code and human-readable message, falling back to `error.code` for the enveloped shape. Outputs
	 * are empty when absent; safe on malformed or empty bodies.
	 */
	static void ParseCodedError(const FString& Body, FString& OutCode, FString& OutMessage);

	// ── Templated conversions (T is a USTRUCT) ──

	/**
	 * Serializes a USTRUCT to a snake_case JSON string for a request body. With bOmitEmptyStrings,
	 * top-level fields whose value is an empty string are dropped — for request models with optional
	 * members, where an absent key and an empty string mean different things to the backend.
	 */
	template <typename T>
	static bool StructToWireJson(const T& Struct, FString& OutJson, bool bOmitEmptyStrings = false)
	{
		const TSharedPtr<FJsonObject> SnakeObj = StructToWireObject(Struct, bOmitEmptyStrings);
		if (!SnakeObj.IsValid())
		{
			return false;
		}
		// Condensed, not the default pretty print: request bodies are wire payloads, and callers
		// (and tests) rely on the compact "key":"value" form.
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
		return FJsonSerializer::Serialize(SnakeObj.ToSharedRef(), Writer);
	}

	/**
	 * StructToWireJson stopping before the writer. A model that has to be nested inside a larger
	 * document uses this instead of serializing to a string and parsing it straight back — same
	 * key transform, one pass. Null when the struct could not be converted.
	 */
	template <typename T>
	static TSharedPtr<FJsonObject> StructToWireObject(const T& Struct, bool bOmitEmptyStrings = false)
	{
		const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!FJsonObjectConverter::UStructToJsonObject(T::StaticStruct(), &Struct, Obj, 0, 0))
		{
			return nullptr;
		}
		const TSharedRef<FJsonObject> SnakeObj = TransformObjectKeys(Obj, /*bToPascal*/ false);
		if (bOmitEmptyStrings)
		{
			TArray<FString> EmptyKeys;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : SnakeObj->Values)
			{
				if (Pair.Value.IsValid() && Pair.Value->Type == EJson::String && Pair.Value->AsString().IsEmpty())
				{
					EmptyKeys.Add(Pair.Key);
				}
			}
			for (const FString& Key : EmptyKeys)
			{
				SnakeObj->RemoveField(Key);
			}
		}
		return SnakeObj;
	}

	/** Transforms snake->Pascal keys and converts one already-parsed object into a USTRUCT. */
	template <typename T>
	static bool WireObjectToStruct(const TSharedRef<FJsonObject>& Object, T& OutStruct, FString& OutError)
	{
		const TSharedRef<FJsonObject> Transformed = TransformObjectKeys(Object, /*bToPascal*/ true);
		if (!FJsonObjectConverter::JsonObjectToUStruct(Transformed, T::StaticStruct(), &OutStruct, 0, 0))
		{
			OutError = TEXT("Malformed response body");
			return false;
		}
		return true;
	}

	/** Deserializes a bare (non-enveloped) JSON object body directly into a USTRUCT. */
	template <typename T>
	static bool WireJsonToStruct(const FString& Json, T& OutStruct, FString& OutError)
	{
		TSharedPtr<FJsonObject> Root;
		if (!TryParseObject(Json, Root) || !Root.IsValid())
		{
			OutError = TEXT("Malformed response body");
			return false;
		}
		return WireObjectToStruct(Root.ToSharedRef(), OutStruct, OutError);
	}

	/**
	 * Unwraps the {error, response, result} envelope and deserializes `result` into a USTRUCT. Missing
	 * or null `result` is treated as a serialization failure.
	 */
	template <typename T>
	static bool UnwrapResultToStruct(const FString& Body, T& OutStruct, FString& OutError)
	{
		TSharedPtr<FJsonObject> Root;
		if (!TryParseObject(Body, Root) || !Root.IsValid())
		{
			OutError = TEXT("Malformed response body");
			return false;
		}
		const TSharedPtr<FJsonObject>* ResultObj = nullptr;
		if (!Root->TryGetObjectField(TEXT("result"), ResultObj) || !ResultObj->IsValid())
		{
			OutError = TEXT("Invalid response from server (missing result)");
			return false;
		}
		return WireObjectToStruct((*ResultObj).ToSharedRef(), OutStruct, OutError);
	}

	/**
	 * Unwraps a paginated list ({items, total, page, limit}), from `result` when enveloped or the root
	 * otherwise. Provisional until the first list provider lands — the exact envelope nesting is confirmed then.
	 */
	template <typename T>
	static bool UnwrapPaginated(const FString& Body, TFlockPage<T>& OutPage, FString& OutError)
	{
		TSharedPtr<FJsonObject> Root;
		if (!TryParseObject(Body, Root) || !Root.IsValid())
		{
			OutError = TEXT("Malformed response body");
			return false;
		}
		TSharedPtr<FJsonObject> Page = Root;
		const TSharedPtr<FJsonObject>* ResultObj = nullptr;
		if (Root->TryGetObjectField(TEXT("result"), ResultObj) && ResultObj->IsValid())
		{
			Page = *ResultObj;
		}
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Page->TryGetArrayField(TEXT("items"), Items))
		{
			OutError = TEXT("Invalid paginated response (missing items)");
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Elem : *Items)
		{
			const TSharedPtr<FJsonObject> ElemObj = Elem.IsValid() ? Elem->AsObject() : nullptr;
			if (!ElemObj.IsValid())
			{
				OutError = TEXT("Malformed paginated item");
				return false;
			}
			T Item;
			if (!WireObjectToStruct(ElemObj.ToSharedRef(), Item, OutError))
			{
				return false;
			}
			OutPage.Items.Add(Item);
		}
		Page->TryGetNumberField(TEXT("total"), OutPage.Total);
		Page->TryGetNumberField(TEXT("page"), OutPage.Page);
		Page->TryGetNumberField(TEXT("limit"), OutPage.Limit);
		return true;
	}

};
