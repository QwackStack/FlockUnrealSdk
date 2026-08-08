// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "Misc/EngineVersionComparison.h"
#include "HAL/PreprocessorHelpers.h" // PREPROCESSOR_TO_STRING, so the warning cannot drift from the macros

/**
 * The single home for the SDK's engine-version claim, and for any engine-conditional code.
 *
 * **The rule: `UE_VERSION_OLDER_THAN`, `UE_VERSION_NEWER_THAN` and `ENGINE_*_VERSION` appear nowhere
 * else in this plugin.** A version guard scattered across the codebase is a guard nobody can audit;
 * here, the whole of the SDK's engine-compatibility surface is one file you can read in a minute. The
 * CI consistency job enforces it.
 *
 * As it stands **nothing in the SDK is version-conditional** - the supported range is spanned entirely
 * by spellings that compile unchanged on every engine in it. That is a deliberate outcome, not an
 * accident: where an engine API moved, the SDK was moved onto the portable API rather than given a
 * guard, because a guard is a thing somebody must re-audit every time the range changes. So read this
 * file as *the claim*, not as a pile of conditionals. If a guard ever has to be added, it is added here.
 *
 * The supported range
 * -------------------
 * The range is what has actually been built and tested, not what is hoped to work. Both ends are real:
 *
 *   - Below the floor is a hard error. An unsupported engine otherwise produces a cascade of
 *     unknown-symbol errors that name everything except the actual problem.
 *   - Above the ceiling is a *warning*, and the SDK still compiles. A newer engine usually breaks
 *     nothing, and refusing to build would break every consumer the day a new engine ships even when
 *     nothing actually changed. If something did break, the compiler says so on its own. The warning
 *     exists to prompt a maintainer to run the sweep, not to stop anyone working.
 *
 * Moving either end
 * -----------------
 * Install the engine, run `Tooling/Build-AllEngines.ps1` (which builds *and* runs the test suite on
 * every installed engine, and reports what it could not cover), fix what breaks, then move the end here,
 * in `Flock.uplugin`, and in README.md together - the CI consistency job fails if those disagree.
 *
 * Known version-sensitive APIs the SDK already uses, for whoever does that work:
 *   - `EJsonObjectConversionFlags::SkipStandardizeCase`  (5.1+, so 5.0 can never be supported)
 *   - `IHttpRequest::SetResponseBodyReceiveStream`       (5.0+)
 *   - `FTSTicker`, `TObjectPtr`                          (5.0+)
 *   - `FJsonObject::Values` key type                     (FString, then an interned shared string)
 *
 * That last one is the reason this SDK never touches `FJsonObject::Values` directly except in
 * `FFlockJsonUtils::GetFieldNames`. `HasField` / `TryGetField` / `SetField` have stable signatures
 * across the range; the container behind them does not.
 *
 * ASCII only in this file. Tooling on both sides reads it - a PowerShell script and a shell script in
 * CI - and a BOM-less round trip through Windows PowerShell turns non-ASCII into mojibake.
 */

#define FLOCK_ENGINE_FLOOR_MAJOR 5
#define FLOCK_ENGINE_FLOOR_MINOR 5

#define FLOCK_ENGINE_CEILING_MAJOR 5
#define FLOCK_ENGINE_CEILING_MINOR 8

// A plugin compiled against an engine below the floor produces a cascade of unknown-symbol errors that
// name everything except the actual problem. This turns that into one line.
static_assert(!UE_VERSION_OLDER_THAN(FLOCK_ENGINE_FLOOR_MAJOR, FLOCK_ENGINE_FLOOR_MINOR, 0),
	"The Flock SDK requires Unreal Engine 5.5 or newer. Older engines are not a supported configuration - "
	"see Source/Flock/Public/Misc/FlockEngineCompat.h.");

// Deliberately a warning rather than an error - see "The supported range" above. Anyone hitting this is
// on an engine newer than anything the SDK has been run against; the SDK will very likely work, and the
// resolution is to run the sweep and move the ceiling, not to patch around this line.
//
// Spelled as "not older than the next minor" rather than NEWER_THAN(ceiling): NEWER_THAN compares the
// patch too, so NEWER_THAN(5, 8, 0) would fire on 5.8.1 - a patch release of an engine that IS in range.
// The +1 lands the comparison on the first minor outside the range, which is what the ceiling means.
#if !UE_VERSION_OLDER_THAN(FLOCK_ENGINE_CEILING_MAJOR, FLOCK_ENGINE_CEILING_MINOR + 1, 0)
	#pragma message("Flock SDK: this engine is newer than UE " \
		PREPROCESSOR_TO_STRING(FLOCK_ENGINE_CEILING_MAJOR) "." PREPROCESSOR_TO_STRING(FLOCK_ENGINE_CEILING_MINOR) \
		", the newest version the SDK has been built and tested against. It should work; it is simply " \
		"unverified. Run Tooling/Build-AllEngines.ps1 to confirm, then move the ceiling here, in " \
		"Flock.uplugin and in README.md together.")
#endif
