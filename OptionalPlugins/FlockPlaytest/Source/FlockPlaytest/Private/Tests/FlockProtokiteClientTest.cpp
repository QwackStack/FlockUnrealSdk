// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockLogger.h"
#include "FlockProtokiteClient.h"
#include "Http/FlockHttpClient.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
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

namespace
{
	using FStartOutcome = TSharedRef<TOptional<TFlockResult<FFlockPlaytestSessionStartResult>>>;
	using FEndOutcome = TSharedRef<TOptional<TFlockResult<FFlockPlaytestSessionEndResult>>>;

	TMap<FString, FString> SessionTestHeaders()
	{
		TMap<FString, FString> Headers;
		Headers.Add(TEXT("X-Flock-API-Key"), TEXT("secret"));
		Headers.Add(TEXT("X-Game-Version-ID"), FlockPlaytestFixtures::GameVersionId);
		return Headers;
	}

	/** Starts a session through the fixture's client. The outcome stays unset while no answer has arrived. */
	FStartOutcome StartSession(FClientFixture& Fixture)
	{
		FFlockPlaytestSessionStartRequest Request;
		Request.Identity.DeviceId = TEXT("0f8fad5b-d9cb-469f-a165-70867728950e");
		Request.FlockSessionId = FlockPlaytestFixtures::FirstFlockSessionId;
		Request.DebugInfo.Add(TEXT("sdk_version"), TEXT("1.13.0"));

		const FStartOutcome Outcome = MakeShared<TOptional<TFlockResult<FFlockPlaytestSessionStartResult>>>();
		Fixture.Client->StartPlaytestSession(TEXT("http://localhost:8020"), SessionTestHeaders(), Request,
			[Outcome](TFlockResult<FFlockPlaytestSessionStartResult> Answer) { *Outcome = Answer; });
		return Outcome;
	}

	/** Ends the fixture's session through its client. The outcome stays unset while no answer has arrived. */
	FEndOutcome EndSession(FClientFixture& Fixture)
	{
		const FEndOutcome Outcome = MakeShared<TOptional<TFlockResult<FFlockPlaytestSessionEndResult>>>();
		Fixture.Client->EndPlaytestSession(TEXT("http://localhost:8020"), SessionTestHeaders(), FlockPlaytestFixtures::PlaytestSessionId,
			[Outcome](TFlockResult<FFlockPlaytestSessionEndResult> Answer) { *Outcome = Answer; });
		return Outcome;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockProtokiteClientStartIsSentOnceWhileEndIsRetriedTest,
	"Flock.Playtest.Client.StartIsSentOnceWhileEndIsRetried",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockProtokiteClientStartIsSentOnceWhileEndIsRetriedTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		FFlockHttpResponse Response;
		const TCHAR* What;
	};
	const FCase Cases[] = {
		{ FFlockPlaytestFakeTransport::ConnectionFailure(), TEXT("No connection") },
		{ FFlockPlaytestFakeTransport::TimedOut(), TEXT("A timeout") },
		{ FFlockPlaytestFakeTransport::Status(503, FlockPlaytestFixtures::FlockUnreachableBody), TEXT("HTTP 503") },
		{ FFlockPlaytestFakeTransport::Status(429, TEXT("{}")), TEXT("HTTP 429") },
		{ FFlockPlaytestFakeTransport::Status(408, TEXT("{}")), TEXT("HTTP 408") },
	};
	for (const FCase& Case : Cases)
	{
		FClientFixture Fixture;
		Fixture.Transport->Answer(FlockPlaytestFixtures::PlaytestSessionStartRoute, Case.Response);
		const FStartOutcome Outcome = StartSession(Fixture);
		RunPendingPlaytestRetries();

		TestEqual(FString::Printf(TEXT("%s: sent once, though three retries are allowed"), Case.What), Fixture.Transport->Requests.Num(), 1);
		TestTrue(FString::Printf(TEXT("%s: fails"), Case.What), Outcome->IsSet() && !Outcome->GetValue().bSuccess);
	}
	{
		FClientFixture Fixture;
		Fixture.Transport->bHoldReplies = true;
		const FStartOutcome Outcome = StartSession(Fixture);
		Fixture.Transport->DropAllHeldReplies();
		RunPendingPlaytestRetries();
		TestEqual(TEXT("An answer that never arrives: sent once"), Fixture.Transport->Requests.Num(), 1);
		TestFalse(TEXT("An answer that never arrives: no outcome"), Outcome->IsSet());
	}
	{
		// The same fixture does retry an end, so the single starts above are the rule and not retries that never run.
		FClientFixture Fixture;
		Fixture.Transport->AnswerInOrder(FlockPlaytestFixtures::PlaytestSessionEndRoute, {
			FFlockPlaytestFakeTransport::Status(503, FlockPlaytestFixtures::FlockUnreachableBody), FFlockPlaytestFakeTransport::NoContent() });
		const FEndOutcome Outcome = EndSession(Fixture);
		RunPendingPlaytestRetries();
		TestEqual(TEXT("An end that failed is sent again"), Fixture.Transport->Requests.Num(), 2);
		TestTrue(TEXT("And succeeds on the retry"), Outcome->IsSet() && Outcome->GetValue().bSuccess);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockProtokiteClientStartNeedsASessionIdTest,
	"Flock.Playtest.Client.StartNeedsASessionId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockProtokiteClientStartNeedsASessionIdTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		FString Body;
		bool bStarts;
		const TCHAR* What;
	};
	const FCase Cases[] = {
		{ FlockPlaytestFixtures::SessionStartBody(), true, TEXT("A session id") },
		{ FlockPlaytestFixtures::Envelope(TEXT("{}")), false, TEXT("No session id") },
		{ FlockPlaytestFixtures::SessionStartBody(TEXT("")), false, TEXT("An empty session id") },
		{ FlockPlaytestFixtures::SessionStartBody(FString(FlockPlaytestFixtures::PlaytestSessionId) + TEXT(" ")), false, TEXT("A session id with a space") },
	};
	for (const FCase& Case : Cases)
	{
		FClientFixture Fixture;
		Fixture.Transport->Answer(FlockPlaytestFixtures::PlaytestSessionStartRoute, FFlockPlaytestFakeTransport::Status(200, Case.Body));
		const FStartOutcome Outcome = StartSession(Fixture);

		if (TestTrue(FString::Printf(TEXT("%s: answered"), Case.What), Outcome->IsSet()))
		{
			TestEqual(FString::Printf(TEXT("%s: started"), Case.What), Outcome->GetValue().bSuccess, Case.bStarts);
			if (Case.bStarts)
			{
				TestEqual(TEXT("The session id is read"), Outcome->GetValue().Value.SessionId, FString(FlockPlaytestFixtures::PlaytestSessionId));
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockProtokiteClientStartSendsTheRequestItIsGivenTest,
	"Flock.Playtest.Client.StartSendsTheRequestItIsGiven",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockProtokiteClientStartSendsTheRequestItIsGivenTest::RunTest(const FString& Parameters)
{
	FClientFixture Fixture;
	Fixture.Transport->Answer(FlockPlaytestFixtures::PlaytestSessionStartRoute,
		FFlockPlaytestFakeTransport::Status(200, FlockPlaytestFixtures::SessionStartBody()));
	StartSession(Fixture);

	if (TestEqual(TEXT("One request"), Fixture.Transport->Requests.Num(), 1))
	{
		const FFlockHttpRequest& Request = Fixture.Transport->Requests[0];
		TestEqual(TEXT("A POST"), Request.Method, FString(TEXT("POST")));
		TestEqual(TEXT("To the session route"), Request.Url, FString(TEXT("http://localhost:8020/game/sdk/playtest-session")));
		TestEqual(TEXT("With the API key given"), Request.Headers.FindRef(TEXT("X-Flock-API-Key")), FString(TEXT("secret")));

		TSharedPtr<FJsonObject> Body;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Request.JsonBody);
		FJsonSerializer::Deserialize(Reader, Body);
		TestEqual(TEXT("device_id"), StringMember(Body, TEXT("device_id")), FString(TEXT("0f8fad5b-d9cb-469f-a165-70867728950e")));
		TestEqual(TEXT("flock_session_id"), StringMember(Body, TEXT("flock_session_id")), FString(FlockPlaytestFixtures::FirstFlockSessionId));
		TestEqual(TEXT("No steam_id when none is given"), StringMember(Body, TEXT("steam_id")), FString(TEXT("<absent>")));
		TestEqual(TEXT("No player_name when none is given"), StringMember(Body, TEXT("player_name")), FString(TEXT("<absent>")));
		const TSharedPtr<FJsonObject>* Debug = nullptr;
		if (TestTrue(TEXT("extra_debug is an object"), HasMemberSpelled(Body, TEXT("extra_debug")) && Body->TryGetObjectField(TEXT("extra_debug"), Debug) && Debug != nullptr))
		{
			TestEqual(TEXT("extra_debug carries the facts given"), StringMember(*Debug, TEXT("sdk_version")), FString(TEXT("1.13.0")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockProtokiteClientEndAcceptsNoContentTest,
	"Flock.Playtest.Client.EndAcceptsNoContent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockProtokiteClientEndAcceptsNoContentTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		FFlockHttpResponse Response;
		bool bEnds;
		const TCHAR* What;
	};
	const FCase Cases[] = {
		{ FFlockPlaytestFakeTransport::NoContent(), true, TEXT("204 with no body") },
		{ FFlockPlaytestFakeTransport::Status(200, TEXT("{}")), true, TEXT("200 with an empty object") },
		{ FFlockPlaytestFakeTransport::Status(200, TEXT("<html>captive portal</html>")), false, TEXT("A 200 that cannot be read") },
		{ FFlockPlaytestFakeTransport::Status(404, TEXT("{\"detail\":\"Playtest session not found\"}")), false, TEXT("HTTP 404") },
	};
	for (const FCase& Case : Cases)
	{
		FClientFixture Fixture;
		Fixture.Transport->Answer(FlockPlaytestFixtures::PlaytestSessionEndRoute, Case.Response);
		const FEndOutcome Outcome = EndSession(Fixture);
		RunPendingPlaytestRetries();

		TestTrue(FString::Printf(TEXT("%s: answered"), Case.What), Outcome->IsSet());
		TestEqual(FString::Printf(TEXT("%s: ended"), Case.What), Outcome->IsSet() && Outcome->GetValue().bSuccess, Case.bEnds);
		if (Fixture.Transport->Requests.Num() > 0)
		{
			const FFlockHttpRequest& Request = Fixture.Transport->Requests[0];
			TestEqual(FString::Printf(TEXT("%s: a POST"), Case.What), Request.Method, FString(TEXT("POST")));
			TestEqual(FString::Printf(TEXT("%s: to the session's end address"), Case.What), Request.Url,
				FString::Printf(TEXT("http://localhost:8020/game/sdk/playtest-session/%s/end"), FlockPlaytestFixtures::PlaytestSessionId));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockProtokiteClientSessionAddressesTest,
	"Flock.Playtest.Client.SessionAddressesEncodeTheId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockProtokiteClientSessionAddressesTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Start, trailing slashes removed"), FFlockProtokiteClient::MakePlaytestSessionStartUrl(TEXT("http://localhost:8020//")),
		FString(TEXT("http://localhost:8020/game/sdk/playtest-session")));
	TestEqual(TEXT("End, with the session id percent-encoded"), FFlockProtokiteClient::MakePlaytestSessionEndUrl(TEXT("http://localhost:8020/"), TEXT("a/b c")),
		FString(TEXT("http://localhost:8020/game/sdk/playtest-session/a%2Fb%20c/end")));
	return true;
}

#endif
