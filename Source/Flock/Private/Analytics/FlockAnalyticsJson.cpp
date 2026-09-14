// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Analytics/FlockAnalyticsJson.h"

#include "Http/FlockJsonUtils.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	const TCHAR* WireException = TEXT("exception");
	const TCHAR* WireLogicError = TEXT("logic_error");
	const TCHAR* WireDebug = TEXT("debug");

	/**
	 * Where a spooled entry keeps its answered-failure count. Underscore-prefixed and never written by the wire
	 * serializers, which build bodies from the struct rather than from the stored payload.
	 */
	const TCHAR* FailedSendCountKey = TEXT("_flock_failed_sends");

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

int32 FFlockAnalyticsJson::ReadFailedSendCount(const FString& Payload)
{
	TSharedPtr<FJsonObject> Root;
	if (!FFlockJsonUtils::TryParseObject(Payload, Root) || !Root.IsValid())
	{
		return 0;
	}
	int32 FailedSends = 0;
	Root->TryGetNumberField(FailedSendCountKey, FailedSends);
	return FMath::Max(FailedSends, 0);
}

FString FFlockAnalyticsJson::WithFailedSendCount(const FString& Payload, int32 FailedSends)
{
	TSharedPtr<FJsonObject> Root;
	if (!FFlockJsonUtils::TryParseObject(Payload, Root) || !Root.IsValid())
	{
		return Payload;
	}
	Root->SetNumberField(FailedSendCountKey, FMath::Max(FailedSends, 0));
	return SerializeObject(Root.ToSharedRef());
}

TSharedRef<FJsonObject> FFlockAnalyticsJson::AnalyticsEventToJson(const FFlockAnalyticsEventRequest& Event)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	// Always written, even empty: a held event is never sent, and an empty id on the wire is the server's 404.
	Object->SetStringField(TEXT("player_id"), Event.PlayerId);
	Object->SetStringField(TEXT("event_name"), Event.EventName);
	SetStringIfSet(Object, TEXT("event_category"), Event.EventCategory);
	SetStringIfSet(Object, TEXT("session_id"), Event.SessionId);
	SetStringIfSet(Object, TEXT("timestamp"), Event.Timestamp);
	// The server refuses a null or list here, so an empty bag goes out as {}.
	Object->SetObjectField(TEXT("properties"), Event.Properties.ToJsonObject());
	return Object;
}

FString FFlockAnalyticsJson::SerializeAnalyticsEvents(const TArray<FFlockAnalyticsEventRequest>& Events)
{
	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(Events.Num());
	for (const FFlockAnalyticsEventRequest& Event : Events)
	{
		Items.Add(MakeShared<FJsonValueObject>(AnalyticsEventToJson(Event)));
	}

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("events"), Items);
	return SerializeObject(Root);
}

FString FFlockAnalyticsJson::SerializeSpooledAnalyticsEvent(const FFlockSpooledAnalyticsEvent& Entry)
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetObjectField(TEXT("event"), AnalyticsEventToJson(Entry.Event));
	SetStringIfSet(Root, TEXT("local_session_id"), Entry.LocalSessionId);
	Root->SetNumberField(FailedSendCountKey, FMath::Max(Entry.FailedSends, 0));
	return SerializeObject(Root);
}

bool FFlockAnalyticsJson::DeserializeSpooledAnalyticsEvent(const FString& Json, FFlockSpooledAnalyticsEvent& OutEntry)
{
	OutEntry = FFlockSpooledAnalyticsEvent();

	TSharedPtr<FJsonObject> Root;
	if (!FFlockJsonUtils::TryParseObject(Json, Root) || !Root.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* EventObject = nullptr;
	if (!Root->TryGetObjectField(TEXT("event"), EventObject) || !EventObject->IsValid())
	{
		return false;
	}

	const TSharedRef<FJsonObject> Event = EventObject->ToSharedRef();
	Event->TryGetStringField(TEXT("player_id"), OutEntry.Event.PlayerId);
	if (!Event->TryGetStringField(TEXT("event_name"), OutEntry.Event.EventName) || OutEntry.Event.EventName.IsEmpty())
	{
		return false;
	}
	Event->TryGetStringField(TEXT("event_category"), OutEntry.Event.EventCategory);
	Event->TryGetStringField(TEXT("session_id"), OutEntry.Event.SessionId);
	Event->TryGetStringField(TEXT("timestamp"), OutEntry.Event.Timestamp);

	const TSharedPtr<FJsonObject>* Properties = nullptr;
	if (Event->TryGetObjectField(TEXT("properties"), Properties) && Properties->IsValid())
	{
		OutEntry.Event.Properties = FFlockCommandData::FromJsonString(SerializeObject(Properties->ToSharedRef()));
	}

	Root->TryGetStringField(TEXT("local_session_id"), OutEntry.LocalSessionId);
	int32 FailedSends = 0;
	Root->TryGetNumberField(FailedSendCountKey, FailedSends);
	OutEntry.FailedSends = FMath::Max(FailedSends, 0);
	return true;
}

TSharedPtr<FJsonObject> FFlockAnalyticsJson::SnapshotToJson(const FFlockSessionSnapshot& Snapshot)
{
	// Empty strings are dropped rather than written blank, matching every other body the SDK emits.
	return FFlockJsonUtils::StructToWireObject(Snapshot, /*bOmitEmptyStrings*/ true);
}

bool FFlockAnalyticsJson::SnapshotFromJson(const TSharedRef<FJsonObject>& Object, FFlockSessionSnapshot& OutSnapshot)
{
	OutSnapshot = FFlockSessionSnapshot();
	FString Error;
	return FFlockJsonUtils::WireObjectToStruct(Object, OutSnapshot, Error);
}

FString FFlockAnalyticsJson::SerializeSnapshot(const FFlockSessionSnapshot& Snapshot)
{
	const TSharedPtr<FJsonObject> Object = SnapshotToJson(Snapshot);
	return Object.IsValid() ? SerializeObject(Object.ToSharedRef()) : FString();
}

bool FFlockAnalyticsJson::DeserializeSnapshot(const FString& Json, FFlockSessionSnapshot& OutSnapshot)
{
	OutSnapshot = FFlockSessionSnapshot();
	TSharedPtr<FJsonObject> Root;
	if (!FFlockJsonUtils::TryParseObject(Json, Root) || !Root.IsValid())
	{
		return false;
	}
	return SnapshotFromJson(Root.ToSharedRef(), OutSnapshot);
}
