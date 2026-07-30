// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Setup/FlockSummonPolicy.h"

bool FFlockSummonPolicy::ShouldSummon(const FFlockSummonContext& Context)
{
	// Headless outranks everything: a commandlet or an unattended build must never raise UI, whatever
	// state the project is in.
	if (Context.bHeadless)
	{
		return false;
	}

	// At most once per editor process. The counter is in-memory on purpose — persisting it would mean a
	// developer who restarts the editor to fix something never hears about it again.
	if (Context.bAlreadySummonedThisSession)
	{
		return false;
	}

	// A first add is the one case that summons without an error: nothing is wrong, but the developer has
	// never seen the SDK and deserves one introduction. Not suppressible, because there is no finding to
	// have suppressed yet.
	if (Context.bFirstAdd)
	{
		return true;
	}

	// Otherwise the panel only interrupts for something that stops the SDK working.
	if (!Context.bHasError)
	{
		return false;
	}

	// Suppression mutes the interruption, never the information — the panel and the settings banner still
	// report the finding to anyone who looks.
	return !Context.bSuppressedByUser;
}
