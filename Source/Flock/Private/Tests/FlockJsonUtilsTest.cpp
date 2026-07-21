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
	FString OutCode;
	FString OutMessage;

	FFlockJsonUtils::ParseCodedError(
		TEXT("{\"detail\":{\"code\":\"player.email_already_registered\",\"message\":\"Email already registered\"}}"),
		OutCode, OutMessage);
	TestEqual(TEXT("detail.code preferred"), OutCode, FString(TEXT("player.email_already_registered")));
	TestEqual(TEXT("detail.message parsed"), OutMessage, FString(TEXT("Email already registered")));

	FFlockJsonUtils::ParseCodedError(TEXT("{\"detail\":{\"code\":\"game.game_not_found\"}}"), OutCode, OutMessage);
	TestEqual(TEXT("code without message"), OutCode, FString(TEXT("game.game_not_found")));
	TestEqual(TEXT("absent message -> empty"), OutMessage, FString());

	FFlockJsonUtils::ParseCodedError(TEXT("{\"error\":{\"code\":\"game.game_not_found\"}}"), OutCode, OutMessage);
	TestEqual(TEXT("error.code fallback"), OutCode, FString(TEXT("game.game_not_found")));
	TestEqual(TEXT("fallback has no message"), OutMessage, FString());

	FFlockJsonUtils::ParseCodedError(TEXT("{\"result\":{}}"), OutCode, OutMessage);
	TestEqual(TEXT("no code -> empty"), OutCode, FString());

	// Stale values must not survive a parse of a body without them.
	OutCode = TEXT("stale");
	OutMessage = TEXT("stale");
	FFlockJsonUtils::ParseCodedError(TEXT("not json"), OutCode, OutMessage);
	TestEqual(TEXT("malformed -> code cleared"), OutCode, FString());
	TestEqual(TEXT("malformed -> message cleared"), OutMessage, FString());

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockJsonUtilsOmitEmptyTest, "Flock.Http.Json.OmitEmptyStrings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockJsonUtilsOmitEmptyTest::RunTest(const FString& Parameters)
{
	FFlockGameVersionSchema Model;
	Model.Id = TEXT("id-1");
	Model.Name = TEXT("");   // must vanish with bOmitEmptyStrings
	Model.Env = TEXT("dev"); // must stay

	FString Json;
	TestTrue(TEXT("serializes"), FFlockJsonUtils::StructToWireJson(Model, Json, /*bOmitEmptyStrings*/ true));
	TestTrue(TEXT("keeps non-empty"), Json.Contains(TEXT("\"env\"")));
	TestTrue(TEXT("keeps id"), Json.Contains(TEXT("\"id\"")));
	TestFalse(TEXT("omits empty name"), Json.Contains(TEXT("\"name\"")));

	// Default keeps empties (existing callers unchanged).
	FString DefaultJson;
	TestTrue(TEXT("serializes default"), FFlockJsonUtils::StructToWireJson(Model, DefaultJson));
	TestTrue(TEXT("default keeps empty name"), DefaultJson.Contains(TEXT("\"name\"")));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
