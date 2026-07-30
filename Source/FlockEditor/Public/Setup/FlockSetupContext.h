// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Setup/FlockConnectionProbe.h"
#include "Setup/FlockSetupStatus.h"
#include "Setup/FlockSummonPolicy.h"

/**
 * The impure half of setup status: reads project settings, the codegen manifest, and per-user editor
 * state, and assembles the snapshot the pure evaluator consumes.
 *
 * Split from FFlockSetupStatus so the evaluator stays testable with no editor, no CDO, and no network.
 * Everything that touches the world lives here; everything that decides lives there.
 */
class FLOCKEDITOR_API FFlockSetupContext
{
public:
	/** Snapshot of the project right now, including whatever the last probe learned this session. */
	static FFlockSetupInput BuildInput();

	/** Convenience: BuildInput() straight through the evaluator. */
	static TArray<FFlockSetupFinding> Evaluate();

	// ── Probe state (session-scoped, never persisted) ──

	static const FFlockProbeResult& LastProbe();
	static void SetLastProbe(const FFlockProbeResult& Result);

	/** Drops the probe result, so the panel stops reporting connectivity it can no longer vouch for. */
	static void ClearProbe();

	// ── Auto-summon bookkeeping ──

	/**
	 * The summon context for right now, combining findings, per-user suppression, headless state, and
	 * whether the panel has already opened itself this session.
	 */
	static FFlockSummonContext BuildSummonContext(const TArray<FFlockSetupFinding>& Findings);

	/**
	 * Records that the panel opened itself. In-memory only: persisting it would mean a developer who
	 * restarts the editor to fix something never hears about it again.
	 */
	static void MarkSummoned();

	/** Test seam — resets the session-scoped state this class holds. */
	static void ResetSessionStateForTesting();
};
