// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Analytics/FlockGameplayEvents.h"
#include "Flock.h"
#include "FlockSubsystem.h"
#include "Http/FlockJsonUtils.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace FlockGameplayEvents
{
	FFlockCommandData WithNamedProperties(const FFlockCommandData& ExtraProperties, TArrayView<const FNamedProperty> Named)
	{
		// ToJsonObject builds a fresh object from the game's, so nothing below reaches the game's own properties.
		const TSharedRef<FJsonObject> Object = ExtraProperties.ToJsonObject();
		for (const FNamedProperty& Property : Named)
		{
			if (!Property.Value.IsSet())
			{
				continue;
			}
			const FString Key = Property.Key;
			const FString OtherSpelling = FFlockJsonUtils::OtherSpellingOf(FFlockJsonUtils::GetFieldNames(Object), Key);
			if (!OtherSpelling.IsEmpty())
			{
				UE_LOG(LogFlock, Warning, TEXT("[Flock SDK] Extra property '%s' is not sent: the call's own '%s' argument replaces it. ")
					TEXT("This engine's JSON object cannot hold two spellings of one key; give the extra property another name to send both."),
					*OtherSpelling, *Key);
			}
			// The object's lookup ignores letter case, and setting a key replaces the entry whole, its spelling included: the
			// game's other spelling goes with its value (Flock.Analytics.GameplayEvents pins this on every engine).
			Object->SetField(Key, Property.Value.GetValue().ToJsonValue());
		}
		FString Json;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Object, Writer);
		return FFlockCommandData::FromJsonString(Json);
	}

	bool TrackLevelStartFromBlueprint(UFlockSubsystem* Sdk, int32 Level, const FString& Difficulty,
		const FFlockCommandData& ExtraProperties)
	{
		return Sdk != nullptr && Sdk->TrackLevelStart(FromBlueprintNumber(Level), Difficulty, ExtraProperties);
	}

	bool TrackLevelCompleteFromBlueprint(UFlockSubsystem* Sdk, int32 Level, int32 Score, int32 DurationSeconds, int32 Stars,
		const FFlockCommandData& ExtraProperties)
	{
		return Sdk != nullptr && Sdk->TrackLevelComplete(FromBlueprintNumber(Level), FromBlueprintNumber(Score),
			FromBlueprintNumber(DurationSeconds), FromBlueprintNumber(Stars), ExtraProperties);
	}

	bool TrackLevelFailFromBlueprint(UFlockSubsystem* Sdk, int32 Level, const FString& Reason, int32 DurationSeconds,
		const FFlockCommandData& ExtraProperties)
	{
		return Sdk != nullptr && Sdk->TrackLevelFail(FromBlueprintNumber(Level), Reason, FromBlueprintNumber(DurationSeconds),
			ExtraProperties);
	}

	bool TrackCheckpointFromBlueprint(UFlockSubsystem* Sdk, int32 Level, const FString& Checkpoint,
		const FFlockCommandData& ExtraProperties)
	{
		return Sdk != nullptr && Sdk->TrackCheckpoint(FromBlueprintNumber(Level), Checkpoint, ExtraProperties);
	}

	bool TrackTutorialStepFromBlueprint(UFlockSubsystem* Sdk, int32 Step, const FString& StepName,
		const FFlockCommandData& ExtraProperties)
	{
		return Sdk != nullptr && Sdk->TrackTutorialStep(FromBlueprintNumber(Step), StepName, ExtraProperties);
	}

	bool TrackSettingsChangeFromBlueprint(UFlockSubsystem* Sdk, const FString& Setting, const FString& Value,
		const FFlockCommandData& ExtraProperties)
	{
		return Sdk != nullptr && Sdk->TrackSettingsChange(Setting, Value, ExtraProperties);
	}

	bool TrackMatchStartFromBlueprint(UFlockSubsystem* Sdk, const FString& Mode, const FString& Map, int32 TeamSize,
		const FFlockCommandData& ExtraProperties)
	{
		return Sdk != nullptr && Sdk->TrackMatchStart(Mode, Map, FromBlueprintNumber(TeamSize), ExtraProperties);
	}

	bool TrackMatchCompleteFromBlueprint(UFlockSubsystem* Sdk, const FString& Mode, const FString& Result, int32 Score,
		int32 DurationSeconds, int32 Kills, const FFlockCommandData& ExtraProperties)
	{
		return Sdk != nullptr && Sdk->TrackMatchComplete(Mode, Result, FromBlueprintNumber(Score),
			FromBlueprintNumber(DurationSeconds), FromBlueprintNumber(Kills), ExtraProperties);
	}
}
