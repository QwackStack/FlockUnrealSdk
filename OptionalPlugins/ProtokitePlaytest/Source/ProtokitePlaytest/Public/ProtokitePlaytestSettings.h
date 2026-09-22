// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "Engine/DeveloperSettings.h"
#include "ProtokitePlaytestSettings.generated.h"

/**
 * Protokite Playtest settings for this project.
 *
 * Playtesting does nothing until Enable Playtesting is turned on and Protokite API URL is set. Values are
 * saved to the project's DefaultGame.ini and read at runtime.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Protokite Playtest Settings"))
class PROTOKITEPLAYTEST_API UProtokitePlaytestSettings : public UDeveloperSettings
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
	 * Ask the player what this playtest may collect, and collect nothing until they answer.
	 *
	 * On by default. The question is the playtest's own and says so: it is asked once this build's playtest is loaded,
	 * in wording that keeps it apart from any privacy or analytics choice the game asks about, and the answer -- the
	 * screen and play data, one of the two, or nothing -- is kept on the player's machine and used by every later
	 * launch. Choosing nothing leaves the build behaving exactly as one with Enable Playtesting off.
	 *
	 * Turn it off only where the players have already been asked another way, for an internal test the team is running
	 * on its own machines, or for an automated run with nobody there to answer; the build then collects everything the
	 * playtest turns on, and says so in what each session sends.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Player Consent", meta = (DisplayName = "Ask The Player For Playtest Consent"))
	bool bAskThePlayerForPlaytestConsent = true;

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
	 * How many megabytes the recordings in Saved/ProtokitePlaytest/Recordings may take together. A playtest recording that has
	 * not been uploaded is kept there, and so is a test video. Before a recording starts, the oldest recordings of games
	 * that are no longer running are deleted until it fits. When even that leaves less room than Recording Size Limit, the
	 * recording is cut shorter; with less than 1 MB left, none starts.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Video Recording", meta = (DisplayName = "Recordings Disk Budget (MB)", ClampMin = "1"))
	int32 RecordingsDiskBudgetMb = 4096;

	/**
	 * The key that opens the playtest's feedback form. Set it to none to leave opening the form entirely to the game,
	 * which calls Open Feedback Form itself -- from a pause menu, say.
	 *
	 * The key does nothing when the playtest has published no form: there would be nothing to show.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Feedback Form", meta = (DisplayName = "Feedback Form Key"))
	FKey FeedbackFormKey = EKeys::F9;

	/**
	 * Whether the game pauses while the form is open.
	 *
	 * Off by default, because a paused game is the wrong thing for a playtest built around what a player was doing:
	 * pausing a multiplayer match does nothing, and pausing a single-player one loses whatever the player was about to
	 * describe. A game that wants it can turn it on.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Feedback Form", meta = (DisplayName = "Pause The Game While The Form Is Open"))
	bool bPauseWhileFeedbackFormIsOpen = false;

};
