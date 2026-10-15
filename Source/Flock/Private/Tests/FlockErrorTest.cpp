// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Http/FlockError.h"
#include "Http/FlockErrorHints.h"
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
		const FFlockError Error = FFlockError::Make(EFlockErrorType::Validation, TEXT("Email already registered"),
			400, FString(), Code);
		TestTrue(FString::Printf(TEXT("%s -> already registered"), Code), Error.IsAlreadyRegistered());
		TestTrue(FString::Printf(TEXT("%s -> library agrees"), Code), UFlockErrorLibrary::IsAlreadyRegistered(Error));
	}

	// A taken display name is a different fix, so it is deliberately outside the group.
	TestFalse(TEXT("name taken excluded"),
		FFlockError::Make(EFlockErrorType::Validation, TEXT("Name already registered"), 400, FString(),
			TEXT("player.name_already_registered")).IsAlreadyRegistered());

	TestFalse(TEXT("no code -> false"),
		FFlockError::Make(EFlockErrorType::Network, TEXT("The server failed while handling the request"), 500).IsAlreadyRegistered());

	TestFalse(TEXT("unrelated code -> false"),
		FFlockError::Make(EFlockErrorType::Network, TEXT("Shop item not found"), 404, FString(),
			TEXT("shop.item_not_found")).IsAlreadyRegistered());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockErrorDisplayTest, "Flock.Http.Error.Display",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockErrorDisplayTest::RunTest(const FString& Parameters)
{
	const FFlockError Error = FFlockError::Make(EFlockErrorType::Validation, TEXT("Password too short"),
		422, TEXT("{\"detail\":{\"code\":\"player.invalid_registration_request\",\"message\":\"Password too short\"}}"),
		TEXT("player.invalid_registration_request"), TEXT("Password too short"));

	TestEqual(TEXT("server message carried"), Error.ServerMessage, FString(TEXT("Password too short")));
	TestEqual(TEXT("the message is what is wrong"), Error.Message, FString(TEXT("Password too short")));

	const FString Text = Error.ToString();
	TestTrue(TEXT("display has type"), Text.Contains(TEXT("Validation")));
	TestTrue(TEXT("display has status"), Text.Contains(TEXT("422")));
	TestTrue(TEXT("display has body"), Text.Contains(TEXT("{\"detail\"")));

	// Client-side errors carry no status; the display text must not claim one.
	const FString NoStatus = FFlockError::Make(EFlockErrorType::Timeout, TEXT("The server did not answer in time")).ToString();
	TestFalse(TEXT("no status -> no HTTP suffix"), NoStatus.Contains(TEXT("HTTP")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockErrorComposedMessageTest, "Flock.Http.Error.ComposedMessage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockErrorComposedMessageTest::RunTest(const FString& Parameters)
{
	FFlockError Error = FFlockError::Make(EFlockErrorType::Validation, TEXT("Not enough gold"),
		400, TEXT("{\"detail\":{\"code\":\"shop.insufficient_funds\",\"message\":\"Not enough gold\"}}"),
		TEXT("shop.insufficient_funds"), TEXT("Not enough gold"));
	Error.Operation = TEXT("Purchase shop item");

	const FString Composed = Error.ToDisplayText();
	TestEqual(TEXT("library composes the same text"), UFlockErrorLibrary::ToDisplayString(Error), Composed);
	TestEqualSensitive(TEXT("operation, reason, tag, then the fix"), Composed,
		FString(TEXT("Purchase shop item failed: Not enough gold [shop.insufficient_funds, HTTP 400]\nFix: "))
			+ FFlockErrorHints::For(EFlockErrorCode::ShopInsufficientFunds));
	TestEqualSensitive(TEXT("the same without the operation"), Error.ReasonAndFix(),
		FString(TEXT("Not enough gold [shop.insufficient_funds, HTTP 400]\nFix: ")) + FFlockErrorHints::For(EFlockErrorCode::ShopInsufficientFunds));

	// Unbounded payload stays on ToString(); the composed line is what an error tracker buckets on.
	TestFalse(TEXT("body kept out of the composed text"), Composed.Contains(TEXT("{\"detail\"")));
	TestTrue(TEXT("body still on ToString"), Error.ToString().Contains(TEXT("{\"detail\"")));

	// No code: the status rides on the tag, and the fix is the one for an answer that did not come from Flock.
	const FFlockError Bare = FFlockError::Make(EFlockErrorType::Validation, TEXT("The server refused a value in the request"), 422);
	TestTrue(TEXT("a bare failure tags its status"), Bare.ToDisplayText().StartsWith(TEXT("The server refused a value in the request [HTTP 422]\nFix: ")));

	// A failure the SDK finds itself: no status, no code, its own fix.
	const FFlockError ClientSide = FFlockError::MakeWithFix(EFlockErrorType::Validation, TEXT("Shop Item ID is empty"), TEXT("Pass a Shop Item ID."));
	TestEqualSensitive(TEXT("client-side text is its reason and its fix"), ClientSide.ToDisplayText(),
		FString(TEXT("Shop Item ID is empty\nFix: Pass a Shop Item ID.")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockErrorHintStampedTest, "Flock.Http.Error.HintStamped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockErrorHintStampedTest::RunTest(const FString& Parameters)
{
	// Make is the one place a failure is built, so the fix travels with every one.
	const FFlockError Coded = FFlockError::Make(EFlockErrorType::Validation, TEXT("Device already registered"),
		400, FString(), TEXT("player.device_already_registered"));
	TestEqual(TEXT("hint stamped from the code"), Coded.Hint,
		FFlockErrorHints::For(EFlockErrorCode::PlayerDeviceAlreadyRegistered));
	TestTrue(TEXT("hint is not empty"), !Coded.Hint.IsEmpty());
	TestEqualSensitive(TEXT("and it is the failure's fix"), Coded.Hint, FFlockErrorHints::ForFailure(Coded));

	// A code this SDK version does not recognize has no advice of its own; the fix for its kind names it.
	const FFlockError Unrecognized = FFlockError::Make(EFlockErrorType::Validation, TEXT("Something new"),
		400, FString(), TEXT("player.something_new"));
	TestEqual(TEXT("unknown code parses to Unknown"), Unrecognized.ErrorCode, EFlockErrorCode::Unknown);
	TestTrue(TEXT("unknown code -> the fix names it"), Unrecognized.Hint.Contains(TEXT("player.something_new")));

	TestFalse(TEXT("no code -> a fix for the kind"),
		FFlockError::Make(EFlockErrorType::Timeout, TEXT("The server did not answer in time")).Hint.IsEmpty());

	return true;
}

#endif // WITH_AUTOMATION_TESTS
