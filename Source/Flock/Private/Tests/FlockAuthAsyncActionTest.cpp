// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Blueprint/FlockAuthAsyncActions.h"
#include "Tests/Support/FlockEventTestListener.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAuthNodesGuardTest, "Flock.Auth.Nodes.Guard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAuthNodesGuardTest::RunTest(const FString& Parameters)
{
	// A world context with no game instance behind it: the SDK can't resolve, so every node must
	// fire its failure pin exactly once with a Validation error. (Success paths are thin adapters
	// over the fully-tested provider and need a live world; they're exercised in PIE.)
	// Any concrete UObject with no world behind it works as the unusable context.
	UObject* BadContext = NewObject<UFlockAuthNodeTestListener>(GetTransientPackage());
	UFlockAuthNodeTestListener* Listener = NewObject<UFlockAuthNodeTestListener>();

	// Login node.
	{
		UFlockLoginAction* Action = UFlockLoginAction::LoginWithEmail(BadContext, TEXT("a@b.c"), TEXT("pw"));
		Action->OnFailure.AddDynamic(Listener, &UFlockAuthNodeTestListener::HandleLoginPin);
		Action->Activate();
		TestEqual(TEXT("login failure fired once"), Listener->LoginPinCount, 1);
		TestEqual(TEXT("validation error"), static_cast<int32>(Listener->LastError.Type), static_cast<int32>(EFlockErrorType::Validation));
	}
	// Register node.
	{
		UFlockRegisterAction* Action = UFlockRegisterAction::RegisterWithDevice(BadContext, TEXT("dev-1"), TEXT(""));
		Action->OnFailure.AddDynamic(Listener, &UFlockAuthNodeTestListener::HandleRegisterPin);
		Action->Activate();
		TestEqual(TEXT("register failure fired once"), Listener->RegisterPinCount, 1);
	}
	// Restore node routes to OnNoSession.
	{
		UFlockRestoreSessionAction* Action = UFlockRestoreSessionAction::RestoreSession(BadContext);
		Action->OnNoSession.AddDynamic(Listener, &UFlockAuthNodeTestListener::HandleRestorePin);
		Action->Activate();
		TestEqual(TEXT("no-session fired once"), Listener->RestorePinCount, 1);
	}
	// Account node.
	{
		UFlockAuthAccountAction* Action = UFlockAuthAccountAction::ForgotPassword(BadContext, TEXT("a@b.c"));
		Action->OnFailure.AddDynamic(Listener, &UFlockAuthNodeTestListener::HandleAccountPin);
		Action->Activate();
		TestEqual(TEXT("account failure fired once"), Listener->AccountPinCount, 1);
	}
	// Name node.
	{
		UFlockNameAvailableAction* Action = UFlockNameAvailableAction::IsNameAvailable(BadContext, TEXT("Duck"));
		Action->OnFailure.AddDynamic(Listener, &UFlockAuthNodeTestListener::HandleNamePin);
		Action->Activate();
		TestEqual(TEXT("name failure fired once"), Listener->NamePinCount, 1);
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
