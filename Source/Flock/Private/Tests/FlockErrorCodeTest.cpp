// Copyright 2022, Qwacks. All Rights Reserved.

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

	TestEqual(TEXT("unknown code -> Unknown"),
		static_cast<int32>(FFlockErrorCodes::Parse(TEXT("nope.not_a_real_code"))),
		static_cast<int32>(EFlockErrorCode::Unknown));

	TestEqual(TEXT("empty -> Unknown"),
		static_cast<int32>(FFlockErrorCodes::Parse(TEXT(""))),
		static_cast<int32>(EFlockErrorCode::Unknown));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
