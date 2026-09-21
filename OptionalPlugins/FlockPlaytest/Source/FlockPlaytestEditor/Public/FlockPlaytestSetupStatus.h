// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

/** How much a setup finding matters to a playtest. */
enum class EFlockPlaytestSetupSeverity : uint8
{
	/** Worth knowing: the playtest waits on something the game has to do. */
	Info,
	/** The playtest runs, but not the way it should, or not against the playtest meant. */
	Warning,
	/** No playtest session can start until this is fixed. */
	StopsPlaytesting,
};

/** Where a finding is fixed. */
enum class EFlockPlaytestSetupFix : uint8
{
	OpenPlaytestSettings,
	OpenFlockSettings,
};

/** One thing in this project's settings that affects a playtest. */
struct FLOCKPLAYTESTEDITOR_API FFlockPlaytestSetupFinding
{
	/** Stable, so tests and anything that filters findings can name one. */
	FName Id;
	EFlockPlaytestSetupSeverity Severity = EFlockPlaytestSetupSeverity::Info;
	FText Title;
	FText Detail;
	EFlockPlaytestSetupFix Fix = EFlockPlaytestSetupFix::OpenPlaytestSettings;
};

/** The settings a playtest depends on, read once so the findings can be decided without the editor. */
struct FLOCKPLAYTESTEDITOR_API FFlockPlaytestSetupInput
{
	bool bPlaytestingEnabled = false;
	FString ProtokiteApiUrl;

	/** Whether this build puts the playtest's own consent question to its players before collecting anything. */
	bool bAskThePlayerForPlaytestConsent = true;

	/** The Flock SDK's Game Version: the version's name, which the Flock SDK resolves to the ID it sends. */
	FString FlockGameVersion;
	bool bFlockAnalyticsEnabled = true;
	bool bFlockAnalyticsAutoStartSession = true;
	bool bFlockAnalyticsRequireExplicitConsent = false;

	/** This project's saved settings, from the playtest plugin's and the Flock SDK's settings pages. */
	static FFlockPlaytestSetupInput FromProjectSettings();
};

/**
 * Decides what in a project's settings would stop or change a playtest, before anyone presses Play. Pure: no editor,
 * no network. A project with Enable Playtesting off has nothing to hear, because off is the chosen state of every build
 * that is not a playtest build.
 */
class FLOCKPLAYTESTEDITOR_API FFlockPlaytestSetupStatus
{
public:
	/** How Protokite names a playtest's Flock game version: this, then the test's id. */
	static constexpr const TCHAR* PlaytestVersionPrefix = TEXT("pt-");

	/** Findings for these settings, most serious first. Empty means nothing stands in a playtest's way. */
	static TArray<FFlockPlaytestSetupFinding> Evaluate(const FFlockPlaytestSetupInput& Input);
};
