// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Http/FlockHttpClient.h"
#include "Http/FlockResult.h"
#include "Models/FlockGameModels.h"
#include "FlockLogger.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockRecordingLogger.h"

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


// ── Every call is traced, so a graph that does nothing visible can still be accounted for ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockHttpClientTraceTest, "Flock.Http.Client.TracesEveryCall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockHttpClientTraceTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
	const TSharedRef<FFlockRecordingLogger> Log = MakeShared<FFlockRecordingLogger>();
	const TSharedRef<FFlockHttpClient> Client = MakeShared<FFlockHttpClient>(Fake, Log);

	Fake->On(TEXT("version"), FFlockFakeTransport::Status(200, TEXT("{\"result\":{\"id\":\"v1\"}}")));
	Client->Get<FFlockGameVersionSchema>(TEXT("http://x/version"), {},
		[](TFlockResult<FFlockGameVersionSchema>) {});

	// Both halves. The request line alone was already there and is not the gap — a call that went out and
	// never came back looks identical to one that succeeded without it.
	TestTrue(TEXT("the request is traced"),
		FFlockRecordingLogger::AnyContains(Log->Debugs, TEXT("-> GET http://x/version")));
	TestTrue(TEXT("and so is the response"),
		FFlockRecordingLogger::AnyContains(Log->Debugs, TEXT("<- 200 GET http://x/version")));
	TestEqual(TEXT("a success stays out of the warning channel"), Log->Warnings.Num(), 0);

	// A failure is a warning, not a debug line: whoever is chasing one has usually not enabled debug
	// logging yet, which is exactly why they cannot see what went wrong.
	const TSharedRef<FFlockRecordingLogger> FailLog = MakeShared<FFlockRecordingLogger>();
	const TSharedRef<FFlockHttpClient> FailClient = MakeShared<FFlockHttpClient>(Fake, FailLog);
	Fake->On(TEXT("boom"), FFlockFakeTransport::Status(422, TEXT("{\"error\":\"template_validation_failed\"}")));
	FailClient->Get<FFlockGameVersionSchema>(TEXT("http://x/boom"), {},
		[](TFlockResult<FFlockGameVersionSchema>) {});

	TestTrue(TEXT("a failed call warns"),
		FFlockRecordingLogger::AnyContains(FailLog->Warnings, TEXT("<- 422 GET http://x/boom")));
	// The server's coded reason is the most useful line in the trace, so it must survive into the log.
	TestTrue(TEXT("and carries the server's reason"),
		FFlockRecordingLogger::AnyContains(FailLog->Warnings, TEXT("template_validation_failed")));

	// An unreachable server is distinguishable from a rejection — different fix, so different wording.
	const TSharedRef<FFlockRecordingLogger> OfflineLog = MakeShared<FFlockRecordingLogger>();
	const TSharedRef<FFlockHttpClient> OfflineClient = MakeShared<FFlockHttpClient>(Fake, OfflineLog);
	Fake->On(TEXT("gone"), FFlockFakeTransport::Offline());
	OfflineClient->Get<FFlockGameVersionSchema>(TEXT("http://x/gone"), {},
		[](TFlockResult<FFlockGameVersionSchema>) {});

	TestTrue(TEXT("an unreachable server says so"),
		FFlockRecordingLogger::AnyContains(OfflineLog->Warnings, TEXT("failed to reach the server")));

	return true;
}

// ── The trace names its caller, on both halves, and cannot be misattributed by a later call ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockHttpClientOriginTest, "Flock.Http.Client.TraceNamesTheCaller",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockHttpClientOriginTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
	const TSharedRef<FFlockRecordingLogger> Log = MakeShared<FFlockRecordingLogger>();
	const TSharedRef<FFlockHttpClient> Client = MakeShared<FFlockHttpClient>(Fake, Log);
	Fake->On(TEXT("thing"), FFlockFakeTransport::Status(200, TEXT("{\"result\":{\"id\":\"v1\"}}")));

	// Default attribution, for a call the SDK makes on its own behalf (a token refresh, a session ping).
	Client->Get<FFlockGameVersionSchema>(TEXT("http://x/thing"), {}, [](TFlockResult<FFlockGameVersionSchema>) {});
	TestTrue(TEXT("an SDK-internal call is attributed to C++"),
		FFlockRecordingLogger::AnyContains(Log->Debugs, TEXT("[C++]")));

	// What a Blueprint node's origin scope publishes. The provider sets this; the HTTP layer sits below
	// the provider and can only see it because it is ambient.
	FFlockCallOrigin::Set(TEXT("Blueprint 'bpTest'"));
	const TSharedRef<FFlockRecordingLogger> BpLog = MakeShared<FFlockRecordingLogger>();
	const TSharedRef<FFlockHttpClient> BpClient = MakeShared<FFlockHttpClient>(Fake, BpLog);
	BpClient->Get<FFlockGameVersionSchema>(TEXT("http://x/thing"), {}, [](TFlockResult<FFlockGameVersionSchema>) {});

	// Both halves, or a failing call names its caller on the line that says it succeeded and not on the
	// one that says it failed.
	TestTrue(TEXT("the request names the graph"),
		FFlockRecordingLogger::AnyContains(BpLog->Debugs, TEXT("-> GET http://x/thing [Blueprint 'bpTest']")));
	TestTrue(TEXT("and so does the response"),
		FFlockRecordingLogger::AnyContains(BpLog->Debugs, TEXT("<- 200 GET http://x/thing (")));
	TestTrue(TEXT("the response carries the origin too"),
		FFlockRecordingLogger::AnyContains(BpLog->Debugs, TEXT("[Blueprint 'bpTest']")));

	// The reason the origin is copied at request time rather than read when the response lands: by then
	// some other call has usually started, and re-reading would attribute this response to that caller.
	const TSharedRef<FFlockRecordingLogger> DeferredLog = MakeShared<FFlockRecordingLogger>();
	const TSharedRef<FFlockHttpClient> DeferredClient = MakeShared<FFlockHttpClient>(Fake, DeferredLog);
	Fake->bDeferred = true;
	DeferredClient->Get<FFlockGameVersionSchema>(TEXT("http://x/thing"), {}, [](TFlockResult<FFlockGameVersionSchema>) {});
	FFlockCallOrigin::Set(TEXT("Blueprint 'somethingElse'"));
	Fake->FlushPending();
	Fake->bDeferred = false;

	TestTrue(TEXT("a late response keeps the origin it was sent with"),
		FFlockRecordingLogger::AnyContains(DeferredLog->Debugs, TEXT("<- 200 GET http://x/thing (")));
	TestFalse(TEXT("and is not attributed to whatever started meanwhile"),
		FFlockRecordingLogger::AnyContains(DeferredLog->Debugs, TEXT("somethingElse")));

	FFlockCallOrigin::Set(TEXT("C++"));
	return true;
}

// ── Reachability latch ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockHttpClientOfflineLatchTest, "Flock.Http.Client.OfflineLatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockHttpClientOfflineLatchTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
	const TSharedRef<FFlockHttpClient> Client = MakeClient(Fake);
	const FString Url = TEXT("http://x/test");

	auto Send = [&](const FFlockHttpResponse& Response)
	{
		Fake->On(TEXT("test"), Response);
		Client->Get<FFlockGameVersionSchema>(Url, {}, [](TFlockResult<FFlockGameVersionSchema>) {});
	};

	TestFalse(TEXT("a client nobody has sent through reads as online"), Client->IsLikelyOffline());

	Send(FFlockFakeTransport::Offline());
	TestTrue(TEXT("a connection failure latches offline"), Client->IsLikelyOffline());

	// The point of the whole rule: the server answering *at all* proves the network works, whatever it
	// thought of the request. A 500 has to clear the latch as firmly as a 200 would.
	Send(FFlockFakeTransport::Status(500, TEXT("")));
	TestFalse(TEXT("a 500 clears it — the server answered"), Client->IsLikelyOffline());

	Send(FFlockFakeTransport::Offline());
	TestTrue(TEXT("latched again"), Client->IsLikelyOffline());
	Send(FFlockFakeTransport::Status(401, TEXT("")));
	TestFalse(TEXT("a 401 clears it too"), Client->IsLikelyOffline());

	Send(FFlockFakeTransport::Ok(TEXT("{\"result\":{}}")));
	TestFalse(TEXT("and so does a success"), Client->IsLikelyOffline());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockHttpClientTimeoutDoesNotLatchTest, "Flock.Http.Client.TimeoutDoesNotLatchOffline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockHttpClientTimeoutDoesNotLatchTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
	const TSharedRef<FFlockHttpClient> Client = MakeClient(Fake);

	// A slow server is not a dead network, and the two mistakes are not symmetric: a false offline serves
	// stale cache without ever attempting the network, while a false online costs one attempt. So only a
	// genuine "couldn't reach it" latches.
	Fake->On(TEXT("test"), FFlockFakeTransport::Timeout());
	Client->Get<FFlockGameVersionSchema>(TEXT("http://x/test"), {}, [](TFlockResult<FFlockGameVersionSchema>) {});
	TestFalse(TEXT("a timeout does not latch offline"), Client->IsLikelyOffline());

	// Nor does a cancellation — the caller stopped it, the network never said anything.
	FFlockHttpResponse Cancelled;
	Cancelled.Result = EFlockHttpResult::Cancelled;
	Fake->On(TEXT("test"), Cancelled);
	Client->Get<FFlockGameVersionSchema>(TEXT("http://x/test"), {}, [](TFlockResult<FFlockGameVersionSchema>) {});
	TestFalse(TEXT("nor does a cancellation"), Client->IsLikelyOffline());

	// And a timeout must not *clear* a latch either — it carries no information in either direction.
	Fake->On(TEXT("test"), FFlockFakeTransport::Offline());
	Client->Get<FFlockGameVersionSchema>(TEXT("http://x/test"), {}, [](TFlockResult<FFlockGameVersionSchema>) {});
	Fake->On(TEXT("test"), FFlockFakeTransport::Timeout());
	Client->Get<FFlockGameVersionSchema>(TEXT("http://x/test"), {}, [](TFlockResult<FFlockGameVersionSchema>) {});
	TestTrue(TEXT("a timeout leaves an existing latch alone"), Client->IsLikelyOffline());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockHttpClientOfflineLatchExpiresTest, "Flock.Http.Client.OfflineLatchExpires",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockHttpClientOfflineLatchExpiresTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
	const TSharedRef<FFlockHttpClient> Client = MakeClient(Fake);

	// Expiry is what makes the latch self-healing: without it, a device that went offline once and never
	// completed another request would serve cache forever, because only a completion can clear it.
	Client->SetOfflineLatchSecondsForTesting(0.05f);
	Fake->On(TEXT("test"), FFlockFakeTransport::Offline());
	Client->Get<FFlockGameVersionSchema>(TEXT("http://x/test"), {}, [](TFlockResult<FFlockGameVersionSchema>) {});
	TestTrue(TEXT("latched"), Client->IsLikelyOffline());

	FPlatformProcess::Sleep(0.1f);
	TestFalse(TEXT("the latch lapses so the next read re-tests the network for real"), Client->IsLikelyOffline());

	return true;
}

#endif // WITH_AUTOMATION_TESTS
