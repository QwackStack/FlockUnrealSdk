// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

/** Everything the auto-summon decision is allowed to depend on. */
struct FLOCKEDITOR_API FFlockSummonContext
{
	/** Any Error-severity finding is present. */
	bool bHasError = false;

	/**
	 * No last-seen SDK version recorded for this user on this project — the plugin is new to them.
	 * Written the first time the panel is shown, so this is true exactly once per developer per project.
	 */
	bool bFirstAdd = false;

	/** Unattended or commandlet. No UI may be raised at all. */
	bool bHeadless = false;

	/** The panel already summoned itself since the editor process started. */
	bool bAlreadySummonedThisSession = false;

	/** The developer chose "don't show again" for the current findings. */
	bool bSuppressedByUser = false;
};

/**
 * Whether the panel may open itself.
 *
 * Its own type rather than widget code because these are real rules with real edge cases, and inside a
 * Slate widget they would be untestable. The rules exist to keep the panel from becoming a nuisance —
 * a panel that has never appeared for no reason keeps the right to appear.
 */
class FLOCKEDITOR_API FFlockSummonPolicy
{
public:
	static bool ShouldSummon(const FFlockSummonContext& Context);
};
