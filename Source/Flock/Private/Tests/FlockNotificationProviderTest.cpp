// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockAuthSession.h"
#include "FlockLogger.h"
#include "HAL/FileManager.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockSnapshotStore.h"
#include "Misc/Base64.h"
#include "Misc/Paths.h"
#include "Providers/FlockNotificationProvider.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockTestSafeIndex.h"

namespace FlockNotificationProviderTestHelpers
{
	inline FFlockRetryPolicy NoRetry()
	{
		FFlockRetryPolicy Policy;
		Policy.MaxRetries = 0;
		return Policy;
	}

	inline FString MakeTestJwt(const FString& PlayerId)
	{
		const int64 Exp = FDateTime::UtcNow().ToUnixTimestamp() + 3600;
		FString Payload = FBase64::Encode(FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId, Exp));
		Payload.ReplaceInline(TEXT("+"), TEXT("-"));
		Payload.ReplaceInline(TEXT("/"), TEXT("_"));
		Payload.ReplaceInline(TEXT("="), TEXT(""));
		return FString::Printf(TEXT("h.%s.s"), *Payload);
	}

	inline FString TempRoot()
	{
		return FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
			FString::Printf(TEXT("nf_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	// ── Real wire shapes. Eight of the nine notification routes are enveloped, but GET /v1/notification is
	// **bare** — {items,total,page,limit} at the document root with no {error,response,result} around it.
	// That asymmetry is the whole reason these fixtures are written by hand: enveloping the inbox fixture
	// would pass while the live route failed, which is exactly the bug class this family invites. ──

	inline FString Env(const FString& Inner)
	{
		return FString::Printf(TEXT("{\"error\":null,\"response\":null,\"result\":%s}"), *Inner);
	}

	/** One inbox row. ReadAt null is the unread state and must survive as an empty string, not "null". */
	inline FString Row(const FString& Id, bool bRead)
	{
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"studio_id\":\"s1\",\"game_id\":\"g1\",\"recipient_type\":\"player\",")
			TEXT("\"recipient_id\":\"player-a\",\"type\":\"reward\",\"severity\":\"info\",")
			TEXT("\"title\":\"Daily bonus\",\"body\":\"Your reward is ready\",\"data\":{\"Coins\":25},")
			TEXT("\"read_at\":%s,\"campaign_id\":null,\"created_at\":\"2026-08-12T00:00:00Z\",")
			TEXT("\"updated_at\":\"2026-08-12T00:00:00Z\"}"),
			*Id, bRead ? TEXT("\"2026-08-12T01:00:00Z\"") : TEXT("null"));
	}

	/** BARE — no envelope. This is what the live backend answers on GET /v1/notification. */
	inline FString InboxBody()
	{
		return FString::Printf(TEXT("{\"items\":[%s,%s],\"total\":7,\"page\":1,\"limit\":50}"),
			*Row(TEXT("n-1"), /*bRead*/ false), *Row(TEXT("n-2"), /*bRead*/ true));
	}

	inline FString UnreadCountBody(int32 Count = 4) { return Env(FString::Printf(TEXT("{\"count\":%d}"), Count)); }
	inline FString SummaryBody() { return Env(FString::Printf(TEXT("{\"unread_count\":4,\"items\":[%s]}"), *Row(TEXT("n-1"), false))); }
	inline FString MarkReadBody() { return Env(Row(TEXT("n-1"), /*bRead*/ true)); }
	inline FString MarkAllReadBody() { return Env(TEXT("{\"updated\":4}")); }

	struct FFixture
	{
		FString Dir;
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockHttpClient> Client;
		TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();
		TSharedRef<FFlockAuthSession> Session;
		TSharedPtr<FFlockSnapshotStore> Snapshot;
		TSharedPtr<FFlockNotificationProvider> Provider;

		explicit FFixture(const FString& ExistingDir = FString())
			: Dir(ExistingDir.IsEmpty() ? TempRoot() : ExistingDir)
			, Client(MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>()))
			, Session(MakeShared<FFlockAuthSession>(Client, Store, MakeShared<FFlockNullLogger>(),
				TEXT("http://x/v1"), TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") } }))
		{
			Snapshot = MakeShared<FFlockSnapshotStore>(Dir, MakeShared<FFlockNullLogger>(), TEXT("9.9.9"));
			Provider = MakeShared<FFlockNotificationProvider>(Client, NoRetry(), MakeShared<FFlockNullLogger>(),
				Session, TEXT("http://x/v1"), Snapshot, TEXT("ver-1"));
			RouteAll();
		}

		/**
		 * Order is load-bearing: the fake answers the first route whose fragment the URL contains, and
		 * "notification" is a prefix of every one of these. The specific paths go first; the inbox list is
		 * matched on "notification?" so its query string is what distinguishes it.
		 */
		void RouteAll()
		{
			Fake->On(TEXT("notification/unread_count"), FFlockFakeTransport::Ok(UnreadCountBody()));
			Fake->On(TEXT("notification/summary"), FFlockFakeTransport::Ok(SummaryBody()));
			Fake->On(TEXT("notification/read_all"), FFlockFakeTransport::Ok(MarkAllReadBody()));
			Fake->On(TEXT("n-1/read"), FFlockFakeTransport::Ok(MarkReadBody()));
			Fake->On(TEXT("notification?"), FFlockFakeTransport::Ok(InboxBody()));
		}

		void SignIn(const FString& PlayerId = TEXT("player-a"))
		{
			FString Error;
			Session->SetTokens(MakeTestJwt(PlayerId), TEXT("r-1"), Error);
		}

		/** Probe says unreachable *and* every route fails to connect — the only offline state that can occur. */
		void GoOffline()
		{
			Provider->SetReachabilityProbe([]() { return false; });
			Fake->On(TEXT("notification/unread_count"), FFlockFakeTransport::Offline());
			Fake->On(TEXT("notification/summary"), FFlockFakeTransport::Offline());
			Fake->On(TEXT("notification/read_all"), FFlockFakeTransport::Offline());
			Fake->On(TEXT("n-1/read"), FFlockFakeTransport::Offline());
			Fake->On(TEXT("notification?"), FFlockFakeTransport::Offline());
		}

		FString LastUrlContaining(const FString& Fragment) const
		{
			for (int32 Index = Fake->Requests.Num() - 1; Index >= 0; --Index)
			{
				if (Fake->Requests[Index].Url.Contains(Fragment))
				{
					return Fake->Requests[Index].Url;
				}
			}
			return FString();
		}
	};

	inline void Cleanup(const FString& Dir)
	{
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
	}
}

using namespace FlockNotificationProviderTestHelpers;

// ── NF-01: the inbox list is BARE. This is the test the whole family hangs on: an enveloped fixture would
// pass here and fail against the real backend with "missing result". ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationParsesBarePageTest, "Flock.Notification.Provider.ParsesBarePaginatedShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationParsesBarePageTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();
	bool bDone = false;
	F.Provider->GetNotifications([&](TFlockResult<FFlockNotificationPage> Result)
	{
		bDone = true;
		TestTrue(TEXT("inbox succeeds"), Result.bSuccess);
		TestEqual(TEXT("two rows on this page"), Result.Value.Items.Num(), 2);
		// Total is the player's whole inbox, not this page's length — the distinction a paging UI needs.
		TestEqual(TEXT("total is the inbox count, not the page size"), Result.Value.Total, 7);
		TestEqual(TEXT("page"), Result.Value.Page, 1);
		TestEqual(TEXT("limit"), Result.Value.Limit, 50);
	});

	TestTrue(TEXT("completed"), bDone);
	Cleanup(F.Dir);
	return true;
}

// ── NF-02: read_at drives IsRead(). A null must not land as the literal "null", or every unread row reads
// as read — the single most damaging parse slip available in this model. ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationReadStateTest, "Flock.Notification.Provider.NullReadAtIsUnread",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationReadStateTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();
	F.Provider->GetNotifications([&](TFlockResult<FFlockNotificationPage> Result)
	{
		if (!TestEqual(TEXT("two rows"), Result.Value.Items.Num(), 2))
		{
			return;
		}
		const FFlockNotification& Unread = FlockTestAt(Result.Value.Items, 0);
		const FFlockNotification& Read = FlockTestAt(Result.Value.Items, 1);

		TestFalse(TEXT("null read_at is unread"), Unread.IsRead());
		TestTrue(TEXT("read_at empty, not the string null"), Unread.ReadAt.IsEmpty());
		TestTrue(TEXT("a stamped read_at is read"), Read.IsRead());

		// campaign_id is null on both rows and must not surface as "null" either.
		TestTrue(TEXT("null campaign_id stays empty"), Unread.CampaignId.IsEmpty());

		// `data` is an open dict kept verbatim — a sender's key spelling must survive the parse.
		TestTrue(TEXT("data handle is populated"), Unread.Data.IsValid());
		int32 Coins = 0;
		TestTrue(TEXT("author key read verbatim"), Unread.Data.TryGetInt(TEXT("Coins"), Coins));
		TestEqual(TEXT("data value"), Coins, 25);
	});
	Cleanup(F.Dir);
	return true;
}

// ── NF-03: every route here is player-scoped by its schema, so all five fail fast with Auth when signed
// out rather than spending a request on a guaranteed 401. ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationAuthGateTest, "Flock.Notification.Provider.RequiresSignIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationAuthGateTest::RunTest(const FString& Parameters)
{
	FFixture F;   // deliberately not signed in
	int32 Failures = 0;
	auto ExpectAuth = [&](bool bSuccess, const FFlockError& Error, const TCHAR* What)
	{
		TestFalse(What, bSuccess);
		TestEqual(What, Error.Type, EFlockErrorType::Auth);
		++Failures;
	};

	F.Provider->GetNotifications([&](TFlockResult<FFlockNotificationPage> R) { ExpectAuth(R.bSuccess, R.Error, TEXT("list gated")); });
	F.Provider->GetUnreadCount([&](TFlockResult<int32> R) { ExpectAuth(R.bSuccess, R.Error, TEXT("count gated")); });
	F.Provider->GetSummary([&](TFlockResult<FFlockNotificationSummary> R) { ExpectAuth(R.bSuccess, R.Error, TEXT("summary gated")); });
	F.Provider->MarkRead(TEXT("n-1"), [&](TFlockResult<FFlockNotification> R) { ExpectAuth(R.bSuccess, R.Error, TEXT("mark read gated")); });
	F.Provider->MarkAllRead([&](TFlockResult<FFlockMarkAllReadResult> R) { ExpectAuth(R.bSuccess, R.Error, TEXT("mark all gated")); });

	TestEqual(TEXT("all five gated"), Failures, 5);
	TestEqual(TEXT("and not one request was sent"), F.Fake->Requests.Num(), 0);
	Cleanup(F.Dir);
	return true;
}

// ── NF-04: the count route is enveloped and its one-field object is unwrapped to a plain int. ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationUnreadCountTest, "Flock.Notification.Provider.UnwrapsUnreadCount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationUnreadCountTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();
	bool bDone = false;
	F.Provider->GetUnreadCount([&](TFlockResult<int32> Result)
	{
		bDone = true;
		TestTrue(TEXT("count succeeds"), Result.bSuccess);
		TestEqual(TEXT("count unwrapped from the envelope"), Result.Value, 4);
	});
	TestTrue(TEXT("completed"), bDone);
	Cleanup(F.Dir);
	return true;
}

// ── NF-05: the summary nests notification rows inside an enveloped object — the items need the custom
// parse, not the reflection path, or `data` and the nullables come back wrong. ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationSummaryTest, "Flock.Notification.Provider.ParsesSummaryItems",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationSummaryTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();
	bool bDone = false;
	F.Provider->GetSummary([&](TFlockResult<FFlockNotificationSummary> Result)
	{
		bDone = true;
		TestTrue(TEXT("summary succeeds"), Result.bSuccess);
		TestEqual(TEXT("unread count"), Result.Value.UnreadCount, 4);
		if (TestEqual(TEXT("one preview row"), Result.Value.Items.Num(), 1))
		{
			const FFlockNotification& Row = FlockTestAt(Result.Value.Items, 0);
			TestEqual(TEXT("nested row id"), Row.Id, FString(TEXT("n-1")));
			TestEqual(TEXT("nested row title"), Row.Title, FString(TEXT("Daily bonus")));
			TestFalse(TEXT("nested row unread"), Row.IsRead());
		}
	});
	TestTrue(TEXT("completed"), bDone);
	Cleanup(F.Dir);
	return true;
}

// ── NF-06: the two writes. Mark-read returns the updated row; mark-all returns how many flipped. ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationWritesTest, "Flock.Notification.Provider.MarksRead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationWritesTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();

	bool bOne = false;
	F.Provider->MarkRead(TEXT("n-1"), [&](TFlockResult<FFlockNotification> Result)
	{
		bOne = true;
		TestTrue(TEXT("mark read succeeds"), Result.bSuccess);
		TestTrue(TEXT("row comes back read"), Result.Value.IsRead());
	});
	TestTrue(TEXT("mark read completed"), bOne);
	TestTrue(TEXT("hit the per-id read route"), F.LastUrlContaining(TEXT("n-1/read")).EndsWith(TEXT("notification/n-1/read")));

	bool bAll = false;
	F.Provider->MarkAllRead([&](TFlockResult<FFlockMarkAllReadResult> Result)
	{
		bAll = true;
		TestTrue(TEXT("mark all succeeds"), Result.bSuccess);
		TestEqual(TEXT("rows flipped"), Result.Value.Updated, 4);
	});
	TestTrue(TEXT("mark all completed"), bAll);

	// An empty id is a caller mistake, not a request worth sending.
	const int32 Before = F.Fake->Requests.Num();
	bool bRejected = false;
	F.Provider->MarkRead(FString(), [&](TFlockResult<FFlockNotification> Result)
	{
		bRejected = true;
		TestFalse(TEXT("empty id rejected"), Result.bSuccess);
		TestEqual(TEXT("as validation"), Result.Error.Type, EFlockErrorType::Validation);
	});
	TestTrue(TEXT("rejection completed"), bRejected);
	TestEqual(TEXT("no request sent for an empty id"), F.Fake->Requests.Num(), Before);

	Cleanup(F.Dir);
	return true;
}

// ── NF-07: an inbox read offline serves the last-known page rather than an error screen. ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationOfflineTest, "Flock.Notification.Provider.ServesCacheOffline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationOfflineTest::RunTest(const FString& Parameters)
{
	FString Dir;
	{
		FFixture F;
		Dir = F.Dir;
		F.SignIn();
		F.Provider->GetNotifications([](TFlockResult<FFlockNotificationPage>) {});
	}

	// Fresh provider over the same snapshot directory, same player, nothing reachable.
	FFixture F2(Dir);
	F2.SignIn();
	F2.GoOffline();
	bool bDone = false;
	F2.Provider->GetNotifications([&](TFlockResult<FFlockNotificationPage> Result)
	{
		bDone = true;
		TestTrue(TEXT("cache served offline"), Result.bSuccess);
		TestEqual(TEXT("cached rows"), Result.Value.Items.Num(), 2);
		TestEqual(TEXT("cached total"), Result.Value.Total, 7);
	});
	TestTrue(TEXT("completed"), bDone);
	Cleanup(Dir);
	return true;
}

// ── NF-08: snapshot keys carry the player id, so a shared device cannot serve one account's mail to the
// next. Same directory, different player, offline — the previous player's inbox must not appear. ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationPlayerScopedCacheTest, "Flock.Notification.Provider.CacheIsPlayerScoped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationPlayerScopedCacheTest::RunTest(const FString& Parameters)
{
	FString Dir;
	{
		FFixture F;
		Dir = F.Dir;
		F.SignIn(TEXT("player-a"));
		F.Provider->GetNotifications([](TFlockResult<FFlockNotificationPage>) {});
	}

	FFixture F2(Dir);
	F2.SignIn(TEXT("player-b"));   // a different player on the same device
	F2.GoOffline();
	bool bDone = false;
	F2.Provider->GetNotifications([&](TFlockResult<FFlockNotificationPage> Result)
	{
		bDone = true;
		// No cache exists under player-b's key, so this must fail rather than hand over player-a's inbox.
		TestFalse(TEXT("another player's cache is not served"), Result.bSuccess);
	});
	TestTrue(TEXT("completed"), bDone);
	Cleanup(Dir);
	return true;
}

// ── NF-09: writes are never deferred. A read-receipt replayed later marks messages the player never saw,
// so an unreachable server must fail the call rather than quietly queue it. ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationWritesNotQueuedTest, "Flock.Notification.Provider.WritesFailOfflineNotQueued",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationWritesNotQueuedTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();
	F.GoOffline();

	bool bOne = false;
	F.Provider->MarkRead(TEXT("n-1"), [&](TFlockResult<FFlockNotification> Result)
	{
		bOne = true;
		TestFalse(TEXT("mark read fails offline"), Result.bSuccess);
		TestEqual(TEXT("as a connection error"), Result.Error.Type, EFlockErrorType::Connection);
	});
	TestTrue(TEXT("completed"), bOne);

	bool bAll = false;
	F.Provider->MarkAllRead([&](TFlockResult<FFlockMarkAllReadResult> Result)
	{
		bAll = true;
		TestFalse(TEXT("mark all fails offline"), Result.bSuccess);
	});
	TestTrue(TEXT("completed"), bAll);

	Cleanup(F.Dir);
	return true;
}

// ── NF-10: the list query carries paging and the unread filter, and omits the filter when it is off. ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationQueryTest, "Flock.Notification.Provider.BuildsListQuery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationQueryTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();

	F.Provider->GetNotifications(/*bUnreadOnly*/ true, 2, 25, [](TFlockResult<FFlockNotificationPage>) {});
	const FString Filtered = F.LastUrlContaining(TEXT("notification?"));
	TestTrue(TEXT("page in query"), Filtered.Contains(TEXT("page=2")));
	TestTrue(TEXT("limit in query"), Filtered.Contains(TEXT("limit=25")));
	TestTrue(TEXT("unread filter present"), Filtered.Contains(TEXT("unread_only=true")));

	F.Provider->GetNotifications(/*bUnreadOnly*/ false, 1, 50, [](TFlockResult<FFlockNotificationPage>) {});
	const FString Unfiltered = F.LastUrlContaining(TEXT("notification?"));
	// Omitted rather than sent as false — the server should not have to read "everything" out of a filter.
	TestFalse(TEXT("unread filter omitted when off"), Unfiltered.Contains(TEXT("unread_only")));

	Cleanup(F.Dir);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
