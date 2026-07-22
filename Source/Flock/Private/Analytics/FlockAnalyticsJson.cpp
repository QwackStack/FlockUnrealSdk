// Copyright 2022, Qwacks. All Rights Reserved.

#include "Analytics/FlockAnalyticsJson.h"

#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	const TCHAR* WireException = TEXT("exception");
	const TCHAR* WireLogicError = TEXT("logic_error");
	const TCHAR* WireDebug = TEXT("debug");

	void SetStringIfSet(const TSharedRef<FJsonObject>& Object, const TCHAR* Key, const FString& Value)
	{
		if (!Value.IsEmpty())
		{
			Object->SetStringField(Key, Value);
		}
	}

	/** Writes the caller's map with its keys exactly as given — no case transform. */
	void SetMapIfSet(const TSharedRef<FJsonObject>& Object, const TCHAR* Key, const TMap<FString, FString>& Map)
	{
		if (Map.Num() == 0)
		{
			return;
		}
		const TSharedRef<FJsonObject> Nested = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : Map)
		{
			Nested->SetStringField(Pair.Key, Pair.Value);
		}
		Object->SetObjectField(Key, Nested);
	}

	void ReadMap(const TSharedRef<FJsonObject>& Object, const TCHAR* Key, TMap<FString, FString>& OutMap)
	{
		OutMap.Reset();
		const TSharedPtr<FJsonObject>* Nested = nullptr;
		if (!Object->TryGetObjectField(Key, Nested) || !Nested->IsValid())
		{
			return;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Nested)->Values)
		{
			if (Pair.Value.IsValid())
			{
				OutMap.Add(Pair.Key, Pair.Value->AsString());
			}
		}
	}

	FString SerializeObject(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}
}

FString FFlockAnalyticsJson::LogEventTypeToWire(EFlockLogEventType Type)
{
	switch (Type)
	{
	case EFlockLogEventType::Exception:
		return WireException;
	case EFlockLogEventType::LogicError:
		return WireLogicError;
	default:
		return WireDebug;
	}
}

EFlockLogEventType FFlockAnalyticsJson::WireToLogEventType(const FString& Wire)
{
	if (Wire.Equals(WireException, ESearchCase::IgnoreCase))
	{
		return EFlockLogEventType::Exception;
	}
	if (Wire.Equals(WireLogicError, ESearchCase::IgnoreCase))
	{
		return EFlockLogEventType::LogicError;
	}
	return EFlockLogEventType::Debug;
}

TSharedRef<FJsonObject> FFlockAnalyticsJson::ToJson(const FFlockLogEventRequest& Event)
{
	const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("type"), LogEventTypeToWire(Event.Data.Type));
	SetStringIfSet(Data, TEXT("game_version"), Event.Data.GameVersion);
	SetStringIfSet(Data, TEXT("logical_expression"), Event.Data.LogicalExpression);
	SetStringIfSet(Data, TEXT("error_message"), Event.Data.ErrorMessage);
	SetStringIfSet(Data, TEXT("error_code"), Event.Data.ErrorCode);
	SetMapIfSet(Data, TEXT("error_data"), Event.Data.ErrorData);
	SetStringIfSet(Data, TEXT("error_traceback"), Event.Data.ErrorTraceback);
	if (Event.Data.ErrorTracebackLines.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Lines;
		Lines.Reserve(Event.Data.ErrorTracebackLines.Num());
		for (const FString& Line : Event.Data.ErrorTracebackLines)
		{
			Lines.Add(MakeShared<FJsonValueString>(Line));
		}
		Data->SetArrayField(TEXT("error_traceback_lines"), Lines);
	}
	SetMapIfSet(Data, TEXT("extra_data"), Event.Data.ExtraData);

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("message"), Event.Message);
	Root->SetObjectField(TEXT("data"), Data);
	SetStringIfSet(Root, TEXT("timestamp"), Event.Timestamp);
	return Root;
}

bool FFlockAnalyticsJson::FromJson(const TSharedRef<FJsonObject>& Object, FFlockLogEventRequest& OutEvent)
{
	OutEvent = FFlockLogEventRequest();

	const TSharedPtr<FJsonObject>* Data = nullptr;
	if (!Object->TryGetObjectField(TEXT("data"), Data) || !Data->IsValid())
	{
		return false;
	}
	const TSharedRef<FJsonObject> DataRef = Data->ToSharedRef();

	Object->TryGetStringField(TEXT("message"), OutEvent.Message);
	Object->TryGetStringField(TEXT("timestamp"), OutEvent.Timestamp);

	FString TypeWire;
	DataRef->TryGetStringField(TEXT("type"), TypeWire);
	OutEvent.Data.Type = WireToLogEventType(TypeWire);

	DataRef->TryGetStringField(TEXT("game_version"), OutEvent.Data.GameVersion);
	DataRef->TryGetStringField(TEXT("logical_expression"), OutEvent.Data.LogicalExpression);
	DataRef->TryGetStringField(TEXT("error_message"), OutEvent.Data.ErrorMessage);
	DataRef->TryGetStringField(TEXT("error_code"), OutEvent.Data.ErrorCode);
	DataRef->TryGetStringField(TEXT("error_traceback"), OutEvent.Data.ErrorTraceback);
	DataRef->TryGetStringArrayField(TEXT("error_traceback_lines"), OutEvent.Data.ErrorTracebackLines);
	ReadMap(DataRef, TEXT("error_data"), OutEvent.Data.ErrorData);
	ReadMap(DataRef, TEXT("extra_data"), OutEvent.Data.ExtraData);
	return true;
}

FString FFlockAnalyticsJson::SerializeEvent(const FFlockLogEventRequest& Event)
{
	return SerializeObject(ToJson(Event));
}

FString FFlockAnalyticsJson::SerializeEvents(const TArray<FFlockLogEventRequest>& Events)
{
	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(Events.Num());
	for (const FFlockLogEventRequest& Event : Events)
	{
		Items.Add(MakeShared<FJsonValueObject>(ToJson(Event)));
	}

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("events"), Items);
	return SerializeObject(Root);
}

bool FFlockAnalyticsJson::DeserializeEvent(const FString& Json, FFlockLogEventRequest& OutEvent)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return false;
	}
	return FromJson(Root.ToSharedRef(), OutEvent);
}
