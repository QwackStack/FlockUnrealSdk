// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Version/FlockVersionLookup.h"
#include "Version/FlockHttpVersionLookup.h"

namespace
{
	/** A lookup that resolves successfully, standing in for the real HTTP lookup. */
	class FFakeResolvableLookup : public IFlockVersionLookup
	{
	public:
		virtual void Resolve(const FString& ApiUrl, const FString& ApiKey, const FString& GameVersion, FFlockResolveComplete OnComplete) override
		{
			OnComplete.ExecuteIfBound(FFlockResolveResult::Ok(TEXT("fake-id")));
		}

		virtual bool CanResolve() const override { return true; }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockStubLookupTest, "Flock.Editor.Lookup.Stub",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockStubLookupTest::RunTest(const FString& Parameters)
{
	FFlockStubVersionLookup Stub;
	TestFalse(TEXT("Stub cannot resolve"), Stub.CanResolve());

	bool bCalled = false;
	FFlockResolveResult Captured;
	Stub.Resolve(TEXT("url"), TEXT("key"), TEXT("1.0.0"),
		FFlockResolveComplete::CreateLambda([&bCalled, &Captured](const FFlockResolveResult& Result)
		{
			bCalled = true;
			Captured = Result;
		}));

	TestTrue(TEXT("Stub completes its callback"), bCalled);
	TestFalse(TEXT("Stub reports failure"), Captured.bSuccess);
	TestFalse(TEXT("Stub provides a reason"), Captured.Error.IsEmpty());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLookupRegistryTest, "Flock.Editor.Lookup.Registry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLookupRegistryTest::RunTest(const FString& Parameters)
{
	// The FlockEditor module registers the HTTP lookup at startup, so the live registry can resolve.
	TestTrue(TEXT("Module-registered lookup is live"), FFlockVersionLookupRegistry::Get().CanResolve());

	// Reset falls back to the stub, which keeps the build guard inert.
	FFlockVersionLookupRegistry::Reset();
	TestFalse(TEXT("Reset restores the stub"), FFlockVersionLookupRegistry::Get().CanResolve());

	// Registering a lookup makes it active.
	FFlockVersionLookupRegistry::Set(MakeShared<FFakeResolvableLookup>());
	TestTrue(TEXT("Registered lookup becomes active"), FFlockVersionLookupRegistry::Get().CanResolve());

	// Put the module's real lookup back so this test doesn't disarm resolve/build-guard for the session.
	FFlockVersionLookupRegistry::Set(MakeShared<FFlockHttpVersionLookup>());
	TestTrue(TEXT("Real lookup restored"), FFlockVersionLookupRegistry::Get().CanResolve());

	return true;
}

#endif // WITH_AUTOMATION_TESTS
