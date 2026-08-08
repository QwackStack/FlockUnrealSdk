// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

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
#include <type_traits>

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

	/**
	 * The field names of a JSON object, in author spelling, in map order.
	 *
	 * The one JSON operation with no engine API behind it, which is why it is worth centralising: every
	 * other access this SDK needs (`HasField`, `TryGetField`, `SetField`) has a supported signature that
	 * is stable across engines, but enumerating keys means touching `FJsonObject::Values` directly -- and
	 * that container's key *type* is not stable. Later engines key it by a shared interned string rather
	 * than FString. Both spellings dereference to `const TCHAR*`, so building the names through that is
	 * portable and needs no version guard; doing it here means the one unstable spelling lives in one
	 * place instead of five call sites.
	 *
	 * Null or empty object yields an empty array.
	 */
	static TArray<FString> GetFieldNames(const TSharedPtr<FJsonObject>& Object);

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

	/**
	 * True when T supplies its own wire parse — a static
	 * `bool FromWireObject(const TSharedRef<FJsonObject>&, T&, FString&)` — instead of the reflection
	 * path. Models whose members cannot arrive through reflection (a config's flattened opaque data, its
	 * verbatim schema JSON) declare it; everything else leaves it off and takes the default path.
	 */
	template <typename T>
	static constexpr bool HasCustomWireParse() { return TFromWireObjectDetector<T>::Value; }

	/**
	 * Transforms snake->Pascal keys and converts one already-parsed object into a USTRUCT. A type that
	 * declares a static FromWireObject takes that path instead — the one place the choice is made, so
	 * Get<T>/GetList<T>/UnwrapResult* and snapshot reads all honor it with no call-site changes. The
	 * custom path owns its own key handling and must not be re-transformed here (a config's dictionary
	 * keys are author data, not field names).
	 */
	template <typename T>
	static bool WireObjectToStruct(const TSharedRef<FJsonObject>& Object, T& OutStruct, FString& OutError)
	{
		if constexpr (HasCustomWireParse<T>())
		{
			return T::FromWireObject(Object, OutStruct, OutError);
		}
		else
		{
			const TSharedRef<FJsonObject> Transformed = TransformObjectKeys(Object, /*bToPascal*/ true);
			if (!FJsonObjectConverter::JsonObjectToUStruct(Transformed, T::StaticStruct(), &OutStruct, 0, 0))
			{
				OutError = TEXT("Malformed response body");
				return false;
			}
			return true;
		}
	}

	// ── Plain round-trip (snapshot persistence: symmetric, no snake<->Pascal transform) ──
	// The offline snapshot store persists an already-parsed PascalCase model and reads it back later. That
	// is an internal format, not the wire, so it must NOT run the key transform (which is one-directional
	// by intent and would mangle a config's data on the way back). Every member therefore has to be
	// reflectable — a model that hides data behind a custom wire parse keeps it in a reflectable field.

	/** USTRUCT -> condensed plain JSON (PascalCase keys), for snapshot storage. */
	template <typename T>
	static bool StructToPlainJson(const T& Struct, FString& OutJson)
	{
		const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!FJsonObjectConverter::UStructToJsonObject(T::StaticStruct(), &Struct, Obj, 0, 0))
		{
			return false;
		}
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
		return FJsonSerializer::Serialize(Obj, Writer);
	}

	/** Plain JSON (PascalCase keys) -> USTRUCT, the inverse of StructToPlainJson. */
	template <typename T>
	static bool PlainJsonToStruct(const FString& Json, T& OutStruct)
	{
		TSharedPtr<FJsonObject> Root;
		if (!TryParseObject(Json, Root) || !Root.IsValid())
		{
			return false;
		}
		return FJsonObjectConverter::JsonObjectToUStruct(Root.ToSharedRef(), T::StaticStruct(), &OutStruct, 0, 0);
	}

	/** TArray<USTRUCT> -> condensed plain JSON array, for snapshotting a list result. */
	template <typename T>
	static bool ArrayToPlainJson(const TArray<T>& Items, FString& OutJson)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Items.Num());
		for (const T& Item : Items)
		{
			const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			if (!FJsonObjectConverter::UStructToJsonObject(T::StaticStruct(), &Item, Obj, 0, 0))
			{
				return false;
			}
			Values.Add(MakeShared<FJsonValueObject>(Obj));
		}
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
		return FJsonSerializer::Serialize(Values, Writer);
	}

	/** Plain JSON array -> TArray<USTRUCT>, the inverse of ArrayToPlainJson. */
	template <typename T>
	static bool ArrayFromPlainJson(const FString& Json, TArray<T>& OutItems)
	{
		TSharedPtr<FJsonValue> Root;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid() || Root->Type != EJson::Array)
		{
			return false;
		}
		OutItems.Reset();
		for (const TSharedPtr<FJsonValue>& Elem : Root->AsArray())
		{
			if (!Elem.IsValid() || Elem->Type != EJson::Object)
			{
				return false;
			}
			T Item;
			if (!FJsonObjectConverter::JsonObjectToUStruct(Elem->AsObject().ToSharedRef(), T::StaticStruct(), &Item, 0, 0))
			{
				return false;
			}
			OutItems.Add(Item);
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
	 * Unwraps the {error, response, result} envelope where `result` is a JSON array, deserializing each
	 * element into a T. The enveloped-list counterpart of UnwrapResultToStruct — five config/patch routes
	 * answer with `result: [...]`, which UnwrapResultToStruct (object-only) would reject. A missing or
	 * non-array `result`, or a non-object element, is a serialization failure. This is not the paginated
	 * shape ({items,total,page,limit}) — see UnwrapPaginated.
	 */
	template <typename T>
	static bool UnwrapResultToArray(const FString& Body, TArray<T>& OutArray, FString& OutError)
	{
		TSharedPtr<FJsonObject> Root;
		if (!TryParseObject(Body, Root) || !Root.IsValid())
		{
			OutError = TEXT("Malformed response body");
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* ResultArray = nullptr;
		if (!Root->TryGetArrayField(TEXT("result"), ResultArray))
		{
			OutError = TEXT("Invalid response from server (missing result)");
			return false;
		}
		OutArray.Reset();
		for (const TSharedPtr<FJsonValue>& Elem : *ResultArray)
		{
			// Check the type before AsObject(): on a non-object, FJsonValue::AsObject() returns a valid
			// *empty* object (and logs an error) rather than null, so an IsValid() guard would silently
			// accept a string/number element as an empty struct.
			if (!Elem.IsValid() || Elem->Type != EJson::Object)
			{
				OutError = TEXT("Malformed response element");
				return false;
			}
			T Item;
			if (!WireObjectToStruct(Elem->AsObject().ToSharedRef(), Item, OutError))
			{
				return false;
			}
			OutArray.Add(Item);
		}
		return true;
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
	 * otherwise. Distinct from UnwrapResultToArray: the config/patch list routes answer with `result` as a
	 * bare array (no items/total/page/limit wrapper), which the config provider confirmed is a separate
	 * shape from this one — the two are not interchangeable. This stays for the routes that are genuinely
	 * paginated.
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

private:
	/**
	 * Detects a static `bool FromWireObject(const TSharedRef<FJsonObject>&, T&, FString&)` on T. Drives
	 * the branch in WireObjectToStruct via HasCustomWireParse<T>().
	 */
	template <typename T, typename = void>
	struct TFromWireObjectDetector
	{
		static constexpr bool Value = false;
	};

	template <typename T>
	struct TFromWireObjectDetector<T, std::void_t<decltype(T::FromWireObject(
		std::declval<const TSharedRef<FJsonObject>&>(), std::declval<T&>(), std::declval<FString&>()))>>
	{
		static constexpr bool Value = true;
	};
};
