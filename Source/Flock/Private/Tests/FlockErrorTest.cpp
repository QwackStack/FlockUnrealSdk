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

	// Client-side errors carry no status; the display text must not claim one.
	const FString NoStatus = FFlockError::Make(EFlockErrorType::Timeout, TEXT("Request timeout")).ToString();
	TestFalse(TEXT("no status -> no HTTP suffix"), NoStatus.Contains(TEXT("HTTP")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockErrorComposedMessageTest, "Flock.Http.Error.ComposedMessage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockErrorComposedMessageTest::RunTest(const FString& Parameters)
{
	FFlockError Error = FFlockError::Make(EFlockErrorType::Validation, TEXT("Validation failed (HTTP 400)"),
		400, TEXT("{\"detail\":{\"code\":\"shop.insufficient_funds\",\"message\":\"Not enough gold\"}}"),
		TEXT("shop.insufficient_funds"), TEXT("Not enough gold"));
	Error.Operation = TEXT("Purchase shop item");

	const FString Composed = Error.ToDisplayText();
	TestEqual(TEXT("library composes the same text"), UFlockErrorLibrary::ToDisplayString(Error), Composed);
	TestTrue(TEXT("names the operation"), Composed.StartsWith(TEXT("Purchase shop item failed: ")));
	TestTrue(TEXT("carries the server's reason"), Composed.Contains(TEXT("Not enough gold")));
	TestTrue(TEXT("tags code and status"), Composed.Contains(TEXT("[shop.insufficient_funds, HTTP 400]")));
	TestTrue(TEXT("names the fix"), Composed.Contains(TEXT("\nFix: ")));
	TestTrue(TEXT("the fix is the hint for the code"),
		Composed.Contains(FFlockErrorHints::For(EFlockErrorCode::ShopInsufficientFunds)));

	// Unbounded payload stays on ToString(); the composed line is what an error tracker buckets on.
	TestFalse(TEXT("body kept out of the composed text"), Composed.Contains(TEXT("{\"detail\"")));
	TestTrue(TEXT("body still on ToString"), Error.ToString().Contains(TEXT("{\"detail\"")));

	// No coded body: the terse message already names the status, so the tag must not repeat it.
	const FString Bare = FFlockError::Make(EFlockErrorType::Validation, TEXT("Validation failed (HTTP 422)"), 422).ToDisplayText();
	TestEqual(TEXT("bare failure reads as the terse message"), Bare, FString(TEXT("Validation failed (HTTP 422)")));

	// A field-error body has a reason but no code, so the status rides on the tag instead.
	FFlockError FieldErrors = FFlockError::Make(EFlockErrorType::Validation, TEXT("Validation failed (HTTP 422)"), 422,
		FString(), FString(), TEXT("body.player_data: Input should be a valid dictionary"));
	TestEqual(TEXT("reason plus status, no code"), FieldErrors.ToDisplayText(),
		FString(TEXT("body.player_data: Input should be a valid dictionary [HTTP 422]")));

	// A client-side failure has no operation, status, code or hint — it composes to exactly its message.
	const FFlockError ClientSide = FFlockError::Make(EFlockErrorType::Serialization, TEXT("Failed to serialize request body"));
	TestEqual(TEXT("client-side text is unchanged"), ClientSide.ToDisplayText(), ClientSide.Message);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockErrorHintStampedTest, "Flock.Http.Error.HintStamped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockErrorHintStampedTest::RunTest(const FString& Parameters)
{
	// Make is the one place a coded error is built, so the remedy travels with every failure.
	const FFlockError Coded = FFlockError::Make(EFlockErrorType::Validation, TEXT("Validation failed (HTTP 400)"),
		400, FString(), TEXT("player.device_already_registered"));
	TestEqual(TEXT("hint stamped from the code"), Coded.Hint,
		FFlockErrorHints::For(EFlockErrorCode::PlayerDeviceAlreadyRegistered));
	TestTrue(TEXT("hint is not empty"), !Coded.Hint.IsEmpty());

	// A code this SDK version does not recognize parses to Unknown, which is waived rather than hinted.
	const FFlockError Unrecognized = FFlockError::Make(EFlockErrorType::Network, TEXT("HTTP request failed (HTTP 500)"),
		500, FString(), TEXT("player.something_new"));
	TestEqual(TEXT("unknown code -> no hint"), Unrecognized.Hint, FString());

	TestEqual(TEXT("no code -> no hint"),
		FFlockError::Make(EFlockErrorType::Timeout, TEXT("Request timeout")).Hint, FString());

	return true;
}

#endif // WITH_AUTOMATION_TESTS
