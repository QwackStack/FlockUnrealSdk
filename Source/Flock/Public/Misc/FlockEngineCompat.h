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
 * Almost all of the SDK is spanned by spellings that compile unchanged on every engine in the range,
 * and that is deliberate: where an engine API moved, the SDK was moved onto the portable API rather
 * than given a guard, because a guard is a thing somebody must re-audit every time the range changes.
 * **Two things have no portable spelling**, and both live at the bottom of this file:
 *
 *   - `UUserDefinedStruct`'s header. It moved from `Engine/` to `StructUtils/` in 5.5, and the old
 *     path is not a fallback: from 5.6 the `Engine/` header forwards only under
 *     `UE_ENABLE_INCLUDE_ORDER_DEPRECATED_IN_5_5`, which a module on the latest include order does
 *     not have. So 5.4 can use only one spelling and 5.6+ can use only the other.
 *   - `FAutomationTestBase::TestEqualSensitive`, which arrived in 5.5. Below that the SDK supplies its
 *     own, so the tests read the same on every engine. Test-only: nothing a game compiles depends on it.
 *
 * Everything else is still portable by construction, and a new guard belongs here rather than beside
 * the code that needs it - the CI job in .github/workflows/engine-claim.yml fails on one found
 * anywhere else.
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
 *   - `UUserDefinedStruct`'s header                      (Engine/ below 5.5, StructUtils/ from 5.5)
 *   - `FAutomationTestBase::TestEqualSensitive`          (5.5+; stood in for below that)
 *   - `EAutomationTestFlags`                             (a struct-scoped enum, then an enum class in
 *     5.5 - the spellings the tests use compile as both, and nothing stores or returns the type)
 *
 * The `FJsonObject::Values` entry is why the SDK reaches for `HasField` / `TryGetField` / `SetField`
 * wherever it can: those have stable signatures across the range, and the container behind them does
 * not. Ten places do loop over `Values` directly, and eight of them name the pair
 * `TPair<FString, TSharedPtr<FJsonValue>>`. That compiles on 5.4 through 5.8, where the key really is
 * an FString, and it is what would break first if a later engine interns the key - so whoever moves
 * the ceiling should spell those `auto` and read the key as `FString(*Pair.Key)`, the way
 * `FlockTestSpelling::HasMemberSpelled` and `FFlockJsonUtils::GetFieldNames` already do.
 *
 * ASCII only in this file. Tooling on both sides reads it - a PowerShell script and a shell script in
 * CI - and a BOM-less round trip through Windows PowerShell turns non-ASCII into mojibake.
 */

#define FLOCK_ENGINE_FLOOR_MAJOR 5
#define FLOCK_ENGINE_FLOOR_MINOR 4

#define FLOCK_ENGINE_CEILING_MAJOR 5
#define FLOCK_ENGINE_CEILING_MINOR 8

// A plugin compiled against an engine below the floor produces a cascade of unknown-symbol errors that
// name everything except the actual problem. This turns that into one line.
static_assert(!UE_VERSION_OLDER_THAN(FLOCK_ENGINE_FLOOR_MAJOR, FLOCK_ENGINE_FLOOR_MINOR, 0),
	"The Flock SDK requires Unreal Engine 5.4 or newer. Older engines are not a supported configuration - "
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

// UUserDefinedStruct moved from Engine/ to StructUtils/ in UE 5.5, and NEITHER spelling is portable across the
// range: 5.4 has only the Engine/ one, and from 5.6 the Engine/ header forwards to StructUtils/ only under
// UE_ENABLE_INCLUDE_ORDER_DEPRECATED_IN_5_5, which a module on the latest include order does not have. So the path
// is named here and the codegen files include this macro rather than either spelling.
#if UE_VERSION_OLDER_THAN(5, 5, 0)
	#define FLOCK_USER_DEFINED_STRUCT_HEADER "Engine/UserDefinedStruct.h"
#else
	#define FLOCK_USER_DEFINED_STRUCT_HEADER "StructUtils/UserDefinedStruct.h"
#endif

// FAutomationTestBase::TestEqualSensitive - the string check that reads letter case - arrived in UE 5.5. Below that
// the SDK supplies its own, so the same test source reads the same on every engine in the range.
#define FLOCK_ENGINE_HAS_TEST_EQUAL_SENSITIVE (!UE_VERSION_OLDER_THAN(5, 5, 0))

#if WITH_AUTOMATION_TESTS && !FLOCK_ENGINE_HAS_TEST_EQUAL_SENSITIVE

#include "Misc/AutomationTest.h"

namespace FlockEngineCompat
{
	/**
	 * What TestEqualSensitive does on the engines that have it: fails the check unless the two strings are the same
	 * letter for letter. It lives here, rather than beside the other spelling checks in the Flock module's private
	 * test support, because FlockEditor and the playtest plugin call it too and neither can reach that folder.
	 */
	inline bool CheckEqualSpelledExactly(FAutomationTestBase& Test, const FString& What, const FString& Actual,
		const FString& Expected)
	{
		if (Actual.Equals(Expected, ESearchCase::CaseSensitive))
		{
			return true;
		}
		Test.AddError(FString::Printf(TEXT("Expected '%s' to be '%s', but it was '%s'."), *What, *Expected, *Actual), 1);
		return false;
	}
}

// A macro, because the call sites name it the way the engine's own is named: unqualified, inside a test's RunTest,
// where `this` is the test. On UE 5.5 and newer this is not defined at all and the engine's member is what runs.
#define TestEqualSensitive(What, Actual, Expected) \
	FlockEngineCompat::CheckEqualSpelledExactly(*this, What, Actual, Expected)

#endif // WITH_AUTOMATION_TESTS && !FLOCK_ENGINE_HAS_TEST_EQUAL_SENSITIVE
