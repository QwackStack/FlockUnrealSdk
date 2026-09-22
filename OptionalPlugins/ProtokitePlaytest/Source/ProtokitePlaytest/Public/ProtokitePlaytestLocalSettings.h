// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "ProtokitePlaytestLocalSettings.generated.h"

/**
 * Protokite Playtest settings that belong to one person on one computer, for trying the plugin out in the editor.
 *
 * Saved under the project's Saved folder rather than DefaultGame.ini, so they are never committed and never reach a
 * build someone else plays.
 */
UCLASS(Config = EditorPerProjectUserSettings, meta = (DisplayName = "Protokite Playtest Local Settings"))
class PROTOKITEPLAYTEST_API UProtokitePlaytestLocalSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

#if WITH_EDITORONLY_DATA
	/**
	 * Record a video every time you play in the editor, with no playtest needed. The file is saved under
	 * Saved/ProtokitePlaytest/Recordings/TestVideos, never uploaded, and its path is logged when it is saved. It uses the Video
	 * Recording settings in Protokite Playtest Settings, and takes effect the next time you press Play.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Video Recording", meta = (DisplayName = "Record Video In Play In Editor"))
	bool bRecordVideoInPlayInEditor = false;
#endif
};
