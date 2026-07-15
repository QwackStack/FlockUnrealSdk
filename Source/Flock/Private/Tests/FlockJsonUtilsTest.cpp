// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Http/FlockJsonUtils.h"
#include "Models/FlockGameModels.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockJsonCaseRoundTripTest, "Flock.Http.Json.CaseRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockJsonCaseRoundTripTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("snake -> pascal"), FFlockJsonUtils::SnakeToPascal(TEXT("game_version_id")), FString(TEXT("GameVersionId")));
	TestEqual(TEXT("single word snake -> pascal"), FFlockJsonUtils::SnakeToPascal(TEXT("id")), FString(TEXT("Id")));
	TestEqual(TEXT("pascal -> snake"), FFlockJsonUtils::ToSnakeCase(TEXT("GameVersionId")), FString(TEXT("game_version_id")));
	TestEqual(TEXT("camel -> snake"), FFlockJsonUtils::ToSnakeCase(TEXT("releaseType")), FString(TEXT("release_type")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockJsonEnvelopeUnwrapTest, "Flock.Http.Json.EnvelopeUnwrap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockJsonEnvelopeUnwrapTest::RunTest(const FString& Parameters)
{
	FFlockGameVersionSchema Out;
	FString Error;
	const FString Body = TEXT("{\"result\":{\"id\":\"ver-1\",\"release_type\":\"prod\"}}");
	TestTrue(TEXT("unwrap succeeds"), FFlockJsonUtils::UnwrapResultToStruct(Body, Out, Error));
	TestEqual(TEXT("id populated"), Out.Id, FString(TEXT("ver-1")));
	TestEqual(TEXT("release_type mapped to ReleaseType"), Out.ReleaseType, FString(TEXT("prod")));

	FFlockGameVersionSchema Missing;
	FString MissingError;
	TestFalse(TEXT("missing result fails"),
		FFlockJsonUtils::UnwrapResultToStruct(TEXT("{\"error\":{\"code\":\"x\"}}"), Missing, MissingError));

	FFlockGameVersionSchema Malformed;
	FString MalformedError;
	TestFalse(TEXT("malformed body fails"),
		FFlockJsonUtils::UnwrapResultToStruct(TEXT("not json"), Malformed, MalformedError));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockJsonCodedErrorTest, "Flock.Http.Json.CodedError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockJsonCodedErrorTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("detail.code preferred"),
		FFlockJsonUtils::ParseCodedErrorCode(TEXT("{\"detail\":{\"code\":\"player.email_already_registered\"}}")),
		FString(TEXT("player.email_already_registered")));

	TestEqual(TEXT("error.code fallback"),
		FFlockJsonUtils::ParseCodedErrorCode(TEXT("{\"error\":{\"code\":\"game.game_not_found\"}}")),
		FString(TEXT("game.game_not_found")));

	TestEqual(TEXT("no code -> empty"),
		FFlockJsonUtils::ParseCodedErrorCode(TEXT("{\"result\":{}}")), FString());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockJsonPaginatedTest, "Flock.Http.Json.Paginated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockJsonPaginatedTest::RunTest(const FString& Parameters)
{
	const FString Body = TEXT("{\"result\":{\"items\":[{\"id\":\"a\"},{\"id\":\"b\"}],\"total\":2,\"page\":1,\"limit\":10}}");
	TFlockPage<FFlockGameVersionSchema> Page;
	FString Error;
	TestTrue(TEXT("paginated unwrap succeeds"), FFlockJsonUtils::UnwrapPaginated(Body, Page, Error));
	TestEqual(TEXT("two items"), Page.Items.Num(), 2);
	TestEqual(TEXT("total"), Page.Total, 2);
	TestEqual(TEXT("page"), Page.Page, 1);
	TestEqual(TEXT("limit"), Page.Limit, 10);
	if (Page.Items.Num() == 2)
	{
		TestEqual(TEXT("item 0 id"), Page.Items[0].Id, FString(TEXT("a")));
		TestEqual(TEXT("item 1 id"), Page.Items[1].Id, FString(TEXT("b")));
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
