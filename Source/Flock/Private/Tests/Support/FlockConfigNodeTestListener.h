// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockError.h"
#include "Models/FlockConfigModels.h"
#include "Models/FlockGameModels.h"
#include "FlockConfigNodeTestListener.generated.h"

/**
 * Bind target for the config/game async-node pin tests (dynamic delegates need UFUNCTION handlers on a
 * UObject). Not guarded by WITH_AUTOMATION_TESTS on purpose — UHT generates its registration unconditionally.
 */
UCLASS()
class UFlockConfigNodeTestListener : public UObject
{
	GENERATED_BODY()

public:
	int32 ConfigPinCount = 0;
	int32 ConfigListPinCount = 0;
	int32 ConfigDataPinCount = 0;
	int32 GamePinCount = 0;
	int32 GameVersionPinCount = 0;
	FFlockError LastError;

	UFUNCTION()
	void HandleConfigPin(const FFlockGameConfigSchema& Config, const FFlockError& Error) { ++ConfigPinCount; LastError = Error; }

	UFUNCTION()
	void HandleConfigListPin(const TArray<FFlockGameConfigSchema>& Configs, const FFlockError& Error) { ++ConfigListPinCount; LastError = Error; }

	UFUNCTION()
	void HandleConfigDataPin(const FFlockGameConfigData& Data, const FFlockError& Error) { ++ConfigDataPinCount; LastError = Error; }

	UFUNCTION()
	void HandleGamePin(const FFlockGameSchema& Game, const FFlockError& Error) { ++GamePinCount; LastError = Error; }

	UFUNCTION()
	void HandleGameVersionPin(const FFlockGameVersionSchema& Version, const FFlockError& Error) { ++GameVersionPinCount; LastError = Error; }
};
