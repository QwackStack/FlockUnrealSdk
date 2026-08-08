// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Http/FlockJsonUtils.h"
#include "Models/FlockGameModels.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockJsonCaseRoundTripTest, "Flock.Http.Json.CaseRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockJsonCaseRoundTripTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("snake -> pascal"), FFlockJsonUtils::SnakeToPascal(TEXT("game_version_id")), FString(TEXT("GameVersionId")));
	TestEqual(TEXT("single word snake -> pascal"), FFlockJsonUtils::SnakeToPascal(TEXT("id")), FString(TEXT("Id")));
	TestEqual(TEXT("pascal -> snake"), FFlockJsonUtils::ToSnakeCase(TEXT("GameVersionId")), FString(TEXT("game_version_id")));
	TestEqual(TEXT("camel -> snake"), FFlockJsonUtils::ToSnakeCase(TEXT("releaseType")), FString(TEXT("release_type")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockJsonEnvelopeUnwrapTest, "Flock.Http.Json.EnvelopeUnwrap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

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
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

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
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

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
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockJsonArrayUnwrapTest, "Flock.Http.Json.ArrayUnwrap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockJsonArrayUnwrapTest::RunTest(const FString& Parameters)
{
	// The enveloped-list shape: `result` is a bare array, the shape the config/patch list routes answer
	// with. Each element goes through the same per-element parse, so snake keys still map to Pascal.
	const FString Body = TEXT("{\"result\":[{\"id\":\"a\",\"release_type\":\"prod\"},{\"id\":\"b\"}]}");
	TArray<FFlockGameVersionSchema> Items;
	FString Error;
	TestTrue(TEXT("array unwrap succeeds"), FFlockJsonUtils::UnwrapResultToArray(Body, Items, Error));
	TestEqual(TEXT("two items"), Items.Num(), 2);
	if (Items.Num() == 2)
	{
		TestEqual(TEXT("item 0 id"), Items[0].Id, FString(TEXT("a")));
		TestEqual(TEXT("item 0 release_type mapped"), Items[0].ReleaseType, FString(TEXT("prod")));
		TestEqual(TEXT("item 1 id"), Items[1].Id, FString(TEXT("b")));
	}

	// An empty result array is a valid, empty list — not a failure.
	TArray<FFlockGameVersionSchema> Empty;
	FString EmptyError;
	TestTrue(TEXT("empty array succeeds"), FFlockJsonUtils::UnwrapResultToArray(TEXT("{\"result\":[]}"), Empty, EmptyError));
	TestEqual(TEXT("no items"), Empty.Num(), 0);

	// Missing result, a non-array result (the object shape), a non-object element, and malformed JSON all fail.
	TArray<FFlockGameVersionSchema> Bad;
	FString BadError;
	TestFalse(TEXT("missing result fails"),
		FFlockJsonUtils::UnwrapResultToArray(TEXT("{\"error\":{\"code\":\"x\"}}"), Bad, BadError));
	TestFalse(TEXT("object result (not array) fails"),
		FFlockJsonUtils::UnwrapResultToArray(TEXT("{\"result\":{\"id\":\"a\"}}"), Bad, BadError));
	TestFalse(TEXT("non-object element fails"),
		FFlockJsonUtils::UnwrapResultToArray(TEXT("{\"result\":[\"nope\"]}"), Bad, BadError));
	TestFalse(TEXT("malformed body fails"),
		FFlockJsonUtils::UnwrapResultToArray(TEXT("not json"), Bad, BadError));

	return true;
}

namespace
{
	// A type declaring a static FromWireObject: proves HasCustomWireParse detects the custom path. Plain
	// struct on purpose — the detector only inspects the static member, no USTRUCT reflection needed.
	struct FFlockWireParseProbe
	{
		static bool FromWireObject(const TSharedRef<FJsonObject>&, FFlockWireParseProbe&, FString&) { return true; }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockJsonWireParseDetectionTest, "Flock.Http.Json.WireParseDetection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockJsonWireParseDetectionTest::RunTest(const FString& Parameters)
{
	// A type with a static FromWireObject is routed to its own parse; a reflection model is not. This is
	// the switch that keeps config data out of TransformObjectKeys (which would rewrite dictionary keys).
	TestTrue(TEXT("probe with FromWireObject detected"), FFlockJsonUtils::HasCustomWireParse<FFlockWireParseProbe>());
	TestFalse(TEXT("reflection model not detected"), FFlockJsonUtils::HasCustomWireParse<FFlockGameVersionSchema>());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockJsonPlainRoundTripTest, "Flock.Http.Json.PlainRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockJsonPlainRoundTripTest::RunTest(const FString& Parameters)
{
	// The snapshot round-trip is symmetric plain reflection (PascalCase, no snake transform): what goes in
	// comes back unchanged, which is what lets a persisted model be re-read verbatim on the next launch.
	FFlockGameVersionSchema In;
	In.Id = TEXT("ver-1");
	In.Name = TEXT("1.2.3");
	In.ReleaseType = TEXT("prod");

	FString Json;
	TestTrue(TEXT("struct serializes"), FFlockJsonUtils::StructToPlainJson(In, Json));
	// Plain form keeps PascalCase field names — it is not the wire form.
	TestTrue(TEXT("plain keys are PascalCase"), Json.Contains(TEXT("\"Id\":\"ver-1\"")));
	TestFalse(TEXT("no snake keys in plain form"), Json.Contains(TEXT("release_type")));

	FFlockGameVersionSchema Out;
	TestTrue(TEXT("struct round-trips"), FFlockJsonUtils::PlainJsonToStruct(Json, Out));
	TestEqual(TEXT("id preserved"), Out.Id, In.Id);
	TestEqual(TEXT("name preserved"), Out.Name, In.Name);
	TestEqual(TEXT("release type preserved"), Out.ReleaseType, In.ReleaseType);

	// Array form for a list snapshot.
	TArray<FFlockGameVersionSchema> List;
	List.Add(In);
	FFlockGameVersionSchema Second;
	Second.Id = TEXT("ver-2");
	List.Add(Second);

	FString ArrayJson;
	TestTrue(TEXT("array serializes"), FFlockJsonUtils::ArrayToPlainJson(List, ArrayJson));
	TestTrue(TEXT("array is an array"), ArrayJson.StartsWith(TEXT("[")));

	TArray<FFlockGameVersionSchema> RoundTripped;
	TestTrue(TEXT("array round-trips"), FFlockJsonUtils::ArrayFromPlainJson(ArrayJson, RoundTripped));
	TestEqual(TEXT("two items back"), RoundTripped.Num(), 2);
	if (RoundTripped.Num() == 2)
	{
		TestEqual(TEXT("item 0 id"), RoundTripped[0].Id, FString(TEXT("ver-1")));
		TestEqual(TEXT("item 1 id"), RoundTripped[1].Id, FString(TEXT("ver-2")));
	}

	// A non-array body is rejected by the array reader.
	TArray<FFlockGameVersionSchema> Bad;
	TestFalse(TEXT("object rejected as array"), FFlockJsonUtils::ArrayFromPlainJson(TEXT("{\"Id\":\"x\"}"), Bad));

	return true;
}

/**
 * The two properties of JSON key handling that the SDK relies on everywhere and that no engine
 * documents as a contract: names come back in the author's spelling, and lookup ignores case.
 *
 * Worth pinning because the container behind them is *not* stable across engines - later engines key
 * FJsonObject by an interned shared string rather than FString. Both currently hash case-insensitively
 * (the engine's own comment on the shared-string hash says it must match FString's), and both preserve
 * the stored spelling. If a future engine changes either, `FFlockStructuredData`'s exact-then-Pascal
 * resolution silently starts resolving differently, and every dotted-path getter in the SDK is affected.
 * This test is what turns that into a red line instead of a support ticket.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockJsonKeySemanticsTest, "Flock.Http.Json.KeySemantics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockJsonKeySemanticsTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Object;
	TestTrue(TEXT("parses"), FFlockJsonUtils::TryParseObject(
		TEXT("{\"max_health\":10,\"GameCurrencies\":{\"gold\":1},\"MixedCase\":\"v\"}"), Object));

	// Enumeration returns author spelling verbatim -- not lower-cased, not Pascal-ised. Codegen and the
	// commands surface both write keys back to the server using exactly what this returns.
	const TArray<FString> Names = FFlockJsonUtils::GetFieldNames(Object);
	TestEqual(TEXT("three field names"), Names.Num(), 3);
	TestTrue(TEXT("snake key kept verbatim"), Names.Contains(TEXT("max_health")));
	TestTrue(TEXT("pascal key kept verbatim"), Names.Contains(TEXT("GameCurrencies")));
	TestTrue(TEXT("mixed-case key kept verbatim"), Names.Contains(TEXT("MixedCase")));

	// Lookup ignores case, so a caller's spelling need not match the dashboard's.
	TestTrue(TEXT("exact lookup"), Object->HasField(TEXT("max_health")));
	TestTrue(TEXT("case-insensitive lookup"), Object->HasField(TEXT("MAX_HEALTH")));
	TestTrue(TEXT("case-insensitive lookup, other direction"), Object->HasField(TEXT("mixedcase")));
	TestFalse(TEXT("absent field is absent"), Object->HasField(TEXT("no_such_field")));

	// TryGetField agrees with HasField -- the SDK uses them interchangeably for presence plus value.
	TestTrue(TEXT("TryGetField finds it case-insensitively"), Object->TryGetField(TEXT("GAMECURRENCIES")).IsValid());
	TestFalse(TEXT("TryGetField misses an absent field"), Object->TryGetField(TEXT("nope")).IsValid());

	// A null object enumerates to nothing rather than crashing; several callers pass an unresolved object.
	TestEqual(TEXT("null object yields no names"), FFlockJsonUtils::GetFieldNames(nullptr).Num(), 0);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
