// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Models/FlockCommandModels.h"
#include "FlockPlaytestLibrary.generated.h"

/** Blueprint nodes for the playtest plugin. Each finds the playtest subsystem from the calling graph, and does nothing without one. */
UCLASS()
class FLOCKPLAYTEST_API UFlockPlaytestLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Records one of the game's own events for this playtest, under the playtest category. Build Properties with the Set
	 * Command nodes. Recorded only while playtesting is ready and the playtest turns heavy analytics on. Returns false,
	 * and records nothing, otherwise; for a name the plugin sends itself (performance_window, level_loaded); and when the
	 * Flock SDK refuses the event (its analytics off, consent withheld, an empty name, or a name over 200 characters).
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject",
		AutoCreateRefTerm = "Properties", DisplayName = "Flock Record Playtest Event"))
	static bool RecordPlaytestEvent(const UObject* WorldContextObject, const FString& EventName, const FFlockCommandData& Properties);

	/**
	 * Stops this launch's video recording for good and saves the file, for a game that quits on its own schedule. No
	 * other recording starts this launch. Returns false when no recording is capturing.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Stop Video Recording"))
	static bool StopVideoRecording(const UObject* WorldContextObject);

	/** True while a video recording is capturing the screen. */
	UFUNCTION(BlueprintPure, Category = "Flock|Playtest", meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Is Recording Video"))
	static bool IsRecordingVideo(const UObject* WorldContextObject);
};
