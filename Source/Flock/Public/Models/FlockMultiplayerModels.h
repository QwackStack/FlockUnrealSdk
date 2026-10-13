// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockMultiplayerModels.generated.h"

/** One row of the game's matchmaking queue list, as much of it as the SDK reads. */
USTRUCT()
struct FLOCK_API FFlockMatchmakingQueue
{
	GENERATED_BODY()

	UPROPERTY()
	FString Id;

	UPROPERTY()
	FString Name;
};
