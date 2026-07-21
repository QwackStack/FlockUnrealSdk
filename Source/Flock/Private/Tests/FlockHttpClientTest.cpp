// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Http/FlockHttpClient.h"
#include "Http/FlockResult.h"
#include "Models/FlockGameModels.h"
#include "FlockLogger.h"
#include "Tests/Support/FlockFakeTransport.h"

namespace
{
	TSharedRef<FFlockHttpClient> MakeClient(const TSharedRef<FFlockFakeTransport>& Fake)
	{
		const TSharedRef<IFlockLogger> Logger = MakeShared<FFlockNullLogger>();
		return MakeShared<FFlockHttpClient>(Fake, Logger);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockHttpClientStatusMappingTest, "Flock.Http.Client.StatusMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockHttpClientStatusMappingTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
	const TSharedRef<FFlockHttpClient> Client = MakeClient(Fake);

	auto TypeFor = [&](const FFlockHttpResponse& Response) -> int32
	{
		Fake->On(TEXT("test"), Response);
		EFlockErrorType Type = EFlockErrorType::None;
		Client->Get<FFlockGameVersionSchema>(TEXT("http://x/test"), {},
			[&Type](TFlockResult<FFlockGameVersionSchema> R) { Type = R.Error.Type; });
		return static_cast<int32>(Type);
	};

	TestEqual(TEXT("401 -> Auth"), TypeFor(FFlockFakeTransport::Status(401, TEXT(""))), static_cast<int32>(EFlockErrorType::Auth));
	TestEqual(TEXT("403 -> Auth"), TypeFor(FFlockFakeTransport::Status(403, TEXT(""))), static_cast<int32>(EFlockErrorType::Auth));
	TestEqual(TEXT("400 -> Validation"), TypeFor(FFlockFakeTransport::Status(400, TEXT(""))), static_cast<int32>(EFlockErrorType::Validation));
	TestEqual(TEXT("422 -> Validation"), TypeFor(FFlockFakeTransport::Status(422, TEXT(""))), static_cast<int32>(EFlockErrorType::Validation));
	TestEqual(TEXT("500 -> Network"), TypeFor(FFlockFakeTransport::Status(500, TEXT(""))), static_cast<int32>(EFlockErrorType::Network));
	TestEqual(TEXT("offline -> Connection"), TypeFor(FFlockFakeTransport::Offline()), static_cast<int32>(EFlockErrorType::Connection));
	TestEqual(TEXT("timeout -> Timeout"), TypeFor(FFlockFakeTransport::Timeout()), static_cast<int32>(EFlockErrorType::Timeout));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockHttpClientDeserializeTest, "Flock.Http.Client.Deserialize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockHttpClientDeserializeTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
	const TSharedRef<FFlockHttpClient> Client = MakeClient(Fake);
	const FString Url = TEXT("http://x/game_version/by-name/v1");

	Fake->On(TEXT("game_version"), FFlockFakeTransport::Ok(TEXT("{\"result\":{\"id\":\"ver-1\"}}")));
	bool bSuccess = false;
	FString Id;
	Client->Get<FFlockGameVersionSchema>(Url, {}, [&](TFlockResult<FFlockGameVersionSchema> R)
	{
		bSuccess = R.bSuccess;
		Id = R.Value.Id;
	});
	TestTrue(TEXT("success unwrapped"), bSuccess);
	TestEqual(TEXT("id deserialized"), Id, FString(TEXT("ver-1")));

	Fake->On(TEXT("game_version"), FFlockFakeTransport::Ok(TEXT("not json")));
	EFlockErrorType MalformedType = EFlockErrorType::None;
	Client->Get<FFlockGameVersionSchema>(Url, {}, [&](TFlockResult<FFlockGameVersionSchema> R) { MalformedType = R.Error.Type; });
	TestEqual(TEXT("malformed -> Serialization"), static_cast<int32>(MalformedType), static_cast<int32>(EFlockErrorType::Serialization));

	Fake->On(TEXT("game_version"), FFlockFakeTransport::Ok(TEXT("")));
	EFlockErrorType EmptyType = EFlockErrorType::None;
	Client->Get<FFlockGameVersionSchema>(Url, {}, [&](TFlockResult<FFlockGameVersionSchema> R) { EmptyType = R.Error.Type; });
	TestEqual(TEXT("empty body -> Serialization"), static_cast<int32>(EmptyType), static_cast<int32>(EFlockErrorType::Serialization));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockHttpClientPaginatedTest, "Flock.Http.Client.Paginated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockHttpClientPaginatedTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
	const TSharedRef<FFlockHttpClient> Client = MakeClient(Fake);
	const FString Url = TEXT("http://x/game_version?page=1");

	Fake->On(TEXT("game_version"), FFlockFakeTransport::Ok(
		TEXT("{\"result\":{\"items\":[{\"id\":\"a\"},{\"id\":\"b\"}],\"total\":2,\"page\":1,\"limit\":10}}")));
	bool bSuccess = false;
	TFlockPage<FFlockGameVersionSchema> Page;
	Client->GetPaged<FFlockGameVersionSchema>(Url, {}, [&](TFlockResult<TFlockPage<FFlockGameVersionSchema>> R)
	{
		bSuccess = R.bSuccess;
		Page = R.Value;
	});
	TestTrue(TEXT("paged success"), bSuccess);
	TestEqual(TEXT("two items"), Page.Items.Num(), 2);
	if (Page.Items.Num() == 2)
	{
		TestEqual(TEXT("item ids deserialized"), Page.Items[1].Id, FString(TEXT("b")));
	}
	TestEqual(TEXT("total carried"), Page.Total, 2);
	TestEqual(TEXT("page carried"), Page.Page, 1);
	TestEqual(TEXT("limit carried"), Page.Limit, 10);

	// Non-2xx goes through the same status->error mapping as the other verbs.
	Fake->On(TEXT("game_version"), FFlockFakeTransport::Coded(404, TEXT("game_version.game_version_not_found")));
	FFlockError Error;
	Client->GetPaged<FFlockGameVersionSchema>(Url, {}, [&Error](TFlockResult<TFlockPage<FFlockGameVersionSchema>> R) { Error = R.Error; });
	TestEqual(TEXT("paged error classified"), static_cast<int32>(Error.Type), static_cast<int32>(EFlockErrorType::Network));
	TestEqual(TEXT("paged error keeps code"), static_cast<int32>(Error.ErrorCode),
		static_cast<int32>(EFlockErrorCode::GameVersionGameVersionNotFound));

	// A 2xx body without items is a Serialization failure, not a silent empty page.
	Fake->On(TEXT("game_version"), FFlockFakeTransport::Ok(TEXT("{\"result\":{}}")));
	EFlockErrorType MissingItemsType = EFlockErrorType::None;
	Client->GetPaged<FFlockGameVersionSchema>(Url, {}, [&MissingItemsType](TFlockResult<TFlockPage<FFlockGameVersionSchema>> R) { MissingItemsType = R.Error.Type; });
	TestEqual(TEXT("missing items -> Serialization"), static_cast<int32>(MissingItemsType),
		static_cast<int32>(EFlockErrorType::Serialization));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockHttpClientCodedErrorTest, "Flock.Http.Client.CodedError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockHttpClientCodedErrorTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
	const TSharedRef<FFlockHttpClient> Client = MakeClient(Fake);

	Fake->On(TEXT("game_version"), FFlockFakeTransport::Coded(404, TEXT("game_version.game_version_by_name_not_found")));
	FFlockError Error;
	Client->Get<FFlockGameVersionSchema>(TEXT("http://x/game_version/by-name/v1"), {},
		[&Error](TFlockResult<FFlockGameVersionSchema> R) { Error = R.Error; });

	TestEqual(TEXT("status carried"), Error.StatusCode, 404);
	TestEqual(TEXT("raw code parsed"), Error.Code, FString(TEXT("game_version.game_version_by_name_not_found")));
	TestEqual(TEXT("typed error code"), static_cast<int32>(Error.ErrorCode),
		static_cast<int32>(EFlockErrorCode::GameVersionGameVersionByNameNotFound));
	TestEqual(TEXT("server message parsed"), Error.ServerMessage, FString(TEXT("test")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockHttpClientPostJsonTest, "Flock.Http.Client.PostJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockHttpClientPostJsonTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
	const TSharedRef<FFlockHttpClient> Client = MakeClient(Fake);

	Fake->On(TEXT("login"), FFlockFakeTransport::Ok(TEXT("{\"result\":{\"id\":\"p-1\"}}")));
	bool bSuccess = false;
	FString Id;
	Client->PostJson<FFlockGameVersionSchema>(TEXT("http://x/login"), {}, TEXT("{\"login_type\":\"email\"}"),
		[&](TFlockResult<FFlockGameVersionSchema> R) { bSuccess = R.bSuccess; Id = R.Value.Id; });

	TestTrue(TEXT("success"), bSuccess);
	TestEqual(TEXT("unwrapped"), Id, FString(TEXT("p-1")));
	TestEqual(TEXT("one request"), Fake->CountTo(TEXT("login")), 1);
	TestEqual(TEXT("method"), Fake->Requests.Last().Method, FString(TEXT("POST")));
	TestEqual(TEXT("body passed through"), Fake->Requests.Last().JsonBody, FString(TEXT("{\"login_type\":\"email\"}")));
	TestTrue(TEXT("has body"), Fake->Requests.Last().bHasBody);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
