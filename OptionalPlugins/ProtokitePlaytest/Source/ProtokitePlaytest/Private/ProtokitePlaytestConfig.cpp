// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "ProtokitePlaytestConfig.h"

#include "Http/FlockJsonUtils.h"

namespace
{
	/** A JSON string member, or empty when it is absent or null. */
	FString ReadString(const TSharedRef<FJsonObject>& Object, const TCHAR* Name)
	{
		FString Value;
		Object->TryGetStringField(Name, Value);
		return Value;
	}

	/**
	 * A JSON boolean member, or Fallback when it is absent or holds anything else. Text such as "true", a number
	 * or null is not read as a boolean, so nothing is switched on by guessing what the server meant.
	 */
	bool ReadBool(const TSharedRef<FJsonObject>& Object, const FString& Name, bool Fallback)
	{
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(Name);
		return Value.IsValid() && Value->Type == EJson::Boolean ? Value->AsBool() : Fallback;
	}

	/** The string elements of a JSON array member; anything that is not a string is left out. */
	TArray<FString> ReadStrings(const TSharedRef<FJsonObject>& Object, const TCHAR* Name)
	{
		TArray<FString> Strings;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Object->TryGetArrayField(Name, Values) && Values)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				if (Value.IsValid() && Value->Type == EJson::String)
				{
					Strings.Add(Value->AsString());
				}
			}
		}
		return Strings;
	}

	void ReadFields(const TSharedRef<FJsonObject>& FormObject, TArray<FProtokitePlaytestFormField>& OutFields)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!FormObject->TryGetArrayField(TEXT("fields"), Values) || !Values)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject>* FieldObject = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(FieldObject) || !FieldObject || !FieldObject->IsValid())
			{
				continue;
			}
			const TSharedRef<FJsonObject> Field = FieldObject->ToSharedRef();

			FProtokitePlaytestFormField Question;
			Question.Id = ReadString(Field, TEXT("id"));
			// Answers are submitted by id, so a question without one could never be answered.
			if (Question.Id.IsEmpty())
			{
				continue;
			}
			Question.Type = ReadString(Field, TEXT("type"));
			Question.Label = ReadString(Field, TEXT("label"));
			Question.Required = ReadBool(Field, TEXT("required"), true);
			Question.HelpText = ReadString(Field, TEXT("help_text"));
			Question.Options = ReadStrings(Field, TEXT("options"));
			OutFields.Add(Question);
		}
	}
}

bool FProtokitePlaytestConfig::IsFeatureEnabled(const FString& FeatureName) const
{
	const bool* Enabled = Features.Find(FeatureName);
	return Enabled != nullptr && *Enabled;
}

bool FProtokitePlaytestConfig::FromWireObject(const TSharedRef<FJsonObject>& Object, FProtokitePlaytestConfig& OutConfig,
	FString& OutError)
{
	OutConfig = FProtokitePlaytestConfig();

	OutConfig.TestId = ReadString(Object, TEXT("test_id"));
	if (OutConfig.TestId.IsEmpty())
	{
		OutError = TEXT("The playtest config has no test_id.");
		return false;
	}
	OutConfig.SessionStartedEvent = ReadString(Object, TEXT("session_started_event"));
	OutConfig.FlockGameVersionId = ReadString(Object, TEXT("flock_game_version_id"));

	// Feature names are the server's own keys and are kept exactly as sent. Only a JSON boolean counts.
	const TSharedPtr<FJsonObject>* FeaturesObject = nullptr;
	if (Object->TryGetObjectField(TEXT("features"), FeaturesObject) && FeaturesObject && FeaturesObject->IsValid())
	{
		const TSharedRef<FJsonObject> Features = FeaturesObject->ToSharedRef();
		for (const FString& Name : FFlockJsonUtils::GetFieldNames(*FeaturesObject))
		{
			const TSharedPtr<FJsonValue> Value = Features->TryGetField(Name);
			if (Value.IsValid() && Value->Type == EJson::Boolean)
			{
				OutConfig.Features.Add(Name, Value->AsBool());
			}
		}
	}

	// A null form means the playtest has none published. A form without an id cannot take answers, so it
	// counts as none too.
	const TSharedPtr<FJsonObject>* FormObject = nullptr;
	if (Object->TryGetObjectField(TEXT("form"), FormObject) && FormObject && FormObject->IsValid())
	{
		const TSharedRef<FJsonObject> FormJson = FormObject->ToSharedRef();
		FProtokitePlaytestForm Form;
		Form.Id = ReadString(FormJson, TEXT("id"));
		if (!Form.Id.IsEmpty())
		{
			Form.TestId = ReadString(FormJson, TEXT("test_id"));
			Form.GameId = ReadString(FormJson, TEXT("game_id"));
			Form.Title = ReadString(FormJson, TEXT("title"));
			Form.Description = ReadString(FormJson, TEXT("description"));
			Form.IsPublished = ReadBool(FormJson, TEXT("is_published"), true);
			Form.CreatedAt = ReadString(FormJson, TEXT("created_at"));
			Form.UpdatedAt = ReadString(FormJson, TEXT("updated_at"));
			ReadFields(FormJson, Form.Fields);
			OutConfig.Form = Form;
		}
	}
	return true;
}
