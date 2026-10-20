// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Version/FlockVersionResolver.h"
#include "Version/FlockVersionLookup.h"
#include "Version/FlockHttpVersionLookup.h"
#include "Config/FlockConfig.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockByNameUrlTest, "Flock.Editor.Resolver.ByNameUrl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockByNameUrlTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Trailing slash trimmed, version segment added, name percent-encoded"),
		FFlockVersionResolver::ByNameUrl(TEXT("https://api-flock.qwacks.com/"), TEXT("My Version")),
		FString(TEXT("https://api-flock.qwacks.com/v1/game_version/by-name/My%20Version")));

	TestEqual(TEXT("No trailing slash, plain name preserved"),
		FFlockVersionResolver::ByNameUrl(TEXT("https://api-flock.qwacks.com"), TEXT("1.0.0")),
		FString(TEXT("https://api-flock.qwacks.com/v1/game_version/by-name/1.0.0")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockApplyResultTest, "Flock.Editor.Resolver.ApplyResult",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockApplyResultTest::RunTest(const FString& Parameters)
{
	UFlockConfig* Config = NewObject<UFlockConfig>(GetTransientPackage());
	Config->GameVersionId = TEXT("old");

	// A failed result leaves the prior ID untouched.
	TestFalse(TEXT("Failed result reports no change"),
		FFlockVersionResolver::ApplyResult(*Config, FFlockResolveResult::Fail(TEXT("boom"), TEXT("fix"))));
	TestEqual(TEXT("Prior ID preserved on failure"), Config->GameVersionId, FString(TEXT("old")));

	// A successful result with a new ID changes it.
	TestTrue(TEXT("New ID reports a change"),
		FFlockVersionResolver::ApplyResult(*Config, FFlockResolveResult::Ok(TEXT("new"))));
	TestEqual(TEXT("ID updated on success"), Config->GameVersionId, FString(TEXT("new")));

	// The same ID is a no-op.
	TestFalse(TEXT("Unchanged ID reports no change"),
		FFlockVersionResolver::ApplyResult(*Config, FFlockResolveResult::Ok(TEXT("new"))));

	// An empty success ID is treated as no change.
	TestFalse(TEXT("Empty success ID reports no change"),
		FFlockVersionResolver::ApplyResult(*Config, FFlockResolveResult::Ok(TEXT(""))));

	return true;
}

namespace
{
	/** Answers only when the test says, the way a real lookup answers some time after it is asked. */
	class FHeldVersionLookup final : public IFlockVersionLookup
	{
	public:
		FFlockResolveComplete Waiting;
		virtual void Resolve(const FString&, const FString&, const FString&, FFlockResolveComplete OnComplete) override { Waiting = OnComplete; }
		virtual bool CanResolve() const override { return true; }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockResolveAndBakeFinishedTest, "Flock.Editor.Resolver.TellsItsCallerOnceTheLookupHasAnswered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockResolveAndBakeFinishedTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FHeldVersionLookup> Lookup = MakeShared<FHeldVersionLookup>();
	FFlockVersionLookupRegistry::Set(Lookup);
	ON_SCOPE_EXIT
	{
		FFlockVersionLookupRegistry::Set(MakeShared<FFlockHttpVersionLookup>());
	};

	// It already holds the ID the lookup answers, so the bake changes nothing and writes no settings file.
	UFlockConfig* Config = NewObject<UFlockConfig>(GetTransientPackage());
	Config->GameVersionId = TEXT("ver-abc");
	int32 Finished = 0;
	FFlockVersionResolver::ResolveAndBake(*Config, [&Finished]() { ++Finished; });
	if (!TestTrue(TEXT("Precondition: the lookup was asked"), Lookup->Waiting.IsBound()))
	{
		return false;
	}
	TestEqual(TEXT("Nothing is told before the lookup answers"), Finished, 0);
	Lookup->Waiting.Execute(FFlockResolveResult::Ok(TEXT("ver-abc")));
	TestEqual(TEXT("Told once it has"), Finished, 1);

	AddExpectedMessagePlain(TEXT("Flock resolve failed"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FFlockVersionResolver::ResolveAndBake(*Config, [&Finished]() { ++Finished; });
	Lookup->Waiting.Execute(FFlockResolveResult::Fail(TEXT("unreachable"), TEXT("fix")));
	TestEqual(TEXT("A failed lookup is an answer too"), Finished, 2);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
