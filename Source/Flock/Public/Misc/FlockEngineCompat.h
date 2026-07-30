// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "Misc/EngineVersionComparison.h"

/**
 * The single home for engine-version-conditional code.
 *
 * **The rule: `UE_VERSION_OLDER_THAN` and `ENGINE_MINOR_VERSION` appear nowhere else in this plugin.**
 * A version guard scattered across the codebase is a guard nobody can audit; here, the whole of the
 * SDK's engine-compatibility surface is one file you can read in a minute. The CI consistency job
 * enforces it.
 *
 * At the current floor nothing in the SDK is version-conditional, so this file holds only the floor and
 * the assert below. That is deliberate rather than premature: the floor has to live somewhere both the
 * code and the CI check can read, and the assert already does real work.
 *
 * Widening the floor
 * ------------------
 * The floor is what has actually been compiled, not what we hope works. Only UE 5.5 is installed on the
 * maintainer machine, so 5.5 is what this claims. To lower it: install the engine, run
 * `Tooling/Build-AllEngines.ps1`, fix what breaks *in this file*, then move the floor here, in
 * `Flock.uplugin`, and in README.md together - the CI consistency job fails if those three disagree.
 *
 * Known version-sensitive APIs the SDK already uses, for whoever does that work:
 *   - `EJsonObjectConversionFlags::SkipStandardizeCase`  (5.1+)
 *   - `IHttpRequest::SetResponseBodyReceiveStream`       (5.0+)
 *   - `FTSTicker`, `TObjectPtr`                          (5.0+)
 *
 * ASCII only in this file. Tooling on both sides reads it - a PowerShell script and a shell script in
 * CI - and a BOM-less round trip through Windows PowerShell turns non-ASCII into mojibake.
 */

#define FLOCK_ENGINE_FLOOR_MAJOR 5
#define FLOCK_ENGINE_FLOOR_MINOR 5

// A plugin compiled against an engine below the floor produces a cascade of unknown-symbol errors that
// name everything except the actual problem. This turns that into one line.
static_assert(!UE_VERSION_OLDER_THAN(FLOCK_ENGINE_FLOOR_MAJOR, FLOCK_ENGINE_FLOOR_MINOR, 0),
	"The Flock SDK requires Unreal Engine 5.5 or newer. Older engines are not a supported configuration - "
	"see Source/Flock/Public/Misc/FlockEngineCompat.h.");
