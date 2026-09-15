// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockLogger.h"
#include "FlockProtokiteClient.h"
#include "Http/FlockHttpClient.h"
#include "Tests/FlockPlaytestFakeTransport.h"
#include "Tests/FlockPlaytestTestSupport.h"

namespace
{
	struct FClientFixture
	{
		TSharedRef<FFlockPlaytestFakeTransport> Transport = MakeShared<FFlockPlaytestFakeTransport>();
		TSharedPtr<FFlockProtokiteClient> Client;
		TSharedRef<TOptional<TFlockResult<FFlockPlaytestConfig>>> Result = MakeShared<TOptional<TFlockResult<FFlockPlaytestConfig>>>();

		FClientFixture()
		{
			const TSharedRef<IFlockLogger> Logger = MakeShared<FFlockNullLogger>();
			FFlockRetryPolicy Policy;
			Policy.MaxRetries = 3;
			Policy.InitialDelaySeconds = 0.f;
			Policy.bUseJitter = false;
			Client = MakeShared<FFlockProtokiteClient>(MakeShared<FFlockHttpClient>(Transport, Logger), Policy, Logger);
		}

		void Fetch()
		{
			TMap<FString, FString> Headers;
			Headers.Add(TEXT("X-Flock-API-Key"), TEXT("secret"));
			Headers.Add(TEXT("X-Game-Version-ID"), FlockPlaytestFixtures::GameVersionId);
			const TSharedRef<TOptional<TFlockResult<FFlockPlaytestConfig>>> Out = Result;
			Client->FetchPlaytestConfig(TEXT("http://localhost:8020"), Headers,
				[Out](TFlockResult<FFlockPlaytestConfig> Answer) { *Out = Answer; });
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockProtokiteClientJoinsUrlTest,
	"Flock.Playtest.Client.JoinsUrlWithOrWithoutTrailingSlash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockProtokiteClientJoinsUrlTest::RunTest(const FString& Parameters)
{
	const FString Expected = TEXT("http://localhost:8020/game/sdk/playtest-config");
	TestEqual(TEXT("No trailing slash"), FFlockProtokiteClient::MakePlaytestConfigUrl(TEXT("http://localhost:8020")), Expected);
	TestEqual(TEXT("One trailing slash"), FFlockProtokiteClient::MakePlaytestConfigUrl(TEXT("http://localhost:8020/")), Expected);
	TestEqual(TEXT("Two trailing slashes"), FFlockProtokiteClient::MakePlaytestConfigUrl(TEXT("http://localhost:8020//")), Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockProtokiteClientSendsWhatItIsGivenTest,
	"Flock.Playtest.Client.SendsTheHeadersItIsGiven",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockProtokiteClientSendsWhatItIsGivenTest::RunTest(const FString& Parameters)
{
	FClientFixture Fixture;
	Fixture.Transport->Answer(FlockPlaytestFixtures::PlaytestConfigRoute,
		FFlockPlaytestFakeTransport::Status(200, FlockPlaytestFixtures::ConfigBody()));
	Fixture.Fetch();

	if (TestEqual(TEXT("One request"), Fixture.Transport->Requests.Num(), 1))
	{
		const FFlockHttpRequest& Request = Fixture.Transport->Requests[0];
		TestEqual(TEXT("A GET"), Request.Method, FString(TEXT("GET")));
		TestEqual(TEXT("To the playtest-config route"), Request.Url, FString(TEXT("http://localhost:8020/game/sdk/playtest-config")));
		TestEqual(TEXT("With the API key given"), Request.Headers.FindRef(TEXT("X-Flock-API-Key")), FString(TEXT("secret")));
		TestEqual(TEXT("With the version id given"), Request.Headers.FindRef(TEXT("X-Game-Version-ID")),
			FString(FlockPlaytestFixtures::GameVersionId));
	}
	TestTrue(TEXT("The config is loaded"), Fixture.Result->IsSet() && Fixture.Result->GetValue().bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockProtokiteClientRefusalsAreNotRetriedTest,
	"Flock.Playtest.Client.RefusalsAreNotRetried",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockProtokiteClientRefusalsAreNotRetriedTest::RunTest(const FString& Parameters)
{
	struct FRefusal
	{
		int32 StatusCode;
		const TCHAR* Body;
	};
	const FRefusal Refusals[] = {
		{ 404, FlockPlaytestFixtures::NotLinkedBody },
		{ 401, FlockPlaytestFixtures::InvalidApiKeyBody },
		{ 422, FlockPlaytestFixtures::MissingApiKeyBody },
	};
	for (const FRefusal& Refusal : Refusals)
	{
		FClientFixture Fixture;
		Fixture.Transport->Answer(FlockPlaytestFixtures::PlaytestConfigRoute,
			FFlockPlaytestFakeTransport::Status(Refusal.StatusCode, Refusal.Body));
		Fixture.Fetch();
		RunPendingPlaytestRetries();

		TestEqual(FString::Printf(TEXT("HTTP %d is asked once"), Refusal.StatusCode), Fixture.Transport->Requests.Num(), 1);
		TestTrue(FString::Printf(TEXT("HTTP %d fails with its status"), Refusal.StatusCode),
			Fixture.Result->IsSet() && !Fixture.Result->GetValue().bSuccess
			&& Fixture.Result->GetValue().Error.StatusCode == Refusal.StatusCode);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockProtokiteClientFailuresAreRetriedTest,
	"Flock.Playtest.Client.FailuresAreRetried",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockProtokiteClientFailuresAreRetriedTest::RunTest(const FString& Parameters)
{
	const FFlockHttpResponse Failures[] = {
		FFlockPlaytestFakeTransport::Status(503, FlockPlaytestFixtures::FlockUnreachableBody),
		FFlockPlaytestFakeTransport::ConnectionFailure(),
	};
	for (const FFlockHttpResponse& Failure : Failures)
	{
		FClientFixture Fixture;
		Fixture.Transport->AnswerInOrder(FlockPlaytestFixtures::PlaytestConfigRoute,
			{ Failure, FFlockPlaytestFakeTransport::Status(200, FlockPlaytestFixtures::ConfigBody()) });
		Fixture.Fetch();
		RunPendingPlaytestRetries();

		const FString What = Failure.Result == EFlockHttpResult::ConnectionError
			? FString(TEXT("A connection failure"))
			: FString::Printf(TEXT("HTTP %d"), Failure.StatusCode);
		TestEqual(What + TEXT(" is asked again"), Fixture.Transport->Requests.Num(), 2);
		TestTrue(What + TEXT(" then succeeds on the retry"), Fixture.Result->IsSet() && Fixture.Result->GetValue().bSuccess);
	}
	return true;
}

#endif
