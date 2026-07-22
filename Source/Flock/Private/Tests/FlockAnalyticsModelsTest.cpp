// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Analytics/FlockAnalyticsJson.h"
#include "Http/FlockEndpoints.h"
#include "Http/FlockJsonUtils.h"
#include "Models/FlockAnalyticsModels.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsLogEventWireTest, "Flock.Analytics.Models.LogEventWire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsLogEventWireTest::RunTest(const FString& Parameters)
{
	// The three channels map to the wire spellings, not the enum names.
	TestEqual(TEXT("exception wire"), FFlockAnalyticsJson::LogEventTypeToWire(EFlockLogEventType::Exception), TEXT("exception"));
	TestEqual(TEXT("logic_error wire"), FFlockAnalyticsJson::LogEventTypeToWire(EFlockLogEventType::LogicError), TEXT("logic_error"));
	TestEqual(TEXT("debug wire"), FFlockAnalyticsJson::LogEventTypeToWire(EFlockLogEventType::Debug), TEXT("debug"));

	// A bare debug entry carries only what it must; every unset optional is absent, not blank.
	{
		FFlockLogEventRequest Event;
		Event.Message = TEXT("hello");
		Event.Data.Type = EFlockLogEventType::Debug;

		const FString Json = FFlockAnalyticsJson::SerializeEvent(Event);
		TestTrue(TEXT("message"), Json.Contains(TEXT("\"message\":\"hello\"")));
		// Case-sensitive: the wire spelling is lowercase and the enum name is not, so a default
		// (IgnoreCase) check would accept "Debug" and defeat the point of LogEventTypeToWire.
		TestTrue(TEXT("type"), Json.Contains(TEXT("\"type\":\"debug\""), ESearchCase::CaseSensitive));
		TestFalse(TEXT("no empty error_message"), Json.Contains(TEXT("error_message")));
		TestFalse(TEXT("no empty error_code"), Json.Contains(TEXT("error_code")));
		TestFalse(TEXT("no empty extra_data"), Json.Contains(TEXT("extra_data")));
		TestFalse(TEXT("no empty timestamp"), Json.Contains(TEXT("timestamp")));
		TestFalse(TEXT("no empty traceback lines"), Json.Contains(TEXT("error_traceback_lines")));
	}

	// A fully populated exception.
	{
		FFlockLogEventRequest Event;
		Event.Message = TEXT("boom");
		Event.Timestamp = TEXT("2026-07-21T00:00:00Z");
		Event.Data.Type = EFlockLogEventType::Exception;
		Event.Data.GameVersion = TEXT("1.2.3");
		Event.Data.ErrorMessage = TEXT("null deref");
		Event.Data.ErrorCode = TEXT("E42");
		Event.Data.ErrorTraceback = TEXT("at Foo()");
		Event.Data.ErrorTracebackLines = { TEXT("at Foo()"), TEXT("at Bar()") };

		const FString Json = FFlockAnalyticsJson::SerializeEvent(Event);
		TestTrue(TEXT("type exception"), Json.Contains(TEXT("\"type\":\"exception\""), ESearchCase::CaseSensitive));
		TestTrue(TEXT("game_version"), Json.Contains(TEXT("\"game_version\":\"1.2.3\"")));
		TestTrue(TEXT("error_message"), Json.Contains(TEXT("\"error_message\":\"null deref\"")));
		TestTrue(TEXT("error_code"), Json.Contains(TEXT("\"error_code\":\"E42\"")));
		TestTrue(TEXT("timestamp"), Json.Contains(TEXT("\"timestamp\":\"2026-07-21T00:00:00Z\"")));
		TestTrue(TEXT("traceback lines"), Json.Contains(TEXT("\"error_traceback_lines\":[\"at Foo()\",\"at Bar()\"]")));
	}

	// Batch body is {"events":[...]}.
	{
		FFlockLogEventRequest A;
		A.Message = TEXT("one");
		FFlockLogEventRequest B;
		B.Message = TEXT("two");

		const FString Json = FFlockAnalyticsJson::SerializeEvents({ A, B });
		TestTrue(TEXT("events array"), Json.StartsWith(TEXT("{\"events\":[")));
		TestTrue(TEXT("first"), Json.Contains(TEXT("\"message\":\"one\"")));
		TestTrue(TEXT("second"), Json.Contains(TEXT("\"message\":\"two\"")));
	}
	return true;
}

/**
 * The reason FFlockAnalyticsJson exists: caller-supplied map keys are game-authored and must survive
 * verbatim. Routing these through the generic snake_case exporter would ship `playerLevel` as
 * `player_level` and read it back as `PlayerLevel`.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsFreeFormKeysTest, "Flock.Analytics.Models.FreeFormKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsFreeFormKeysTest::RunTest(const FString& Parameters)
{
	FFlockLogEventRequest Event;
	Event.Message = TEXT("keys");
	Event.Data.Type = EFlockLogEventType::LogicError;
	Event.Data.ExtraData.Add(TEXT("playerLevel"), TEXT("7"));
	Event.Data.ExtraData.Add(TEXT("AlreadyPascal"), TEXT("x"));
	Event.Data.ErrorData.Add(TEXT("http_status"), TEXT("500"));

	// ESearchCase::CaseSensitive is mandatory here, not decoration: FString::Contains defaults to
	// IgnoreCase, so a default-comparison assertion cannot tell `playerLevel` from `playerlevel` and
	// would sit green through the exact corruption this test exists to catch.
	const FString Json = FFlockAnalyticsJson::SerializeEvent(Event);
	TestTrue(TEXT("camelCase key intact"),
		Json.Contains(TEXT("\"playerLevel\":\"7\""), ESearchCase::CaseSensitive));
	TestFalse(TEXT("not lowercased"), Json.Contains(TEXT("\"playerlevel\""), ESearchCase::CaseSensitive));
	TestFalse(TEXT("not snake_cased"), Json.Contains(TEXT("player_level")));
	TestTrue(TEXT("PascalCase key intact"),
		Json.Contains(TEXT("\"AlreadyPascal\":\"x\""), ESearchCase::CaseSensitive));
	TestTrue(TEXT("snake key intact"),
		Json.Contains(TEXT("\"http_status\":\"500\""), ESearchCase::CaseSensitive));

	// Round-trip: the spool reloads exactly what was queued.
	FFlockLogEventRequest Back;
	TestTrue(TEXT("deserializes"), FFlockAnalyticsJson::DeserializeEvent(Json, Back));
	TestEqual(TEXT("message"), Back.Message, TEXT("keys"));
	TestTrue(TEXT("type preserved"), Back.Data.Type == EFlockLogEventType::LogicError);

	// TMap<FString, ...>::Find is case-insensitive too, so the stored key has to be inspected
	// directly to prove the case survived rather than merely that a key matching loosely exists.
	const FString* Level = nullptr;
	for (const TPair<FString, FString>& Pair : Back.Data.ExtraData)
	{
		if (Pair.Key.Equals(TEXT("playerLevel"), ESearchCase::CaseSensitive))
		{
			Level = &Pair.Value;
		}
	}
	TestTrue(TEXT("camelCase key survives the round-trip byte-for-byte"), Level != nullptr);
	if (Level != nullptr)
	{
		TestEqual(TEXT("camelCase value"), *Level, TEXT("7"));
	}
	TestTrue(TEXT("error_data key found"), Back.Data.ErrorData.Contains(TEXT("http_status")));

	// Unknown/garbage type falls back to Debug instead of failing the flush.
	FFlockLogEventRequest Odd;
	TestTrue(TEXT("parses odd type"),
		FFlockAnalyticsJson::DeserializeEvent(TEXT("{\"message\":\"m\",\"data\":{\"type\":\"nonsense\"}}"), Odd));
	TestTrue(TEXT("falls back to debug"), Odd.Data.Type == EFlockLogEventType::Debug);

	// Missing `data` is malformed, not silently accepted.
	FFlockLogEventRequest Bad;
	TestFalse(TEXT("rejects missing data"), FFlockAnalyticsJson::DeserializeEvent(TEXT("{\"message\":\"m\"}"), Bad));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsSessionWireTest, "Flock.Analytics.Models.SessionWire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsSessionWireTest::RunTest(const FString& Parameters)
{
	// Start request: player_id required, unset optionals dropped.
	{
		FFlockSessionStartRequest Req;
		Req.PlayerId = TEXT("p-1");
		Req.Platform = TEXT("Windows");
		FString Json;
		TestTrue(TEXT("serializes"), FFlockJsonUtils::StructToWireJson(Req, Json, /*bOmitEmptyStrings*/ true));
		TestTrue(TEXT("player_id"), Json.Contains(TEXT("\"player_id\":\"p-1\"")));
		TestTrue(TEXT("platform"), Json.Contains(TEXT("\"platform\":\"Windows\"")));
		TestFalse(TEXT("no empty device_type"), Json.Contains(TEXT("device_type")));
		TestFalse(TEXT("no empty game_version_id"), Json.Contains(TEXT("game_version_id")));
	}

	// End request keeps its numeric members even at zero; only strings drop when empty.
	{
		FFlockSessionEndRequest Req;
		Req.DurationSeconds = 42;
		Req.ScreensViewed = 3;
		Req.IsBounce = true;
		Req.EndedAt = TEXT("2026-07-21T00:00:00Z");
		FString Json;
		TestTrue(TEXT("serializes"), FFlockJsonUtils::StructToWireJson(Req, Json, true));
		TestTrue(TEXT("duration_seconds"), Json.Contains(TEXT("\"duration_seconds\":42")));
		TestTrue(TEXT("screens_viewed"), Json.Contains(TEXT("\"screens_viewed\":3")));
		TestTrue(TEXT("is_bounce"), Json.Contains(TEXT("\"is_bounce\":true")));
		TestTrue(TEXT("ended_at"), Json.Contains(TEXT("\"ended_at\":\"2026-07-21T00:00:00Z\"")));
	}

	// The session-start response is BARE: the model sits at the root, not under `result`.
	// Reading it with the enveloped path is the failure this asserts against.
	{
		const FString Body = TEXT("{\"session_id\":\"s-9\"}");
		FFlockSessionStartResponse Raw;
		FString Error;
		TestTrue(TEXT("raw parse succeeds"), FFlockJsonUtils::WireJsonToStruct(Body, Raw, Error));
		TestEqual(TEXT("session_id"), Raw.SessionId, TEXT("s-9"));

		FFlockSessionStartResponse Enveloped;
		FString EnvelopeError;
		TestFalse(TEXT("enveloped parse rejects a bare body"),
			FFlockJsonUtils::UnwrapResultToStruct(Body, Enveloped, EnvelopeError));
	}

	// The fire-and-forget routes answer with a free-form object; an empty ack absorbs any of it.
	{
		FFlockAnalyticsAck Ack;
		FString Error;
		TestTrue(TEXT("ack absorbs arbitrary body"),
			FFlockJsonUtils::WireJsonToStruct(TEXT("{\"whatever\":1,\"nested\":{\"a\":2}}"), Ack, Error));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsEndpointsTest, "Flock.Analytics.Models.Endpoints",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsEndpointsTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("sessions"), FString(FlockEndpoints::AnalyticsSessions), TEXT("analytics/sessions"));
	TestEqual(TEXT("session by id"), FlockEndpoints::AnalyticsSessionById(TEXT("s-1")), TEXT("analytics/sessions/s-1"));
	TestEqual(TEXT("log_event batch"), FString(FlockEndpoints::LogEvent), TEXT("log_event"));
	TestEqual(TEXT("log_event single"), FString(FlockEndpoints::LogEventSingle), TEXT("log_event/single"));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
