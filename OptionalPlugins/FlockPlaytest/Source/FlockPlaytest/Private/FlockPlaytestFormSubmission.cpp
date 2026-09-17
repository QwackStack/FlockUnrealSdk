// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockPlaytestFormSubmission.h"

#include "Dom/JsonObject.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	FString WriteCondensed(const TSharedRef<FJsonObject>& Object)
	{
		FString Json;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Object, Writer);
		return Json;
	}

	TSharedPtr<FJsonObject> ReadObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		return FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid() ? Object : nullptr;
	}
}

FString FFlockPlaytestFormSubmission::ToJson() const
{
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();

	if (!PlaytestSessionId.IsEmpty())
	{
		Body->SetStringField(TEXT("session_id"), PlaytestSessionId);
	}
	if (!Identity.SteamId.IsEmpty())
	{
		Body->SetStringField(TEXT("steam_id"), Identity.SteamId);
	}
	if (!Identity.DeviceId.IsEmpty())
	{
		Body->SetStringField(TEXT("device_id"), Identity.DeviceId);
	}

	// The answers were shaped when they were collected, so they go on the wire as they are. Reading them back here
	// rather than rebuilding them means what was kept on disk is exactly what is sent.
	const TSharedPtr<FJsonObject> Answers = ReadObject(AnswersJson);
	Body->SetObjectField(TEXT("answers"), Answers.IsValid() ? Answers : MakeShared<FJsonObject>());

	return WriteCondensed(Body);
}

FString FFlockPlaytestFormSubmission::ToSavedJson() const
{
	const TSharedRef<FJsonObject> Saved = MakeShared<FJsonObject>();
	Saved->SetStringField(TEXT("playtest_session_id"), PlaytestSessionId);
	Saved->SetStringField(TEXT("steam_id"), Identity.SteamId);
	Saved->SetStringField(TEXT("device_id"), Identity.DeviceId);
	Saved->SetStringField(TEXT("protokite_api_url"), ProtokiteApiUrl);
	Saved->SetStringField(TEXT("flock_game_version_id"), FlockGameVersionId);

	const TSharedPtr<FJsonObject> Answers = ReadObject(AnswersJson);
	Saved->SetObjectField(TEXT("answers"), Answers.IsValid() ? Answers : MakeShared<FJsonObject>());

	// No API key, ever: a later launch sends its own, and a file left on a player's disk should carry nothing secret.
	return WriteCondensed(Saved);
}

bool FFlockPlaytestFormSubmission::FromSavedJson(const FString& Json, FFlockPlaytestFormSubmission& OutSubmission,
	FString& OutError)
{
	OutSubmission = FFlockPlaytestFormSubmission();

	const TSharedPtr<FJsonObject> Saved = ReadObject(Json);
	if (!Saved.IsValid())
	{
		OutError = TEXT("The kept feedback form is not readable.");
		return false;
	}

	Saved->TryGetStringField(TEXT("playtest_session_id"), OutSubmission.PlaytestSessionId);
	Saved->TryGetStringField(TEXT("steam_id"), OutSubmission.Identity.SteamId);
	Saved->TryGetStringField(TEXT("device_id"), OutSubmission.Identity.DeviceId);
	Saved->TryGetStringField(TEXT("protokite_api_url"), OutSubmission.ProtokiteApiUrl);
	Saved->TryGetStringField(TEXT("flock_game_version_id"), OutSubmission.FlockGameVersionId);

	if (OutSubmission.Identity.IsEmpty())
	{
		// The server refuses one with nobody to attribute it to, so sending it would only spend a request.
		OutError = TEXT("The kept feedback form names nobody who filled it in.");
		return false;
	}
	if (OutSubmission.ProtokiteApiUrl.IsEmpty())
	{
		OutError = TEXT("The kept feedback form names nowhere to send it.");
		return false;
	}

	const TSharedPtr<FJsonObject>* Answers = nullptr;
	if (!Saved->TryGetObjectField(TEXT("answers"), Answers) || !(*Answers).IsValid())
	{
		OutError = TEXT("The kept feedback form holds no answers.");
		return false;
	}
	OutSubmission.AnswersJson = WriteCondensed((*Answers).ToSharedRef());
	return true;
}

FString FlockPlaytestFindFieldIdInComplaint(const FString& Message)
{
	int32 Start = INDEX_NONE;
	if (!Message.FindChar(TCHAR('\''), Start))
	{
		return FString();
	}
	int32 End = INDEX_NONE;
	if (!Message.FindChar(TCHAR('\''), End) || !Message.RightChop(Start + 1).FindChar(TCHAR('\''), End))
	{
		return FString();
	}
	// End is measured from just past the opening quote.
	return Message.Mid(Start + 1, End);
}
