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

	/**
	 * The widest the recorded video is, in pixels. The video keeps the shape of the game's screen and fits inside
	 * Video Width by Video Height; a smaller window is recorded at its own size.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Video Recording", meta = (DisplayName = "Video Width", ClampMin = "16", ClampMax = "3840"))
	int32 VideoWidth = 1280;

	/** The tallest the recorded video is, in pixels. See Video Width. */
	UPROPERTY(Config, EditAnywhere, Category = "Video Recording", meta = (DisplayName = "Video Height", ClampMin = "16", ClampMax = "2160"))
	int32 VideoHeight = 720;

	/** How many frames each second of video holds. A game running slower than this records every frame it draws. */
	UPROPERTY(Config, EditAnywhere, Category = "Video Recording", meta = (DisplayName = "Video Frames Per Second", ClampMin = "1", ClampMax = "60"))
	int32 VideoFramesPerSecond = 30;

	/** How many kilobits each second of video aims for. 2000 is about 0.9 GB an hour. */
	UPROPERTY(Config, EditAnywhere, Category = "Video Recording", meta = (DisplayName = "Video Bitrate (kbps)", ClampMin = "100", ClampMax = "50000"))
	int32 VideoBitrateKbps = 2000;

	/** A recording stops for good once it holds this many minutes of play. The playtest carries on without video. */
	UPROPERTY(Config, EditAnywhere, Category = "Video Recording", meta = (DisplayName = "Recording Length Limit (Minutes)", ClampMin = "0.1"))
	float MaxRecordingMinutes = 60.f;

	/**
	 * A recording stops for good before its file would grow past this many megabytes. The playtest carries on without
	 * video. Whichever limit is reached first ends the recording.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Video Recording", meta = (DisplayName = "Recording Size Limit (MB)", ClampMin = "1"))
	int32 MaxRecordingSizeMb = 1536;

	/**
	 * How many megabytes the recordings in Saved/FlockPlaytest/Recordings may take together. A playtest recording that has
	 * not been uploaded is kept there, and so is a test video. Before a recording starts, the oldest recordings of games
	 * that are no longer running are deleted until it fits. When even that leaves less room than Recording Size Limit, the
	 * recording is cut shorter; with less than 1 MB left, none starts.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Video Recording", meta = (DisplayName = "Recordings Disk Budget (MB)", ClampMin = "1"))
	int32 RecordingsDiskBudgetMb = 4096;
};
