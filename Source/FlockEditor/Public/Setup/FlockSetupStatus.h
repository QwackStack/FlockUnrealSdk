// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

/**
 * How badly a finding matters.
 *
 * Error is reserved for "the SDK cannot initialize" and nothing else. Auto-summon keys on Error, and
 * inflating it is exactly how a panel becomes something developers close reflexively.
 */
enum class EFlockSetupSeverity : uint8
{
	/** Worth knowing; no action required. */
	Info,
	/** Works, but something is stale. */
	Warning,
	/** The SDK cannot initialize until this is fixed. */
	Error,
};

/** The primary action offered on a finding's row — the one most likely to clear it. */
enum class EFlockSetupFix : uint8
{
	None,
	OpenSettings,
	Resolve,
	TestConnection,
	SyncSchemas,
};

/**
 * Outcome of an explicit connection probe.
 *
 * There is deliberately no "game not found" state. The Game Name is never sent to the server — the API
 * key identifies the game — so no request can reject it. A wrong name is detected by comparing the game
 * record the server returns against the configured one, which is a different finding at a different
 * severity. See FFlockSetupInput::ProbeServerGameName.
 */
enum class EFlockProbeState : uint8
{
	/** No probe has run this session — the panel says nothing about connectivity. */
	NotRun,
	Ok,
	/** No HTTP response at all: DNS, refused, offline, or a wrong host. */
	Unreachable,
	/** 401/403 — the API key was rejected. */
	KeyRejected,
	/** 404 on the by-name lookup — no version by that name on this game. */
	VersionNotFound,
	/** Completed, failed, and could not be attributed to one credential. Carries the server's wording. */
	Failed,
};

/** One thing that is wrong (or notable) about this project's Flock setup. */
struct FLOCKEDITOR_API FFlockSetupFinding
{
	/** Stable across releases — tests, suppression, and any future telemetry key on this. */
	FName Id;

	EFlockSetupSeverity Severity = EFlockSetupSeverity::Info;

	/** The problem, one line. */
	FText Title;

	/** What to do about it. */
	FText Detail;

	EFlockSetupFix Fix = EFlockSetupFix::None;
};

/**
 * Everything the evaluator is allowed to look at.
 *
 * A snapshot rather than a live read of the settings CDO, and that is the load-bearing decision here:
 * it makes the evaluator testable with no editor, no CDO, and no network. This logic is the one piece
 * that must never be wrong, so it has to be the easiest piece to test.
 */
struct FLOCKEDITOR_API FFlockSetupInput
{
	// ── Required settings ──
	FString ApiUrl;
	FString ApiKey;
	FString GameId;
	FString GameVersion;

	/** Baked by the resolve step; empty means unresolved. */
	FString GameVersionId;

	// ── Codegen ──
	/** False when codegen has never run here, or the generated folder was cleaned. */
	bool bCodegenManifestPresent = false;

	/** Version id the last sync generated for. Compared against GameVersionId — no network needed. */
	FString CodegenManifestVersionId;

	// ── Connection probe (explicit, never automatic) ──
	EFlockProbeState Probe = EFlockProbeState::NotRun;

	/** The server's own wording, surfaced when the failure could not be attributed to one credential. */
	FString ProbeMessage;

	/**
	 * Game name as the server reports it, filled only when a probe got far enough to read the game record.
	 * Compared against GameId to catch a settings page that names a different game than it talks to.
	 */
	FString ProbeServerGameName;

	// ── SDK version ──
	FString CurrentSdkVersion;

	/** Empty on a first add. Per-user state, so it never rides in DefaultGame.ini. */
	FString LastSeenSdkVersion;
};

/**
 * Turns project state into a list of findings. Pure: no editor, no CDO, no network, no logging.
 *
 * Every surface — the panel, the settings banner, the PIE guard, the packaging validator — renders what
 * this returns and reasons about nothing itself. That is the whole point: five surfaces that each decided
 * for themselves what "set up" meant would drift, and did.
 */
class FLOCKEDITOR_API FFlockSetupStatus
{
public:
	/** Findings for this state, ordered most severe first. Empty means the project is healthy. */
	static TArray<FFlockSetupFinding> Evaluate(const FFlockSetupInput& Input);

	/** True when any finding would stop the SDK initializing. */
	static bool HasErrors(const TArray<FFlockSetupFinding>& Findings);

	/** The subset at or above a severity — how the PIE guard and the build validator narrow the list. */
	static TArray<FFlockSetupFinding> AtLeast(const TArray<FFlockSetupFinding>& Findings, EFlockSetupSeverity Min);
};
