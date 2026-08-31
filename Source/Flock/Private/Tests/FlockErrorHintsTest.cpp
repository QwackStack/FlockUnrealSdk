// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockEventModels.h"
#include "Http/FlockErrorCode.h"
#include "Http/FlockErrorHints.h"
#include "UObject/Class.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockErrorHintsCoverageTest, "Flock.Http.ErrorHints.EveryCodeIsAnswered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockErrorHintsCoverageTest::RunTest(const FString& Parameters)
{
	const UEnum* Codes = StaticEnum<EFlockErrorCode>();
	if (!TestNotNull(TEXT("EFlockErrorCode is reflected"), Codes))
	{
		return false;
	}

	// Walked by reflection, not by a hand-written list: a code added later is covered the day it lands
	// rather than the day someone remembers this test exists. NumEnums() counts the implicit _MAX.
	const int32 Count = Codes->NumEnums() - 1;

	// A tripwire on the hand-maintained half. The enum mirrors the backend's detail.code set, so growing
	// it is a decision — this line makes that decision pass through here instead of past it.
	TestEqual(TEXT("EFlockErrorCode declares 71 members"), Count, 71);

	int32 Waived = 0;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const EFlockErrorCode Code = static_cast<EFlockErrorCode>(Codes->GetValueByIndex(Index));
		const FString Name = Codes->GetNameStringByIndex(Index);
		const FString Hint = FFlockErrorHints::For(Code);

		if (FFlockErrorHints::IsWaived(Code))
		{
			++Waived;
			TestTrue(FString::Printf(TEXT("%s is waived, so it carries no hint"), *Name), Hint.IsEmpty());
			continue;
		}

		if (!TestFalse(FString::Printf(TEXT("%s has a hint (add one, or waive it in IsWaived)"), *Name), Hint.IsEmpty()))
		{
			continue;
		}
		// A hint that does not say what to do next is the gap this test exists to close, wearing a
		// disguise. Every one names a call, a menu path or a dashboard step.
		TestTrue(FString::Printf(TEXT("%s hint says something actionable"), *Name), Hint.Len() > 20);
		TestTrue(FString::Printf(TEXT("%s hint is a sentence"), *Name), Hint.EndsWith(TEXT(".")));
	}

	// Only Unknown. A growing waiver list is the failure mode this whole test is aimed at.
	TestEqual(TEXT("exactly one waived code"), Waived, 1);
	TestTrue(TEXT("Unknown is the waived one"), FFlockErrorHints::IsWaived(EFlockErrorCode::Unknown));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockErrorHintsAuthContextTest, "Flock.Http.ErrorHints.AuthContextRefinesTheAmbiguousCode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockErrorHintsAuthContextTest::RunTest(const FString& Parameters)
{
	const EFlockErrorCode Ambiguous = EFlockErrorCode::PlayerInvalidLoginCredentials;
	const FString ContextFree = FFlockErrorHints::For(Ambiguous);

	const FString Device = FFlockErrorHints::ForAuth(Ambiguous, EFlockAuthMethod::Device);
	const FString Email = FFlockErrorHints::ForAuth(Ambiguous, EFlockAuthMethod::Email);

	// The whole reason ForAuth exists: the same code means different things per credential.
	TestNotEqual(TEXT("device hint is refined"), Device, ContextFree);
	TestNotEqual(TEXT("email hint is refined"), Email, ContextFree);
	TestNotEqual(TEXT("device and email differ"), Device, Email);
	TestTrue(TEXT("device hint names device registration"), Device.Contains(TEXT("Flock Register With Device")));
	TestTrue(TEXT("email hint names the wrong password"), Email.Contains(TEXT("Wrong email or password")));

	// A hint must never name a call that does not exist: neither provider has a register route.
	const EFlockAuthMethod LinkOnly[] = { EFlockAuthMethod::Facebook, EFlockAuthMethod::Discord };
	for (const EFlockAuthMethod Method : LinkOnly)
	{
		const FString Hint = FFlockErrorHints::ForAuth(Ambiguous, Method);
		TestFalse(TEXT("link-only provider hint does not name a register node"),
			Hint.Contains(TEXT("Call Flock Register With")));
		TestTrue(TEXT("link-only provider hint names the link node"), Hint.Contains(TEXT("Flock Link ")));
	}

	// The OAuth providers that do have one get told about both ways in.
	const EFlockAuthMethod Registerable[] = { EFlockAuthMethod::Google, EFlockAuthMethod::Apple, EFlockAuthMethod::Steam };
	for (const EFlockAuthMethod Method : Registerable)
	{
		const FString Hint = FFlockErrorHints::ForAuth(Ambiguous, Method);
		TestTrue(TEXT("registerable provider hint names a register node"), Hint.Contains(TEXT("Flock Register With ")));
		TestTrue(TEXT("registerable provider hint names a link node"), Hint.Contains(TEXT("Flock Link ")));
	}

	// Every other code is context-free, so ForAuth must hand back exactly what For does.
	TestEqual(TEXT("unrelated code passes through"),
		FFlockErrorHints::ForAuth(EFlockErrorCode::ShopInsufficientFunds, EFlockAuthMethod::Device),
		FFlockErrorHints::For(EFlockErrorCode::ShopInsufficientFunds));
	TestEqual(TEXT("a method with no credential falls back"),
		FFlockErrorHints::ForAuth(Ambiguous, EFlockAuthMethod::SessionRestore), ContextFree);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
