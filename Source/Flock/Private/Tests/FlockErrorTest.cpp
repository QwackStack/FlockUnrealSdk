// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Http/FlockError.h"
#include "Http/FlockErrorLibrary.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockErrorAlreadyRegisteredTest, "Flock.Http.Error.AlreadyRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockErrorAlreadyRegisteredTest::RunTest(const FString& Parameters)
{
	const TCHAR* IdentityCodes[] = {
		TEXT("player.email_already_registered"),
		TEXT("player.device_already_registered"),
		TEXT("player.google_account_already_registered"),
		TEXT("player.apple_account_already_registered"),
		TEXT("player.steam_account_already_registered"),
	};
	for (const TCHAR* Code : IdentityCodes)
	{
		const FFlockError Error = FFlockError::Make(EFlockErrorType::Validation, TEXT("Validation failed"),
			400, FString(), Code);
		TestTrue(FString::Printf(TEXT("%s -> already registered"), Code), Error.IsAlreadyRegistered());
		TestTrue(FString::Printf(TEXT("%s -> library agrees"), Code), UFlockErrorLibrary::IsAlreadyRegistered(Error));
	}

	// A taken display name is a different fix, so it is deliberately outside the group.
	TestFalse(TEXT("name taken excluded"),
		FFlockError::Make(EFlockErrorType::Validation, TEXT("Validation failed"), 400, FString(),
			TEXT("player.name_already_registered")).IsAlreadyRegistered());

	TestFalse(TEXT("no code -> false"),
		FFlockError::Make(EFlockErrorType::Network, TEXT("HTTP request failed"), 500).IsAlreadyRegistered());

	TestFalse(TEXT("unrelated code -> false"),
		FFlockError::Make(EFlockErrorType::Network, TEXT("HTTP request failed"), 404, FString(),
			TEXT("shop.item_not_found")).IsAlreadyRegistered());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockErrorDisplayTest, "Flock.Http.Error.Display",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockErrorDisplayTest::RunTest(const FString& Parameters)
{
	const FFlockError Error = FFlockError::Make(EFlockErrorType::Validation, TEXT("Validation failed (HTTP 422)"),
		422, TEXT("{\"detail\":{\"code\":\"player.invalid_registration_request\",\"message\":\"Password too short\"}}"),
		TEXT("player.invalid_registration_request"), TEXT("Password too short"));

	TestEqual(TEXT("server message carried"), Error.ServerMessage, FString(TEXT("Password too short")));
	TestEqual(TEXT("message stays terse"), Error.Message, FString(TEXT("Validation failed (HTTP 422)")));

	const FString Text = Error.ToString();
	TestTrue(TEXT("display has type"), Text.Contains(TEXT("Validation")));
	TestTrue(TEXT("display has status"), Text.Contains(TEXT("422")));
	TestTrue(TEXT("display has body"), Text.Contains(TEXT("Password too short")));
	TestEqual(TEXT("library matches ToString"), UFlockErrorLibrary::ToDisplayString(Error), Text);

	// Client-side errors carry no status; the display text must not claim one.
	const FString NoStatus = FFlockError::Make(EFlockErrorType::Timeout, TEXT("Request timeout")).ToString();
	TestFalse(TEXT("no status -> no HTTP suffix"), NoStatus.Contains(TEXT("HTTP")));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
