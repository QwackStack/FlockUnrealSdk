// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Setup/FlockSetupStatus.h"

/**
 * Notification-first PIE setup guard. When the game would auto-initialize the SDK but can't, it warns
 * via the Message Log and an editor toast on entering Play — never silently entering a broken session,
 * but non-blocking by default. Registered by the FlockEditor module.
 *
 * It reasons about nothing itself: it renders the same findings the Flock panel and the settings banner
 * render, narrowed to what actually blocks. Previously it built its own reason string, which is how a
 * guard and a panel end up disagreeing about whether a project is set up.
 */
class FFlockPlayModeGuard
{
public:
	static void Register();
	static void Unregister();

	/**
	 * Pure decision: what should stop this play session, given the project's findings and the two
	 * opt-outs. Empty means enter Play silently.
	 *
	 * @param Findings    everything FFlockSetupStatus reports
	 * @param bGuardEnabled  UFlockConfig::bPlayModeGuardEnabled
	 * @param bAutoInit      UFlockConfig::bAutoInitializeOnLoad — when off the game drives init itself,
	 *                       and second-guessing it would be wrong
	 */
	static TArray<FFlockSetupFinding> BlockingFindings(const TArray<FFlockSetupFinding>& Findings,
		bool bGuardEnabled, bool bAutoInit);

private:
	static void OnBeginPIE(const bool bIsSimulating);

	static FDelegateHandle BeginPIEHandle;
};
