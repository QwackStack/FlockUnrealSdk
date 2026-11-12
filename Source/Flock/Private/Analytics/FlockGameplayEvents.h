// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Models/FlockCommandModels.h"

class UFlockSubsystem;

/**
 * The tracking plan's common gameplay events, sent by UFlockSubsystem's typed calls (TrackLevelStart and the rest): each is
 * a TrackAnalyticsEvent with the plan's event, category and property names.
 */
namespace FlockGameplayEvents
{
	inline const TCHAR* const GameplayCategory = TEXT("gameplay");
	inline const TCHAR* const InterfaceCategory = TEXT("ui");

	inline const TCHAR* const LevelStart = TEXT("level_start");
	inline const TCHAR* const LevelComplete = TEXT("level_complete");
	inline const TCHAR* const LevelFail = TEXT("level_fail");
	inline const TCHAR* const CheckpointStart = TEXT("checkpoint_start");
	inline const TCHAR* const TutorialStep = TEXT("tutorial_step");
	inline const TCHAR* const SettingsChange = TEXT("settings_change");
	inline const TCHAR* const MatchStart = TEXT("match_start");
	inline const TCHAR* const MatchComplete = TEXT("match_complete");

	/**
	 * What a Blueprint node's number pin holds when the game left it unset: the lowest integer, the pins' default. Blueprint
	 * has no optional numbers, and zero is a value the game sends. Each node's pins name it in their CPP_Default_ metadata.
	 */
	inline constexpr int32 BlueprintNumberNotSet = MIN_int32;

	/** One of a call's named arguments: its wire name, and its value when the game set it. */
	struct FNamedProperty
	{
		const TCHAR* Key;
		TOptional<FFlockCommandValue> Value;
	};

	/** A number argument: unset stays unset. */
	inline FNamedProperty Number(const TCHAR* Key, const TOptional<int32>& Value)
	{
		return { Key, Value.IsSet() ? TOptional<FFlockCommandValue>(FFlockCommandValue(Value.GetValue())) : TOptional<FFlockCommandValue>() };
	}

	/** A text argument: an empty one is not sent. */
	inline FNamedProperty Text(const TCHAR* Key, const FString& Value)
	{
		return { Key, Value.IsEmpty() ? TOptional<FFlockCommandValue>() : TOptional<FFlockCommandValue>(FFlockCommandValue(Value)) };
	}

	/** A Blueprint number pin: the lowest integer is the pin left unset; every other value, zero included, is sent. */
	inline TOptional<int32> FromBlueprintNumber(int32 Value)
	{
		return Value == BlueprintNumberNotSet ? TOptional<int32>() : TOptional<int32>(Value);
	}

	/**
	 * A copy of the game's extra properties with every named argument that is set written over it: a named argument wins
	 * over the same key in any letter case (this engine's JSON object cannot hold two spellings of one key), and a warning
	 * names a key of the game's that a different spelling replaced. The game's own properties are never changed.
	 */
	FFlockCommandData WithNamedProperties(const FFlockCommandData& ExtraProperties, TArrayView<const FNamedProperty> Named);

	// ── The Blueprint nodes' bodies: each turns its unset number pins into unset arguments and calls Sdk's typed call. ──
	// Kept apart from UFlockLibrary, which finds the SDK through a world, so a test can hand in its own SDK.

	bool TrackLevelStartFromBlueprint(UFlockSubsystem* Sdk, int32 Level, const FString& Difficulty,
		const FFlockCommandData& ExtraProperties);
	bool TrackLevelCompleteFromBlueprint(UFlockSubsystem* Sdk, int32 Level, int32 Score, int32 DurationSeconds, int32 Stars,
		const FFlockCommandData& ExtraProperties);
	bool TrackLevelFailFromBlueprint(UFlockSubsystem* Sdk, int32 Level, const FString& Reason, int32 DurationSeconds,
		const FFlockCommandData& ExtraProperties);
	bool TrackCheckpointFromBlueprint(UFlockSubsystem* Sdk, int32 Level, const FString& Checkpoint,
		const FFlockCommandData& ExtraProperties);
	bool TrackTutorialStepFromBlueprint(UFlockSubsystem* Sdk, int32 Step, const FString& StepName,
		const FFlockCommandData& ExtraProperties);
	bool TrackSettingsChangeFromBlueprint(UFlockSubsystem* Sdk, const FString& Setting, const FString& Value,
		const FFlockCommandData& ExtraProperties);
	bool TrackMatchStartFromBlueprint(UFlockSubsystem* Sdk, const FString& Mode, const FString& Map, int32 TeamSize,
		const FFlockCommandData& ExtraProperties);
	bool TrackMatchCompleteFromBlueprint(UFlockSubsystem* Sdk, const FString& Mode, const FString& Result, int32 Score,
		int32 DurationSeconds, int32 Kills, const FFlockCommandData& ExtraProperties);
}
