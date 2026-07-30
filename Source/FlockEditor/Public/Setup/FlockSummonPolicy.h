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
 * What the caller should do about the panel: open it, and/or record that this developer has now seen
 * this SDK version.
 *
 * Two outputs rather than one because recording "seen" is a *decision*, not a consequence of not
 * opening. Leaving it to the caller cost a real bug: a headless run raises no UI but was still writing
 * the seen-state, so every automation run consumed the developer's one-shot first-run welcome (and, after
 * a version bump, their upgrade notice) before they ever opened the editor.
 */
struct FLOCKEDITOR_API FFlockSummonDecision
{
	/** Bring the panel up and focus it. */
	bool bOpenPanel = false;

	/**
	 * Persist the current SDK version as seen by this developer.
	 *
	 * False whenever no human could have seen anything. Recording it then is a silent lie about what the
	 * developer has been shown, and it is unrecoverable — the notice is one-shot.
	 */
	bool bRecordSeen = false;
};

/**
 * Whether the panel may open itself, and whether this process may record that it did.
 *
 * Its own type rather than widget code because these are real rules with real edge cases, and inside a
 * Slate widget they would be untestable. The rules exist to keep the panel from becoming a nuisance —
 * a panel that has never appeared for no reason keeps the right to appear.
 */
class FLOCKEDITOR_API FFlockSummonPolicy
{
public:
	/** The full decision. Prefer this; it is the only place both halves are kept consistent. */
	static FFlockSummonDecision Decide(const FFlockSummonContext& Context);

	/** Convenience for the open-panel half alone. */
	static bool ShouldSummon(const FFlockSummonContext& Context);
};
