// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockLogger.h"
#include "ProtokiteClient.h"
#include "Http/FlockHttpClient.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/ProtokitePlaytestFakeTransport.h"
#include "Tests/ProtokitePlaytestTestSupport.h"

namespace
{
	struct FClientFixture
	{
		TSharedRef<FProtokitePlaytestFakeTransport> Transport = MakeShared<FProtokitePlaytestFakeTransport>();
		TSharedPtr<FProtokiteClient> Client;
		TSharedRef<TOptional<TFlockResult<FProtokitePlaytestConfig>>> Result = MakeShared<TOptional<TFlockResult<FProtokitePlaytestConfig>>>();

		FClientFixture()
		{
			const TSharedRef<IFlockLogger> Logger = MakeShared<FFlockNullLogger>();
			FFlockRetryPolicy Policy;
			Policy.MaxRetries = 3;
			Policy.InitialDelaySeconds = 0.f;
			Policy.bUseJitter = false;
			Client = MakeShared<FProtokiteClient>(MakeShared<FFlockHttpClient>(Transport, Logger), Policy, Logger);
		}

		void Fetch()
		{
			TMap<FString, FString> Headers;
			Headers.Add(TEXT("X-Flock-API-Key"), TEXT("secret"));
			Headers.Add(TEXT("X-Game-Version-ID"), ProtokitePlaytestFixtures::GameVersionId);
			const TSharedRef<TOptional<TFlockResult<FProtokitePlaytestConfig>>> Out = Result;
			Client->FetchPlaytestConfig(TEXT("http://localhost:8020"), Headers,
				[Out](TFlockResult<FProtokitePlaytestConfig> Answer) { *Out = Answer; });
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokiteClientJoinsUrlTest,
	"Protokite.Playtest.Client.JoinsUrlWithOrWithoutTrailingSlash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokiteClientJoinsUrlTest::RunTest(const FString& Parameters)
{
	const FString Expected = TEXT("http://localhost:8020/game/sdk/playtest-config");
	TestEqual(TEXT("No trailing slash"), FProtokiteClient::MakePlaytestConfigUrl(TEXT("http://localhost:8020")), Expected);
	TestEqual(TEXT("One trailing slash"), FProtokiteClient::MakePlaytestConfigUrl(TEXT("http://localhost:8020/")), Expected);
	TestEqual(TEXT("Two trailing slashes"), FProtokiteClient::MakePlaytestConfigUrl(TEXT("http://localhost:8020//")), Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokiteClientSendsWhatItIsGivenTest,
	"Protokite.Playtest.Client.SendsTheHeadersItIsGiven",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokiteClientSendsWhatItIsGivenTest::RunTest(const FString& Parameters)
{
	FClientFixture Fixture;
	Fixture.Transport->Answer(ProtokitePlaytestFixtures::PlaytestConfigRoute,
		FProtokitePlaytestFakeTransport::Status(200, ProtokitePlaytestFixtures::ConfigBody()));
	Fixture.Fetch();

	if (TestEqual(TEXT("One request"), Fixture.Transport->Requests.Num(), 1))
	{
		const FFlockHttpRequest& Request = Fixture.Transport->RequestAt(0);
		TestEqual(TEXT("A GET"), Request.Method, FString(TEXT("GET")));
		TestEqual(TEXT("To the playtest-config route"), Request.Url, FString(TEXT("http://localhost:8020/game/sdk/playtest-config")));
		TestEqual(TEXT("With the API key given"), Request.Headers.FindRef(TEXT("X-Flock-API-Key")), FString(TEXT("secret")));
		TestEqual(TEXT("With the version id given"), Request.Headers.FindRef(TEXT("X-Game-Version-ID")),
			FString(ProtokitePlaytestFixtures::GameVersionId));
	}
	TestTrue(TEXT("The config is loaded"), Fixture.Result->IsSet() && Fixture.Result->GetValue().bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokiteClientRefusalsAreNotRetriedTest,
	"Protokite.Playtest.Client.RefusalsAreNotRetried",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokiteClientRefusalsAreNotRetriedTest::RunTest(const FString& Parameters)
{
	struct FRefusal
	{
		int32 StatusCode;
		const TCHAR* Body;
	};
	const FRefusal Refusals[] = {
		{ 404, ProtokitePlaytestFixtures::NotLinkedBody },
		{ 401, ProtokitePlaytestFixtures::InvalidApiKeyBody },
		{ 422, ProtokitePlaytestFixtures::MissingApiKeyBody },
	};
	for (const FRefusal& Refusal : Refusals)
	{
		FClientFixture Fixture;
		Fixture.Transport->Answer(ProtokitePlaytestFixtures::PlaytestConfigRoute,
			FProtokitePlaytestFakeTransport::Status(Refusal.StatusCode, Refusal.Body));
		Fixture.Fetch();
		RunPendingPlaytestRetries();

		TestEqual(FString::Printf(TEXT("HTTP %d is asked once"), Refusal.StatusCode), Fixture.Transport->Requests.Num(), 1);
		TestTrue(FString::Printf(TEXT("HTTP %d fails with its status"), Refusal.StatusCode),
			Fixture.Result->IsSet() && !Fixture.Result->GetValue().bSuccess
			&& Fixture.Result->GetValue().Error.StatusCode == Refusal.StatusCode);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokiteClientFailuresAreRetriedTest,
	"Protokite.Playtest.Client.FailuresAreRetried",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokiteClientFailuresAreRetriedTest::RunTest(const FString& Parameters)
{
	const FFlockHttpResponse Failures[] = {
		FProtokitePlaytestFakeTransport::Status(503, ProtokitePlaytestFixtures::FlockUnreachableBody),
		FProtokitePlaytestFakeTransport::ConnectionFailure(),
	};
	for (const FFlockHttpResponse& Failure : Failures)
	{
		FClientFixture Fixture;
		Fixture.Transport->AnswerInOrder(ProtokitePlaytestFixtures::PlaytestConfigRoute,
			{ Failure, FProtokitePlaytestFakeTransport::Status(200, ProtokitePlaytestFixtures::ConfigBody()) });
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
	using FStartOutcome = TSharedRef<TOptional<TFlockResult<FProtokitePlaytestSessionStartResult>>>;
	using FEndOutcome = TSharedRef<TOptional<TFlockResult<FProtokitePlaytestSessionEndResult>>>;

	TMap<FString, FString> SessionTestHeaders()
	{
		TMap<FString, FString> Headers;
		Headers.Add(TEXT("X-Flock-API-Key"), TEXT("secret"));
		Headers.Add(TEXT("X-Game-Version-ID"), ProtokitePlaytestFixtures::GameVersionId);
		return Headers;
	}

	/** Starts a session through the fixture's client. The outcome stays unset while no answer has arrived. */
	FStartOutcome StartSession(FClientFixture& Fixture)
	{
		FProtokitePlaytestSessionStartRequest Request;
		Request.Identity.DeviceId = TEXT("0f8fad5b-d9cb-469f-a165-70867728950e");
		Request.FlockSessionId = ProtokitePlaytestFixtures::FirstFlockSessionId;
		Request.DebugInfo.Add(TEXT("sdk_version"), TEXT("1.13.0"));

		const FStartOutcome Outcome = MakeShared<TOptional<TFlockResult<FProtokitePlaytestSessionStartResult>>>();
		Fixture.Client->StartPlaytestSession(TEXT("http://localhost:8020"), SessionTestHeaders(), Request,
			[Outcome](TFlockResult<FProtokitePlaytestSessionStartResult> Answer) { *Outcome = Answer; });
		return Outcome;
	}

	/** Ends the fixture's session through its client. The outcome stays unset while no answer has arrived. */
	FEndOutcome EndSession(FClientFixture& Fixture)
	{
		const FEndOutcome Outcome = MakeShared<TOptional<TFlockResult<FProtokitePlaytestSessionEndResult>>>();
		Fixture.Client->EndPlaytestSession(TEXT("http://localhost:8020"), SessionTestHeaders(), ProtokitePlaytestFixtures::PlaytestSessionId,
			[Outcome](TFlockResult<FProtokitePlaytestSessionEndResult> Answer) { *Outcome = Answer; });
		return Outcome;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokiteClientStartIsSentOnceWhileEndIsRetriedTest,
	"Protokite.Playtest.Client.StartIsSentOnceWhileEndIsRetried",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokiteClientStartIsSentOnceWhileEndIsRetriedTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		FFlockHttpResponse Response;
		const TCHAR* What;
	};
	const FCase Cases[] = {
		{ FProtokitePlaytestFakeTransport::ConnectionFailure(), TEXT("No connection") },
		{ FProtokitePlaytestFakeTransport::TimedOut(), TEXT("A timeout") },
		{ FProtokitePlaytestFakeTransport::Status(503, ProtokitePlaytestFixtures::FlockUnreachableBody), TEXT("HTTP 503") },
		{ FProtokitePlaytestFakeTransport::Status(429, TEXT("{}")), TEXT("HTTP 429") },
		{ FProtokitePlaytestFakeTransport::Status(408, TEXT("{}")), TEXT("HTTP 408") },
	};
	for (const FCase& Case : Cases)
	{
		FClientFixture Fixture;
		Fixture.Transport->Answer(ProtokitePlaytestFixtures::PlaytestSessionStartRoute, Case.Response);
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
		Fixture.Transport->AnswerInOrder(ProtokitePlaytestFixtures::PlaytestSessionEndRoute, {
			FProtokitePlaytestFakeTransport::Status(503, ProtokitePlaytestFixtures::FlockUnreachableBody), FProtokitePlaytestFakeTransport::NoContent() });
		const FEndOutcome Outcome = EndSession(Fixture);
		RunPendingPlaytestRetries();
		TestEqual(TEXT("An end that failed is sent again"), Fixture.Transport->Requests.Num(), 2);
		TestTrue(TEXT("And succeeds on the retry"), Outcome->IsSet() && Outcome->GetValue().bSuccess);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokiteClientStartNeedsASessionIdTest,
	"Protokite.Playtest.Client.StartNeedsASessionId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokiteClientStartNeedsASessionIdTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		FString Body;
		bool bStarts;
		const TCHAR* What;
	};
	const FCase Cases[] = {
		{ ProtokitePlaytestFixtures::SessionStartBody(), true, TEXT("A session id") },
		{ ProtokitePlaytestFixtures::Envelope(TEXT("{}")), false, TEXT("No session id") },
		{ ProtokitePlaytestFixtures::SessionStartBody(TEXT("")), false, TEXT("An empty session id") },
		{ ProtokitePlaytestFixtures::SessionStartBody(FString(ProtokitePlaytestFixtures::PlaytestSessionId) + TEXT(" ")), false, TEXT("A session id with a space") },
	};
	for (const FCase& Case : Cases)
	{
		FClientFixture Fixture;
		Fixture.Transport->Answer(ProtokitePlaytestFixtures::PlaytestSessionStartRoute, FProtokitePlaytestFakeTransport::Status(200, Case.Body));
		const FStartOutcome Outcome = StartSession(Fixture);

		if (TestTrue(FString::Printf(TEXT("%s: answered"), Case.What), Outcome->IsSet()))
		{
			TestEqual(FString::Printf(TEXT("%s: started"), Case.What), Outcome->GetValue().bSuccess, Case.bStarts);
			if (Case.bStarts)
			{
				TestEqual(TEXT("The session id is read"), Outcome->GetValue().Value.SessionId, FString(ProtokitePlaytestFixtures::PlaytestSessionId));
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokiteClientStartSendsTheRequestItIsGivenTest,
	"Protokite.Playtest.Client.StartSendsTheRequestItIsGiven",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokiteClientStartSendsTheRequestItIsGivenTest::RunTest(const FString& Parameters)
{
	FClientFixture Fixture;
	Fixture.Transport->Answer(ProtokitePlaytestFixtures::PlaytestSessionStartRoute,
		FProtokitePlaytestFakeTransport::Status(200, ProtokitePlaytestFixtures::SessionStartBody()));
	StartSession(Fixture);

	if (TestEqual(TEXT("One request"), Fixture.Transport->Requests.Num(), 1))
	{
		const FFlockHttpRequest& Request = Fixture.Transport->RequestAt(0);
		TestEqual(TEXT("A POST"), Request.Method, FString(TEXT("POST")));
		TestEqual(TEXT("To the session route"), Request.Url, FString(TEXT("http://localhost:8020/game/sdk/playtest-session")));
		TestEqual(TEXT("With the API key given"), Request.Headers.FindRef(TEXT("X-Flock-API-Key")), FString(TEXT("secret")));

		TSharedPtr<FJsonObject> Body;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Request.JsonBody);
		FJsonSerializer::Deserialize(Reader, Body);
		TestEqual(TEXT("device_id"), StringMember(Body, TEXT("device_id")), FString(TEXT("0f8fad5b-d9cb-469f-a165-70867728950e")));
		TestEqual(TEXT("flock_session_id"), StringMember(Body, TEXT("flock_session_id")), FString(ProtokitePlaytestFixtures::FirstFlockSessionId));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokiteClientEndAcceptsNoContentTest,
	"Protokite.Playtest.Client.EndAcceptsNoContent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokiteClientEndAcceptsNoContentTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		FFlockHttpResponse Response;
		bool bEnds;
		const TCHAR* What;
	};
	const FCase Cases[] = {
		{ FProtokitePlaytestFakeTransport::NoContent(), true, TEXT("204 with no body") },
		{ FProtokitePlaytestFakeTransport::Status(200, TEXT("{}")), true, TEXT("200 with an empty object") },
		{ FProtokitePlaytestFakeTransport::Status(200, TEXT("<html>captive portal</html>")), false, TEXT("A 200 that cannot be read") },
		{ FProtokitePlaytestFakeTransport::Status(404, TEXT("{\"detail\":\"Playtest session not found\"}")), false, TEXT("HTTP 404") },
	};
	for (const FCase& Case : Cases)
	{
		FClientFixture Fixture;
		Fixture.Transport->Answer(ProtokitePlaytestFixtures::PlaytestSessionEndRoute, Case.Response);
		const FEndOutcome Outcome = EndSession(Fixture);
		RunPendingPlaytestRetries();

		TestTrue(FString::Printf(TEXT("%s: answered"), Case.What), Outcome->IsSet());
		TestEqual(FString::Printf(TEXT("%s: ended"), Case.What), Outcome->IsSet() && Outcome->GetValue().bSuccess, Case.bEnds);
		if (Fixture.Transport->Requests.Num() > 0)
		{
			const FFlockHttpRequest& Request = Fixture.Transport->RequestAt(0);
			TestEqual(FString::Printf(TEXT("%s: a POST"), Case.What), Request.Method, FString(TEXT("POST")));
			TestEqual(FString::Printf(TEXT("%s: to the session's end address"), Case.What), Request.Url,
				FString::Printf(TEXT("http://localhost:8020/game/sdk/playtest-session/%s/end"), ProtokitePlaytestFixtures::PlaytestSessionId));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokiteClientSessionAddressesTest,
	"Protokite.Playtest.Client.SessionAddressesEncodeTheId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokiteClientSessionAddressesTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Start, trailing slashes removed"), FProtokiteClient::MakePlaytestSessionStartUrl(TEXT("http://localhost:8020//")),
		FString(TEXT("http://localhost:8020/game/sdk/playtest-session")));
	TestEqual(TEXT("End, with the session id percent-encoded"), FProtokiteClient::MakePlaytestSessionEndUrl(TEXT("http://localhost:8020/"), TEXT("a/b c")),
		FString(TEXT("http://localhost:8020/game/sdk/playtest-session/a%2Fb%20c/end")));
	return true;
}

/**
 * Protokite's refusals carry no Flock error code, and the Flock SDK reads an answer without one as coming from somewhere
 * else: a failed Protokite call must say what Protokite's refusal means and name the playtest's settings instead. Driven
 * through a real call, so a call that hands its failure over without Protokite's fix fails here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokiteClientGivesProtokitesFixTest,
	"Protokite.Playtest.Client.AFailureSaysWhatProtokiteMeantAndHowToFixIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokiteClientGivesProtokitesFixTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		const TCHAR* What;
		FFlockHttpResponse Answer;
		const TCHAR* FixSays;
	};
	// The refusals measured on Protokite's SDK routes (2026-09-14), and two answers that are not Protokite's.
	const FCase Cases[] = {
		{ TEXT("A key Protokite does not know"), FProtokitePlaytestFakeTransport::Status(401, TEXT("{\"detail\":\"Invalid API Key\"}")),
			TEXT("Protokite did not accept this build's Flock API key") },
		{ TEXT("A build no playtest is linked to"), FProtokitePlaytestFakeTransport::Status(404, TEXT("{\"detail\":\"No playtest is linked to this Flock SDK version\"}")),
			TEXT("link the playtest in the Protokite dashboard to the Game Version this build runs") },
		{ TEXT("A captive portal's page"), FProtokitePlaytestFakeTransport::Status(200, TEXT("<html>Sign in to the Wi-Fi</html>")),
			TEXT("The answer did not come from Protokite's API") },
		{ TEXT("A proxy's refusal"), FProtokitePlaytestFakeTransport::Status(403, TEXT("<html>Forbidden</html>")),
			TEXT("The answer did not come from Protokite's API") },
	};
	for (const FCase& Case : Cases)
	{
		FClientFixture Fixture;
		Fixture.Transport->Answer(ProtokitePlaytestFixtures::PlaytestConfigRoute, Case.Answer);
		Fixture.Fetch();
		RunPendingPlaytestRetries();
		if (!TestTrue(FString::Printf(TEXT("%s: failed"), Case.What), Fixture.Result->IsSet() && !Fixture.Result->GetValue().bSuccess))
		{
			continue;
		}
		const FFlockError& Error = Fixture.Result->GetValue().Error;
		TestTrue(FString::Printf(TEXT("%s: the fix says what Protokite meant (%s)"), Case.What, *Error.Hint),
			Error.Hint.Contains(Case.FixSays, ESearchCase::CaseSensitive));
		TestFalse(FString::Printf(TEXT("%s: and never sends the developer to the Flock SDK's API URL"), Case.What),
			Error.Hint.Contains(TEXT("Flock's game API"), ESearchCase::CaseSensitive)
			|| Error.Hint.Contains(TEXT("API URL under Project Settings > Flock SDK"), ESearchCase::CaseSensitive));
	}
	return true;
}

/** Every refusal Protokite is measured to send has a fix of its own, and none of them reads as the Flock SDK's. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokiteClientFixForEveryRefusalTest,
	"Protokite.Playtest.Client.EveryRefusalProtokiteSendsHasItsOwnFix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokiteClientFixForEveryRefusalTest::RunTest(const FString& Parameters)
{
	const auto Refusal = [](int32 Status, const TCHAR* Reason)
	{
		return FFlockError::Make(Status == 401 || Status == 403 ? EFlockErrorType::Auth : EFlockErrorType::Network,
			Reason, Status, FString(), FString(), Reason);
	};
	struct FCase
	{
		FFlockError Error;
		const TCHAR* FixSays;
	};
	const FCase Cases[] = {
		{ Refusal(400, TEXT("This playtest is no longer collecting SDK sessions")), TEXT("start a new playtest in the Protokite dashboard") },
		{ Refusal(401, TEXT("Invalid API Key")), TEXT("Set API Key under Project Settings > Flock SDK") },
		{ Refusal(403, TEXT("Not your playtest session")), TEXT("Protokite holds this playtest session for another game") },
		{ Refusal(404, TEXT("No published feedback form")), TEXT("publish the playtest's form in the Protokite dashboard") },
		{ Refusal(404, TEXT("Playtest session not found")), TEXT("the next launch starts another") },
		{ Refusal(422, TEXT("Missing required answer 'steps'")), TEXT("A form answer it names does not fit the form as published now") },
		{ Refusal(503, TEXT("Could not reach Flock to verify SDK key")), TEXT("Protokite, or Flock behind it, failed") },
		{ FFlockError::Make(EFlockErrorType::Connection, TEXT("The request could not reach the server")),
			TEXT("Protokite API URL in Project Settings > Plugins > Protokite Playtest Settings") },
		{ FFlockError::Make(EFlockErrorType::Timeout, TEXT("The server did not answer in time")), TEXT("can reach Protokite") },
	};
	for (const FCase& Case : Cases)
	{
		const FString Fix = FProtokiteClient::FixFor(Case.Error);
		TestTrue(FString::Printf(TEXT("HTTP %d %s: %s"), Case.Error.StatusCode, *Case.Error.Message, *Fix),
			Fix.Contains(Case.FixSays, ESearchCase::CaseSensitive));
		TestFalse(FString::Printf(TEXT("HTTP %d %s: not the Flock SDK's fix"), Case.Error.StatusCode, *Case.Error.Message),
			Fix.Contains(TEXT("Flock's game API"), ESearchCase::CaseSensitive));
	}
	return true;
}

#endif
