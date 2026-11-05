// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"
#include "Misc/FlockEngineCompat.h"

#if WITH_AUTOMATION_TESTS

#include "Http/FlockErrorCode.h"
#include "Http/FlockErrorHints.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockResult.h"
#include "Models/FlockGameModels.h"
#include "FlockLogger.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockTestRefusals.h"
#include "UObject/Class.h"

namespace
{
	struct FPaymentRefusal
	{
		int32 Status;
		EFlockErrorType Type;
		EFlockErrorCode Member;
		const TCHAR* Body;
	};

	// Every refusal of the payment routes (redeeming a store receipt, opening a web checkout, the purchase and its list), body
	// as the backend sends it. The first nineteen copied from live answers on 2026-10-21, one row per reason; the next six the
	// backend's own messages for causes no live run reached (a store out of reach, an unreadable store answer, a Google payment
	// still pending, a receipt for another product, a provider that would not open a checkout, a purchase id the game does not
	// have), then Stripe's refusal read live on 2026-10-31; then the payment hardening's, read in its code. The last three were
	// measured on the local stack on 2026-11-05 (an item switched off, an unknown status) or read in its code (a reused key).
	const FPaymentRefusal PaymentRefusals[] = {
		{ 503, EFlockErrorType::Network, EFlockErrorCode::PaymentProviderCredentialsUnreadable,
			TEXT(R"BODY({"detail":{"code":"payment_provider.credentials_unreadable","message":"Apple credentials could not be used to sign a request"}})BODY") },
		{ 503, EFlockErrorType::Network, EFlockErrorCode::PaymentProviderCredentialsUnreadable,
			TEXT(R"BODY({"detail":{"code":"payment_provider.credentials_unreadable","message":"Google service account credentials are unusable"}})BODY") },
		{ 404, EFlockErrorType::Network, EFlockErrorCode::PaymentProviderProviderNotFound,
			TEXT(R"BODY({"detail":{"code":"payment_provider.provider_not_found","message":"Provider 'apple_app_store' is not configured for this game"}})BODY") },
		{ 404, EFlockErrorType::Network, EFlockErrorCode::PaymentProviderProviderNotFound,
			TEXT(R"BODY({"detail":{"code":"payment_provider.provider_not_found","message":"Provider 'stripe' is not configured for this game"}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::PaymentProviderProviderNotSupportedHere,
			TEXT(R"BODY({"detail":{"code":"payment_provider.provider_not_supported_here","message":"'APPLE_APP_STORE' is not a store; use the checkout endpoint instead"}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::PaymentProviderProviderNotSupportedHere,
			TEXT(R"BODY({"detail":{"code":"payment_provider.provider_not_supported_here","message":"'steam' is not a store; use the checkout endpoint instead"}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::PaymentProviderProviderNotSupportedHere,
			TEXT(R"BODY({"detail":{"code":"payment_provider.provider_not_supported_here","message":"'stripe' is not a store; use the checkout endpoint instead"}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::PaymentProviderReceiptInvalid,
			TEXT(R"BODY({"detail":{"code":"payment_provider.receipt_invalid","message":"Receipt rejected: Apple returned 401"}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::PaymentProviderReceiptInvalid,
			TEXT(R"BODY({"detail":{"code":"payment_provider.receipt_invalid","message":"Receipt rejected: Google rejected the service account (400)"}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::PaymentProviderReceiptInvalid,
			TEXT(R"BODY({"detail":{"code":"payment_provider.receipt_invalid","message":"Receipt rejected: no transaction_id supplied"}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::PaymentProviderReceiptInvalid,
			TEXT(R"BODY({"detail":{"code":"payment_provider.receipt_invalid","message":"Receipt rejected: product_id and purchase_token are both required"}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::PaymentProviderReceiptProductMismatch,
			TEXT(R"BODY({"detail":{"code":"payment_provider.receipt_product_mismatch","message":"This item has no apple_app_store product id configured"}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::PaymentProviderReceiptProductMismatch,
			TEXT(R"BODY({"detail":{"code":"payment_provider.receipt_product_mismatch","message":"This item has no google_play product id configured"}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::ShopNotARealCurrencyItem,
			TEXT(R"BODY({"detail":{"code":"shop.not_a_real_currency_item","message":"'Gold' is a game currency; buy this through the transaction endpoint instead"}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::ShopRealCurrencySettlementUnavailable,
			TEXT(R"BODY({"detail":{"code":"shop.real_currency_settlement_unavailable","message":"This game has no web payment provider configured"}})BODY") },
		{ 503, EFlockErrorType::Network, EFlockErrorCode::ShopRealCurrencySettlementUnavailable,
			TEXT(R"BODY({"detail":{"code":"shop.real_currency_settlement_unavailable","message":"PAYMENT_WEBHOOK_BASE_URL is not configured, so the provider would have nowhere to confirm this payment"}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::ShopWebhookProviderUnknown,
			TEXT(R"BODY({"detail":{"code":"shop.webhook_provider_unknown","message":"'apple_app_store' does not open web checkouts"}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::ShopWebhookProviderUnknown,
			TEXT(R"BODY({"detail":{"code":"shop.webhook_provider_unknown","message":"'paypal' does not open web checkouts"}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::ShopWebhookProviderUnknown,
			TEXT(R"BODY({"detail":{"code":"shop.webhook_provider_unknown","message":"Several payment providers are active; name one of stripe, tap"}})BODY") },
		{ 502, EFlockErrorType::Network, EFlockErrorCode::PaymentProviderReceiptInvalid,
			TEXT(R"BODY({"detail":{"code":"payment_provider.receipt_invalid","message":"Could not reach apple_app_store to verify the receipt"}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::PaymentProviderReceiptInvalid,
			TEXT(R"BODY({"detail":{"code":"payment_provider.receipt_invalid","message":"Receipt rejected: the store returned an unreadable response"}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::PaymentProviderReceiptInvalid,
			TEXT(R"BODY({"detail":{"code":"payment_provider.receipt_invalid","message":"Receipt rejected: purchase state is 2, not purchased"}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::PaymentProviderReceiptProductMismatch,
			TEXT(R"BODY({"detail":{"code":"payment_provider.receipt_product_mismatch","message":"Receipt is for 'gems_10', but this item sells 'gems_100'"}})BODY") },
		{ 502, EFlockErrorType::Network, EFlockErrorCode::PaymentProviderCheckoutFailed,
			TEXT(R"BODY({"detail":{"code":"payment_provider.checkout_failed","message":"Could not open a tap checkout: gateway timeout"}})BODY") },
		{ 502, EFlockErrorType::Network, EFlockErrorCode::PaymentProviderCheckoutFailed,
			TEXT(R"BODY({"detail":{"code":"payment_provider.checkout_failed","message":"Could not open a stripe checkout: {\n  \"error\": {\n    \"code\": \"amount_too_small\",\n    \"message\": \"The Checkout Session's total amount must convert to at least 200 fils.\""}})BODY") },
		{ 404, EFlockErrorType::Network, EFlockErrorCode::ShopPurchaseNotFound,
			TEXT(R"BODY({"detail":{"code":"shop.purchase_not_found","message":"Purchase not found"}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::PaymentProviderReceiptInvalid,
			TEXT(R"BODY({"detail":{"code":"payment_provider.receipt_invalid","message":"Receipt rejected: could not read the transaction Apple returned"}})BODY") },
		{ 503, EFlockErrorType::Network, EFlockErrorCode::PaymentProviderStoreUnavailable,
			TEXT(R"BODY({"detail":{"code":"payment_provider.store_unavailable","message":"Apple could not be asked about this receipt (HTTP 503); try again later"}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::PaymentProviderStoreCredentialsRejected,
			TEXT(R"BODY({"detail":{"code":"payment_provider.store_credentials_rejected","message":"Apple rejected this game's store credentials (HTTP 401); the studio's payment settings need fixing before receipts can be verified"}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::PaymentProviderReceiptPending,
			TEXT(R"BODY({"detail":{"code":"payment_provider.receipt_pending","message":"Google reports this payment as still in progress; ask again later"}})BODY") },
		{ 403, EFlockErrorType::Auth, EFlockErrorCode::PaymentProviderReceiptBelongsToAnotherPlayer,
			TEXT(R"BODY({"detail":{"code":"payment_provider.receipt_belongs_to_another_player","message":"Receipt refused: the store purchase belongs to a different player"}})BODY") },
		{ 403, EFlockErrorType::Auth, EFlockErrorCode::PaymentProviderReceiptBelongsToAnotherPlayer,
			TEXT(R"BODY({"detail":{"code":"payment_provider.receipt_belongs_to_another_player","message":"Receipt refused: this store transaction was redeemed by another player"}})BODY") },
		{ 403, EFlockErrorType::Auth, EFlockErrorCode::ShopPlayerMismatch,
			TEXT(R"BODY({"detail":{"code":"shop.player_mismatch","message":"player_id does not match the signed-in player"}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::ShopPlayerRequired,
			TEXT(R"BODY({"detail":{"code":"shop.player_required","message":"Send the player's token, or name player_id in the body"}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::ShopPurchaseNotFound,
			TEXT(R"BODY({"detail":{"code":"shop.purchase_not_found","message":"Unknown purchase status 'done'"}})BODY") },
		{ 409, EFlockErrorType::Network, EFlockErrorCode::ShopItemNotForSale,
			TEXT(R"BODY({"detail":{"code":"shop.item_not_for_sale","message":"This item is not for sale right now."}})BODY") },
		{ 422, EFlockErrorType::Validation, EFlockErrorCode::ShopInvalidPurchaseStatus,
			TEXT(R"BODY({"detail":{"code":"shop.invalid_purchase_status","message":"Unknown purchase status 'done'"}})BODY") },
		{ 409, EFlockErrorType::Network, EFlockErrorCode::ShopIdempotencyKeyReused,
			TEXT(R"BODY({"detail":{"code":"shop.idempotency_key_reused","message":"idempotency_key was already used for a different item"}})BODY") },
	};

	struct FPaymentCause
	{
		EFlockErrorCode Member;
		const TCHAR* Phrase;
		bool bQuotesTheServer;
	};

	// These codes each have more than one cause, and only the server's reason tells them apart, so the hint covers every cause;
	// a cause that is not final yet gets "send it again later", never the bad receipt's fix. A phrase marked as the server's is
	// quoted in the hint, so it must be one a refusal above really carries.
	const FPaymentCause PaymentCauses[] = {
		{ EFlockErrorCode::PaymentProviderReceiptInvalid, TEXT("Apple returned 401"), true },
		{ EFlockErrorCode::PaymentProviderReceiptInvalid, TEXT("Google rejected the service account"), true },
		{ EFlockErrorCode::PaymentProviderReceiptInvalid, TEXT("Could not reach"), true },
		{ EFlockErrorCode::PaymentProviderReceiptInvalid, TEXT("unreadable"), true },
		{ EFlockErrorCode::PaymentProviderReceiptInvalid, TEXT("purchase state is 2"), true },
		{ EFlockErrorCode::PaymentProviderReceiptInvalid, TEXT("429 or a 5xx"), false },
		{ EFlockErrorCode::PaymentProviderReceiptInvalid, TEXT("purchase token"), false },
		{ EFlockErrorCode::PaymentProviderReceiptProductMismatch, TEXT("product id configured"), true },
		{ EFlockErrorCode::PaymentProviderReceiptProductMismatch, TEXT("but this item sells"), true },
		{ EFlockErrorCode::PaymentProviderProviderNotFound, TEXT("never connected"), false },
		{ EFlockErrorCode::PaymentProviderProviderNotFound, TEXT("paused"), false },
		{ EFlockErrorCode::PaymentProviderCredentialsUnreadable, TEXT("trying again will not help"), false },
		{ EFlockErrorCode::ShopRealCurrencySettlementUnavailable, TEXT("no web payment provider configured"), true },
		{ EFlockErrorCode::ShopRealCurrencySettlementUnavailable, TEXT("PAYMENT_WEBHOOK_BASE_URL is not configured"), true },
		{ EFlockErrorCode::ShopWebhookProviderUnknown, TEXT("does not open web checkouts"), true },
		{ EFlockErrorCode::ShopWebhookProviderUnknown, TEXT("Several payment providers are active"), true },
		{ EFlockErrorCode::PaymentProviderCheckoutFailed, TEXT("amount_too_small"), true },
		{ EFlockErrorCode::PaymentProviderCheckoutFailed, TEXT("raising the item's price"), false },
		{ EFlockErrorCode::PaymentProviderReceiptInvalid, TEXT("could not read"), true },
		{ EFlockErrorCode::PaymentProviderReceiptInvalid, TEXT("An older Flock server"), false },
		{ EFlockErrorCode::PaymentProviderStoreUnavailable, TEXT("send the same receipt again later"), false },
		{ EFlockErrorCode::PaymentProviderReceiptPending, TEXT("send the same receipt again later"), false },
		{ EFlockErrorCode::PaymentProviderStoreCredentialsRejected, TEXT("the same receipt goes through"), false },
		{ EFlockErrorCode::PaymentProviderStoreCredentialsRejected, TEXT("Google Play Console"), false },
		{ EFlockErrorCode::PaymentProviderReceiptBelongsToAnotherPlayer, TEXT("belongs to a different player"), true },
		{ EFlockErrorCode::PaymentProviderReceiptBelongsToAnotherPlayer, TEXT("redeemed by another player"), true },
		{ EFlockErrorCode::ShopPurchaseNotFound, TEXT("another player's purchase"), false },
		{ EFlockErrorCode::ShopPurchaseNotFound, TEXT("Unknown purchase status"), true },
		{ EFlockErrorCode::ShopPlayerMismatch, TEXT("Leave Player Id empty"), false },
		// Consuming while signed in answers another player's entry as not found (the same hardening).
		{ EFlockErrorCode::PlayerInventoryInventoryEntryNotFound, TEXT("another player's"), false },
		// Switched off in the dashboard: the shops a player reads leave a switched-off shop out.
		{ EFlockErrorCode::ShopShopNotFound, TEXT("switched off (Inactive)"), false },
		{ EFlockErrorCode::ShopItemShopNotFound, TEXT("switched off (Inactive)"), false },
	};

	/** Every code the payment work added: a hint for each of them may name only calls this SDK has. */
	const EFlockErrorCode PaymentCodes[] = {
		EFlockErrorCode::PaymentProviderCheckoutFailed, EFlockErrorCode::PaymentProviderCredentialsUnreadable,
		EFlockErrorCode::PaymentProviderProviderNotFound, EFlockErrorCode::PaymentProviderProviderNotSupportedHere,
		EFlockErrorCode::PaymentProviderReceiptBelongsToAnotherPlayer, EFlockErrorCode::PaymentProviderReceiptInvalid,
		EFlockErrorCode::PaymentProviderReceiptPending, EFlockErrorCode::PaymentProviderReceiptProductMismatch,
		EFlockErrorCode::PaymentProviderStoreCredentialsRejected, EFlockErrorCode::PaymentProviderStoreUnavailable,
		EFlockErrorCode::ShopIdempotencyKeyReused, EFlockErrorCode::ShopInvalidPurchaseStatus, EFlockErrorCode::ShopItemNotForSale,
		EFlockErrorCode::ShopNotARealCurrencyItem, EFlockErrorCode::ShopPlayerMismatch, EFlockErrorCode::ShopPlayerRequired,
		EFlockErrorCode::ShopPurchaseNotFound, EFlockErrorCode::ShopRealCurrencySettlementUnavailable,
		EFlockErrorCode::ShopWebhookProviderUnknown,
	};

	/** Words of calls that do not exist in this SDK yet: a hint naming one sends the developer looking for nothing. */
	const TCHAR* const CallsThisSdkDoesNotHave[] = { TEXT("Async"), TEXT("RedeemStoreReceipt"), TEXT("StartCheckout"),
		TEXT("FlockStore"), TEXT("GetPurchases"), TEXT("PurchaseResultStatuses"), TEXT("PaymentProviders.") };

	FString NameOf(EFlockErrorCode Code)
	{
		return StaticEnum<EFlockErrorCode>()->GetNameStringByValue(static_cast<int64>(Code));
	}

	bool NamesACallThisSdkDoesNotHave(const FString& Hint)
	{
		for (const TCHAR* Word : CallsThisSdkDoesNotHave)
		{
			if (Hint.Contains(Word, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPaymentRefusalsParseTest, "Flock.Http.ErrorHints.PaymentRefusalsParseToTheirCode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPaymentRefusalsParseTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
	const TSharedRef<FFlockHttpClient> Client = MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>());

	TestEqual(TEXT("one row for each refusal kept"), static_cast<int32>(UE_ARRAY_COUNT(PaymentRefusals)), 38);

	TSet<EFlockErrorCode> Seen;
	for (const FPaymentRefusal& Refusal : PaymentRefusals)
	{
		FlockTestRefusals::CheckRefusal(*this, *Fake, *Client, FString(Refusal.Body).Mid(0, 90), Refusal.Status, Refusal.Type,
			Refusal.Member, Refusal.Body);
		Seen.Add(Refusal.Member);
	}

	// Every payment code has at least one body as the server sends it.
	for (const EFlockErrorCode Code : PaymentCodes)
	{
		TestTrue(FString::Printf(TEXT("%s has a refusal row"), *NameOf(Code)), Seen.Contains(Code));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPaymentCausesTest, "Flock.Http.ErrorHints.EachCauseOfAPaymentCodeIsToldApartInItsHint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPaymentCausesTest::RunTest(const FString& Parameters)
{
	for (const FPaymentCause& Cause : PaymentCauses)
	{
		const FString Hint = FFlockErrorHints::For(Cause.Member);
		TestTrue(FString::Printf(TEXT("%s's hint covers the cause \"%s\""), *NameOf(Cause.Member), Cause.Phrase),
			Hint.Contains(Cause.Phrase, ESearchCase::CaseSensitive));
		if (!Cause.bQuotesTheServer)
		{
			continue;
		}
		bool bSent = false;
		for (const FPaymentRefusal& Refusal : PaymentRefusals)
		{
			if (Refusal.Member == Cause.Member && FString(Refusal.Body).Contains(Cause.Phrase, ESearchCase::CaseSensitive))
			{
				bSent = true;
			}
		}
		TestTrue(FString::Printf(TEXT("%s's hint quotes \"%s\", which a refusal of that code carries"),
			*NameOf(Cause.Member), Cause.Phrase), bSent);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPaymentHintsNameOnlyRealCallsTest, "Flock.Http.ErrorHints.APaymentHintNamesOnlyCallsThisSdkHas",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPaymentHintsNameOnlyRealCallsTest::RunTest(const FString& Parameters)
{
	// The check has to catch a call that does not exist here, or every "names none" below proves nothing.
	TestTrue(TEXT("the check catches a redeem call this SDK does not have"),
		NamesACallThisSdkDoesNotHave(TEXT("send the same receipt again later with Shop.RedeemStoreReceiptAsync.")));
	TestFalse(TEXT("and leaves a call this SDK has alone"),
		NamesACallThisSdkDoesNotHave(TEXT("Leave Player Id empty on Purchase (Flock Purchase).")));

	for (const EFlockErrorCode Code : PaymentCodes)
	{
		const FString Hint = FFlockErrorHints::For(Code);
		TestFalse(FString::Printf(TEXT("%s has a hint"), *NameOf(Code)), Hint.IsEmpty());
		TestFalse(FString::Printf(TEXT("%s's hint names a call this SDK does not have: %s"), *NameOf(Code), *Hint),
			NamesACallThisSdkDoesNotHave(Hint));
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
