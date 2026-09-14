// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "FlockPlaytestSettings.generated.h"

/**
 * Flock Playtest settings for this project.
 *
 * Playtesting does nothing until Enable Playtesting is turned on and Protokite API URL is set. Values are
 * saved to the project's DefaultGame.ini and read at runtime.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Flock Playtest Settings"))
class FLOCKPLAYTEST_API UFlockPlaytestSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Groups this panel under Project Settings > Plugins, next to the Flock SDK's own. */
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

	/**
	 * Collect playtest data in this build.
	 *
	 * Off by default, and it has to stay that way: playtest data can include screen recordings and what
	 * players write in a feedback form, so a build only collects it when someone turned this on.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Playtesting", meta = (DisplayName = "Enable Playtesting"))
	bool bPlaytestingEnabled = false;

	/**
	 * Base URL of the Protokite API, for example http://localhost:8020 on a local stack.
	 * Must start with http:// or https:// and contain no spaces or line breaks.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Playtesting", meta = (DisplayName = "Protokite API URL"))
	FString ProtokiteApiUrl;
};
