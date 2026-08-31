// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Http/FlockErrorCode.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockErrorCodeParseTest, "Flock.Http.ErrorCode.Parse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockErrorCodeParseTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("known player code"),
		static_cast<int32>(FFlockErrorCodes::Parse(TEXT("player.email_already_registered"))),
		static_cast<int32>(EFlockErrorCode::PlayerEmailAlreadyRegistered));

	TestEqual(TEXT("known game_version code"),
		static_cast<int32>(FFlockErrorCodes::Parse(TEXT("game_version.game_version_by_name_not_found"))),
		static_cast<int32>(EFlockErrorCode::GameVersionGameVersionByNameNotFound));

	// Observed live: the by-name lookup 404s with this code for a board the game does not have.
	TestEqual(TEXT("known leaderboard code"),
		static_cast<int32>(FFlockErrorCodes::Parse(TEXT("leaderboard.not_found"))),
		static_cast<int32>(EFlockErrorCode::LeaderboardNotFound));

	// Observed live: `notification_template.not_found`, not `notification.template_not_found` — the two
	// spellings PascalCase to the same member name, so only the wire string tells them apart.
	TestEqual(TEXT("known notification template code"),
		static_cast<int32>(FFlockErrorCodes::Parse(TEXT("notification_template.not_found"))),
		static_cast<int32>(EFlockErrorCode::NotificationTemplateNotFound));

	// The namespace repeats inside the reason here, and the doubled word is not a typo.
	TestEqual(TEXT("known player_inventory code"),
		static_cast<int32>(FFlockErrorCodes::Parse(TEXT("player_inventory.inventory_entry_not_found"))),
		static_cast<int32>(EFlockErrorCode::PlayerInventoryInventoryEntryNotFound));

	// Consume and purchase share their reward failures; both arrive as shop.* even from the inventory route.
	TestEqual(TEXT("known shop reward code"),
		static_cast<int32>(FFlockErrorCodes::Parse(TEXT("shop.reward_currency_not_held"))),
		static_cast<int32>(EFlockErrorCode::ShopRewardCurrencyNotHeld));

	TestEqual(TEXT("unknown code -> Unknown"),
		static_cast<int32>(FFlockErrorCodes::Parse(TEXT("nope.not_a_real_code"))),
		static_cast<int32>(EFlockErrorCode::Unknown));

	TestEqual(TEXT("empty -> Unknown"),
		static_cast<int32>(FFlockErrorCodes::Parse(TEXT(""))),
		static_cast<int32>(EFlockErrorCode::Unknown));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
