// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

class SDockTab;
class SFlockPanel;
class FSpawnTabArgs;

/**
 * Owns the editor-facing half of setup status: the Flock tab, the settings-page banner, and the rule
 * about when the tab is allowed to open itself.
 *
 * The tab is a nomad tab rather than a modal window on purpose. A modal enforces attention; the summon
 * policy earns it, because the panel only ever appears when something is actually actionable.
 */
class FFlockSetupUI
{
public:
	static void Register();
	static void Unregister();

	/** Brings the Flock tab up and focuses it. The "Open Flock" path from a toast or a menu. */
	static void OpenPanel();

	/** Rebuilds the open panel's findings, if one is open. No-op otherwise. */
	static void RefreshPanel();

	/**
	 * Opens the panel if — and only if — FFlockSummonPolicy says so, then records that it did.
	 * Called once on editor init.
	 */
	static void SummonIfNeeded();

private:
	static TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs& Args);

	static const FName TabName;
	static TWeakPtr<SFlockPanel> LivePanel;
};
