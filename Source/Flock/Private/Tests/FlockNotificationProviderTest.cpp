// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockAuthSession.h"
#include "Containers/Ticker.h"
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

	/** A policy that *does* retry, so a "sent exactly once" assertion means the call opted out, not the policy. */
	inline FFlockRetryPolicy WithRetries()
	{
		FFlockRetryPolicy Policy;
		Policy.MaxRetries = 2;
		Policy.InitialDelaySeconds = 0.f;
		return Policy;
	}

	inline void PumpRetries()
	{
		for (int32 Index = 0; Index < 8; ++Index)
		{
			FTSTicker::GetCoreTicker().Tick(1.f);
		}
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

	// Real wire shapes. Eight of the nine notification routes are enveloped, but GET /v1/notification is
	// **bare** — {items,total,page,limit} at the document root with no {error,response,result} around it.
	// That asymmetry is the whole reason these fixtures are written by hand: enveloping the inbox fixture
	// would pass while the live route failed, which is exactly the bug class this family invites.

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

	/** The catalog is an enveloped **list** — `result` is a bare array, not a {items,total,...} page. */
	inline FString TemplatesBody()
	{
		return Env(TEXT("[{\"id\":\"tpl-1\",\"name\":\"DailyBonus\",\"category\":\"reward\"},")
			TEXT("{\"id\":\"tpl-2\",\"name\":\"EnergyFull\",\"category\":\"reminder\"}]"));
	}

	inline FString TemplateByNameBody(const FString& Id = TEXT("tpl-1"))
	{
		return Env(FString::Printf(TEXT("{\"id\":\"%s\",\"name\":\"DailyBonus\",\"category\":\"reward\"}"), *Id));
	}

	inline FString DeviceTokenBody(bool bActive = true)
	{
		return Env(FString::Printf(
			TEXT("{\"id\":\"dt-1\",\"player_id\":\"player-a\",\"game_id\":\"g1\",\"platform\":\"android\",")
			TEXT("\"is_active\":%s,\"last_seen_at\":null,\"created_at\":\"2026-08-13T00:00:00Z\"}"),
			bActive ? TEXT("true") : TEXT("false")));
	}

	inline FString UnregisterTokenBody(bool bDeactivated = true)
	{
		return Env(FString::Printf(TEXT("{\"deactivated\":%s}"), bDeactivated ? TEXT("true") : TEXT("false")));
	}

	/** A scheduled row. The three delivery-state timestamps are nullable and carry the real state. */
	inline FString ScheduledBody(bool bCanceled = false)
	{
		return Env(FString::Printf(
			TEXT("{\"id\":\"sch-1\",\"game_id\":\"g1\",\"studio_id\":\"s1\",\"player_id\":\"player-a\",")
			TEXT("\"template_id\":\"tpl-1\",\"variables\":{\"PlayerName\":\"Ada\"},")
			TEXT("\"channels\":[\"in_app\",\"push\"],\"deliver_at\":\"2026-08-13T09:00:00Z\",")
			TEXT("\"status\":\"pending\",\"source\":\"sdk\",\"notification_id\":null,\"delivered_at\":null,")
			TEXT("\"canceled_at\":%s,\"created_at\":\"2026-08-12T00:00:00Z\",\"updated_at\":\"2026-08-12T00:00:00Z\"}"),
			bCanceled ? TEXT("\"2026-08-12T02:00:00Z\"") : TEXT("null")));
	}
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

		explicit FFixture(const FString& ExistingDir = FString(), const FFlockRetryPolicy& Policy = NoRetry())
			: Dir(ExistingDir.IsEmpty() ? TempRoot() : ExistingDir)
			, Client(MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>()))
			, Session(MakeShared<FFlockAuthSession>(Client, Store, MakeShared<FFlockNullLogger>(),
				TEXT("http://x/v1"), TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") } }))
		{
			Snapshot = MakeShared<FFlockSnapshotStore>(Dir, MakeShared<FFlockNullLogger>(), TEXT("9.9.9"));
			Provider = MakeShared<FFlockNotificationProvider>(Client, Policy, MakeShared<FFlockNullLogger>(),
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
			// Order is load-bearing throughout: the fake answers the first route whose fragment the URL
			// contains. "notification_template/by-name?..." contains "notification_template", and
			// "notification/schedule/sch-1" contains "notification/schedule", so the specific paths go first.
			Fake->On(TEXT("device_token/register"), FFlockFakeTransport::Ok(DeviceTokenBody()));
			Fake->On(TEXT("device_token/unregister"), FFlockFakeTransport::Ok(UnregisterTokenBody()));
			Fake->On(TEXT("notification_template/by-name"), FFlockFakeTransport::Ok(TemplateByNameBody()));
			Fake->On(TEXT("notification_template"), FFlockFakeTransport::Ok(TemplatesBody()));
			Fake->On(TEXT("notification/schedule/sch-1"), FFlockFakeTransport::Ok(ScheduledBody(/*bCanceled*/ true)));
			Fake->On(TEXT("notification/schedule"), FFlockFakeTransport::Ok(ScheduledBody()));
			Fake->On(TEXT("notification/unread_count"), FFlockFakeTransport::Ok(UnreadCountBody()));
			Fake->On(TEXT("notification/summary"), FFlockFakeTransport::Ok(SummaryBody()));
			Fake->On(TEXT("notification/read_all"), FFlockFakeTransport::Ok(MarkAllReadBody()));
			Fake->On(TEXT("n-1/read"), FFlockFakeTransport::Ok(MarkReadBody()));
			Fake->On(TEXT("notification?"), FFlockFakeTransport::Ok(InboxBody()));
		}

		/**
		 * Overrides the by-name route while keeping it ahead of the broader "notification_template" one.
		 *
		 * On() removes the old route and **appends** the new one, and Resolve takes the first fragment the
		 * URL contains — so overriding by-name alone drops it behind the catalog route, and a by-name
		 * request comes back answered by the list fixture. Re-registering the list route afterwards puts
		 * the order back. Same trap as the leaderboard fixture's /me vs /{id} routes.
		 */
		void RouteTemplateByName(const FFlockHttpResponse& Response)
		{
			Fake->On(TEXT("notification_template/by-name"), Response);
			Fake->On(TEXT("notification_template"), FFlockFakeTransport::Ok(TemplatesBody()));
		}

		/** The body of the last request whose URL contains Fragment, for asserting what went on the wire. */
		FString LastBodyContaining(const FString& Fragment) const
		{
			for (int32 Index = Fake->Requests.Num() - 1; Index >= 0; --Index)
			{
				if (Fake->Requests[Index].Url.Contains(Fragment))
				{
					return Fake->Requests[Index].JsonBody;
				}
			}
			return FString();
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
			Fake->On(TEXT("notification_template/by-name"), FFlockFakeTransport::Offline());
			Fake->On(TEXT("notification_template"), FFlockFakeTransport::Offline());
			Fake->On(TEXT("notification/schedule/sch-1"), FFlockFakeTransport::Offline());
			Fake->On(TEXT("notification/schedule"), FFlockFakeTransport::Offline());
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

// NF-01: the inbox list is BARE. This is the test the whole family hangs on: an enveloped fixture would
// pass here and fail against the real backend with "missing result".
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

// NF-02: read_at drives IsRead(). A null must not land as the literal "null", or every unread row reads
// as read — the single most damaging parse slip available in this model.
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

// NF-03: every route here is player-scoped by its schema, so all five fail fast with Auth when signed
// out rather than spending a request on a guaranteed 401.
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

// NF-04: the count route is enveloped and its one-field object is unwrapped to a plain int.
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

// NF-05: the summary nests notification rows inside an enveloped object — the items need the custom
// parse, not the reflection path, or `data` and the nullables come back wrong.
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

// NF-06: the two writes. Mark-read returns the updated row; mark-all returns how many flipped.
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

// NF-07: an inbox read offline serves the last-known page rather than an error screen.
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

// NF-08: snapshot keys carry the player id, so a shared device cannot serve one account's mail to the
// next. Same directory, different player, offline — the previous player's inbox must not appear.
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

// NF-09: writes are never deferred. A read-receipt replayed later marks messages the player never saw,
// so an unreachable server must fail the call rather than quietly queue it.
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

// NF-10: the list query carries paging and the unread filter, and omits the filter when it is off.
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

// NF-11: the template catalog is an enveloped LIST (bare array in `result`), and it warms the name memo
// so a later send costs no extra round trip.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationTemplatesTest, "Flock.Notification.Provider.ParsesTemplateList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationTemplatesTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();
	bool bDone = false;
	F.Provider->GetTemplates([&](TFlockResult<TArray<FFlockNotificationTemplate>> Result)
	{
		bDone = true;
		TestTrue(TEXT("catalog succeeds"), Result.bSuccess);
		if (TestEqual(TEXT("two templates"), Result.Value.Num(), 2))
		{
			TestEqual(TEXT("first name"), FlockTestAt(Result.Value, 0).Name, FString(TEXT("DailyBonus")));
			TestEqual(TEXT("first id"), FlockTestAt(Result.Value, 0).Id, FString(TEXT("tpl-1")));
			TestEqual(TEXT("second category"), FlockTestAt(Result.Value, 1).Category, FString(TEXT("reminder")));
		}
	});
	TestTrue(TEXT("completed"), bDone);

	// The catalog warmed the memo, so a by-name lookup answers without touching the network.
	const int32 Before = F.Fake->Requests.Num();
	F.Provider->GetTemplateByName(TEXT("DailyBonus"), [&](TFlockResult<FFlockNotificationTemplate> Result)
	{
		TestTrue(TEXT("memoized lookup succeeds"), Result.bSuccess);
		TestEqual(TEXT("resolved id"), Result.Value.Id, FString(TEXT("tpl-1")));
	});
	TestEqual(TEXT("no request for a memoized name"), F.Fake->Requests.Num(), Before);

	Cleanup(F.Dir);
	return true;
}

// NF-12: the template routes are game-scoped — they declare no Authorization header — so unlike every
// other call here they must work signed out. Gating them would refuse a valid title-screen fetch.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationTemplatesUngatedTest, "Flock.Notification.Provider.TemplatesWorkSignedOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationTemplatesUngatedTest::RunTest(const FString& Parameters)
{
	FFixture F;   // deliberately not signed in
	bool bList = false, bByName = false;

	F.Provider->GetTemplates([&](TFlockResult<TArray<FFlockNotificationTemplate>> Result)
	{
		bList = true;
		TestTrue(TEXT("catalog works signed out"), Result.bSuccess);
	});
	F.Provider->GetTemplateByName(TEXT("DailyBonus"), [&](TFlockResult<FFlockNotificationTemplate> Result)
	{
		bByName = true;
		TestTrue(TEXT("by-name works signed out"), Result.bSuccess);
	});
	TestTrue(TEXT("list completed"), bList);
	TestTrue(TEXT("by-name completed"), bByName);

	// Scheduling still needs a player, because its own schema is player-keyed.
	bool bSchedule = false;
	F.Provider->ScheduleByTemplateName(TEXT("DailyBonus"), FDateTime(2026, 8, 13, 9, 0, 0),
		[&](TFlockResult<FFlockScheduledNotification> Result)
		{
			bSchedule = true;
			TestFalse(TEXT("schedule still gated"), Result.bSuccess);
			TestEqual(TEXT("as an auth failure"), Result.Error.Type, EFlockErrorType::Auth);
		});
	TestTrue(TEXT("schedule completed"), bSchedule);

	Cleanup(F.Dir);
	return true;
}

// NF-13: by-name puts the name in the **query string**, not the path, and adds locale only when given.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationTemplateByNameUrlTest, "Flock.Notification.Provider.TemplateByNameQuery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationTemplateByNameUrlTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.Provider->GetTemplateByName(TEXT("Daily Bonus"), TEXT("ar"), [](TFlockResult<FFlockNotificationTemplate>) {});
	const FString Localized = F.LastUrlContaining(TEXT("notification_template/by-name"));
	TestTrue(TEXT("name is a query param"), Localized.Contains(TEXT("by-name?name=")));
	// A space has to be encoded or the request is malformed.
	TestTrue(TEXT("name is percent-encoded"), Localized.Contains(TEXT("Daily%20Bonus")));
	TestTrue(TEXT("locale sent when given"), Localized.Contains(TEXT("locale=ar")));

	// A locale-specific record is not the default one, so it must not have been memoized under the bare
	// name — the next default-locale lookup has to go to the network.
	const int32 Before = F.Fake->Requests.Num();
	F.Provider->GetTemplateByName(TEXT("Daily Bonus"), [](TFlockResult<FFlockNotificationTemplate>) {});
	TestTrue(TEXT("localized fetch did not poison the default memo"), F.Fake->Requests.Num() > Before);
	const FString Default = F.LastUrlContaining(TEXT("notification_template/by-name"));
	TestFalse(TEXT("locale omitted when empty"), Default.Contains(TEXT("locale=")));

	Cleanup(F.Dir);
	return true;
}

// NF-14: scheduling by name resolves the id first, then posts. The resolve is memoized, so a second
// send by the same name does not repeat it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationScheduleByNameTest, "Flock.Notification.Provider.ScheduleResolvesName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationScheduleByNameTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();

	const FDateTime When(2026, 8, 13, 9, 0, 0);
	bool bDone = false;
	F.Provider->ScheduleByTemplateName(TEXT("DailyBonus"), When,
		FFlockCommandData().Set(TEXT("PlayerName"), TEXT("Ada")).Set(TEXT("Reward"), 100),
		{ EFlockNotificationChannel::InApp, EFlockNotificationChannel::Push },
		[&](TFlockResult<FFlockScheduledNotification> Result)
		{
			bDone = true;
			TestTrue(TEXT("schedule succeeds"), Result.bSuccess);
			TestEqual(TEXT("scheduled id"), Result.Value.Id, FString(TEXT("sch-1")));
		});
	TestTrue(TEXT("completed"), bDone);

	TestEqual(TEXT("resolved by name once"), F.Fake->CountTo(TEXT("notification_template/by-name")), 1);
	TestEqual(TEXT("then posted the schedule"), F.Fake->CountTo(TEXT("notification/schedule")), 1);

	// The body carries the resolved **id**, never the name.
	const FString Body = F.LastBodyContaining(TEXT("notification/schedule"));
	TestTrue(TEXT("template_id resolved from the name"), Body.Contains(TEXT("\"template_id\":\"tpl-1\"")));
	TestFalse(TEXT("the name itself is not sent"), Body.Contains(TEXT("DailyBonus")));
	TestTrue(TEXT("deliver_at is ISO-8601"), Body.Contains(TEXT("\"deliver_at\":\"2026-08-13T09:00:00")));
	// Author keys are the template's own and must never be case-transformed on the way out.
	TestTrue(TEXT("variable keys verbatim"), Body.Contains(TEXT("\"PlayerName\":\"Ada\""), ESearchCase::CaseSensitive));
	TestTrue(TEXT("numeric variable keeps its type"), Body.Contains(TEXT("\"Reward\":100"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("channels use wire spellings"), Body.Contains(TEXT("\"channels\":[\"in_app\",\"push\"]")));

	// Second send by the same name reuses the memo.
	F.Provider->ScheduleByTemplateName(TEXT("DailyBonus"), When, [](TFlockResult<FFlockScheduledNotification>) {});
	TestEqual(TEXT("name resolved only once across two sends"), F.Fake->CountTo(TEXT("notification_template/by-name")), 1);
	TestEqual(TEXT("but both schedules posted"), F.Fake->CountTo(TEXT("notification/schedule")), 2);

	Cleanup(F.Dir);
	return true;
}

// NF-15: an unknown template name is a caller mistake, so it fails Validation before any schedule is
// posted — never a silent send against nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationUnknownTemplateTest, "Flock.Notification.Provider.UnknownTemplateFailsValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationUnknownTemplateTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();
	// A 2xx carrying no id is how the backend reports "no such template for this game".
	F.RouteTemplateByName(FFlockFakeTransport::Ok(Env(TEXT("{\"id\":\"\",\"name\":\"\",\"category\":\"\"}"))));

	bool bDone = false;
	F.Provider->ScheduleByTemplateName(TEXT("NoSuchTemplate"), FDateTime(2026, 8, 13, 9, 0, 0),
		[&](TFlockResult<FFlockScheduledNotification> Result)
		{
			bDone = true;
			TestFalse(TEXT("unknown name fails"), Result.bSuccess);
			TestEqual(TEXT("as validation, not a server error"), Result.Error.Type, EFlockErrorType::Validation);
		});
	TestTrue(TEXT("completed"), bDone);
	TestEqual(TEXT("nothing was scheduled"), F.Fake->CountTo(TEXT("notification/schedule")), 0);

	Cleanup(F.Dir);
	return true;
}

// NF-16: empty variables and channels are omitted entirely, not sent as {} / []. An empty channel list
// reads as "deliver nowhere"; omitting it lets the template's own defaults apply.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationScheduleOmitsEmptyTest, "Flock.Notification.Provider.ScheduleOmitsEmptyOptionals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationScheduleOmitsEmptyTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();
	F.Provider->ScheduleByTemplateName(TEXT("DailyBonus"), FDateTime(2026, 8, 13, 9, 0, 0),
		[](TFlockResult<FFlockScheduledNotification>) {});

	const FString Body = F.LastBodyContaining(TEXT("notification/schedule"));
	TestTrue(TEXT("template_id still sent"), Body.Contains(TEXT("\"template_id\"")));
	TestFalse(TEXT("variables omitted when empty"), Body.Contains(TEXT("\"variables\"")));
	TestFalse(TEXT("channels omitted when empty"), Body.Contains(TEXT("\"channels\"")));

	Cleanup(F.Dir);
	return true;
}

// NF-17: scheduling is not idempotent. A resend after an ambiguous failure could leave the player with
// two of the same reminder, so it must go exactly once even under a policy that retries.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationScheduleNotRetriedTest, "Flock.Notification.Provider.ScheduleNotRetriedOnAmbiguousFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationScheduleNotRetriedTest::RunTest(const FString& Parameters)
{
	FFixture F(FString(), WithRetries());
	F.SignIn();
	// 500 after the request was accepted: the reminder may already exist, so a resend could duplicate it.
	F.Fake->On(TEXT("notification/schedule"), FFlockFakeTransport::Status(500, TEXT("{}")));

	bool bDone = false;
	F.Provider->ScheduleByTemplateId(TEXT("tpl-1"), FDateTime(2026, 8, 13, 9, 0, 0), FFlockCommandData(), {},
		[&](TFlockResult<FFlockScheduledNotification> Result)
		{
			bDone = true;
			TestFalse(TEXT("failure surfaces"), Result.bSuccess);
		});
	TestTrue(TEXT("caller was told"), bDone);

	PumpRetries();
	TestEqual(TEXT("posted exactly once"), F.Fake->CountTo(TEXT("notification/schedule")), 1);

	// Cancel under the same policy *does* retry, so the single attempt above is the rule at work rather
	// than a retry policy that never fires.
	F.Fake->On(TEXT("notification/schedule/sch-1"), FFlockFakeTransport::Status(500, TEXT("{}")));
	F.Provider->CancelScheduled(TEXT("sch-1"), [](TFlockResult<FFlockScheduledNotification>) {});
	PumpRetries();
	TestTrue(TEXT("cancel is retried"), F.Fake->CountTo(TEXT("notification/schedule/sch-1")) > 1);

	Cleanup(F.Dir);
	return true;
}

// NF-18: delivery state is read off the timestamps, not the loose `status` string.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationCancelTest, "Flock.Notification.Provider.CancelsScheduled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationCancelTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();

	bool bDone = false;
	F.Provider->CancelScheduled(TEXT("sch-1"), [&](TFlockResult<FFlockScheduledNotification> Result)
	{
		bDone = true;
		TestTrue(TEXT("cancel succeeds"), Result.bSuccess);
		TestTrue(TEXT("canceled_at set means canceled"), Result.Value.IsCanceled());
		TestFalse(TEXT("and not pending"), Result.Value.IsPending());
		TestFalse(TEXT("and not delivered"), Result.Value.IsDelivered());
	});
	TestTrue(TEXT("completed"), bDone);

	// The create fixture is still pending: null timestamps must read as pending, never as delivered.
	F.Provider->ScheduleByTemplateId(TEXT("tpl-1"), FDateTime(2026, 8, 13, 9, 0, 0), FFlockCommandData(), {},
		[&](TFlockResult<FFlockScheduledNotification> Result)
		{
			TestTrue(TEXT("null timestamps are pending"), Result.Value.IsPending());
			TestTrue(TEXT("null notification_id stays empty"), Result.Value.NotificationId.IsEmpty());
			// Response channels stay verbatim strings — the response types them loosely, so a server-side
			// addition must not fail the parse.
			TestEqual(TEXT("channels parsed"), Result.Value.Channels.Num(), 2);
			TestTrue(TEXT("channel spelling verbatim"), Result.Value.Channels.Contains(TEXT("in_app")));
			FString PlayerName;
			TestTrue(TEXT("variables readable"), Result.Value.Variables.TryGetString(TEXT("PlayerName"), PlayerName));
			TestEqual(TEXT("variable value"), PlayerName, FString(TEXT("Ada")));
		});

	Cleanup(F.Dir);
	return true;
}

// NF-19: logout drops the player's inbox but **keeps** the game-scoped template catalog. That split is
// the whole reason templates get their own snapshot category.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationClearCacheKeepsTemplatesTest, "Flock.Notification.Provider.ClearCacheKeepsTemplates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationClearCacheKeepsTemplatesTest::RunTest(const FString& Parameters)
{
	FString Dir;
	{
		FFixture F;
		Dir = F.Dir;
		F.SignIn();
		F.Provider->GetNotifications([](TFlockResult<FFlockNotificationPage>) {});
		F.Provider->GetTemplates([](TFlockResult<TArray<FFlockNotificationTemplate>>) {});
		F.Provider->ClearCache();
	}

	// Fresh provider over the same directory, offline: the inbox snapshot is gone, the catalog is not.
	FFixture F2(Dir);
	F2.SignIn();
	F2.GoOffline();

	bool bInbox = false, bTemplates = false;
	F2.Provider->GetNotifications([&](TFlockResult<FFlockNotificationPage> Result)
	{
		bInbox = true;
		TestFalse(TEXT("the departing player's inbox was dropped"), Result.bSuccess);
	});
	F2.Provider->GetTemplates([&](TFlockResult<TArray<FFlockNotificationTemplate>> Result)
	{
		bTemplates = true;
		TestTrue(TEXT("the game-scoped catalog survived logout"), Result.bSuccess);
		TestEqual(TEXT("with its rows"), Result.Value.Num(), 2);
	});
	TestTrue(TEXT("inbox completed"), bInbox);
	TestTrue(TEXT("templates completed"), bTemplates);

	Cleanup(Dir);
	return true;
}

// NF-20: the platform mapping. This is the one piece of sub-feature C that can be covered at all in an
// automation run — the real call site reads the running platform, which is Windows here forever. Pure and
// string-in for exactly that reason.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockDevicePlatformMappingTest, "Flock.Notification.DeviceToken.PlatformMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockDevicePlatformMappingTest::RunTest(const FString& Parameters)
{
	EFlockDevicePlatform Platform = EFlockDevicePlatform::Web;

	TestTrue(TEXT("Android maps"), FlockTryResolveDevicePlatform(TEXT("Android"), Platform));
	TestEqual(TEXT("to android"), Platform, EFlockDevicePlatform::Android);

	TestTrue(TEXT("IOS maps"), FlockTryResolveDevicePlatform(TEXT("IOS"), Platform));
	TestEqual(TEXT("to ios"), Platform, EFlockDevicePlatform::IOS);

	// The engine's spelling is "IOS"; be tolerant of case so a platform rename cannot silently unmap it.
	TestTrue(TEXT("case-insensitive"), FlockTryResolveDevicePlatform(TEXT("iOS"), Platform));

	// Everything the push backend does not accept must fail rather than resolve to a plausible value.
	for (const TCHAR* Unsupported : { TEXT("Windows"), TEXT("Mac"), TEXT("Linux"), TEXT("PS5"), TEXT("XSX"), TEXT("") })
	{
		TestFalse(FString::Printf(TEXT("%s does not map"), Unsupported),
			FlockTryResolveDevicePlatform(Unsupported, Platform));
	}

	// The wire spellings are what the backend's enum declares.
	TestEqual(TEXT("android wire"), FString(FlockDevicePlatformToWire(EFlockDevicePlatform::Android)), FString(TEXT("android")));
	TestEqual(TEXT("ios wire"), FString(FlockDevicePlatformToWire(EFlockDevicePlatform::IOS)), FString(TEXT("ios")));
	TestEqual(TEXT("web wire"), FString(FlockDevicePlatformToWire(EFlockDevicePlatform::Web)), FString(TEXT("web")));
	return true;
}

// NF-21: registering posts the platform's wire spelling plus the token, and parses the row back.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockDeviceTokenRegisterTest, "Flock.Notification.DeviceToken.RegistersToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockDeviceTokenRegisterTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();

	bool bDone = false;
	F.Provider->RegisterDeviceToken(EFlockDevicePlatform::Android, TEXT("fcm-abc123"),
		[&](TFlockResult<FFlockDeviceToken> Result)
		{
			bDone = true;
			TestTrue(TEXT("register succeeds"), Result.bSuccess);
			TestEqual(TEXT("row id"), Result.Value.Id, FString(TEXT("dt-1")));
			TestTrue(TEXT("row is active"), Result.Value.IsActive);
			// platform comes back as a plain string, matching how the response types it.
			TestEqual(TEXT("platform echoed"), Result.Value.Platform, FString(TEXT("android")));
			// null last_seen_at must stay empty, not become the literal "null".
			TestTrue(TEXT("null last_seen_at stays empty"), Result.Value.LastSeenAt.IsEmpty());
		});
	TestTrue(TEXT("completed"), bDone);

	const FString Body = F.LastBodyContaining(TEXT("device_token/register"));
	TestTrue(TEXT("wire platform spelling"), Body.Contains(TEXT("\"platform\":\"android\"")));
	TestTrue(TEXT("token sent verbatim"), Body.Contains(TEXT("\"token\":\"fcm-abc123\"")));

	// Unregister answers whether anything was actually deactivated.
	bool bOff = false;
	F.Provider->UnregisterDeviceToken(TEXT("fcm-abc123"), [&](TFlockResult<FFlockUnregisterDeviceTokenResult> Result)
	{
		bOff = true;
		TestTrue(TEXT("unregister succeeds"), Result.bSuccess);
		TestTrue(TEXT("a row was deactivated"), Result.Value.Deactivated);
	});
	TestTrue(TEXT("unregister completed"), bOff);
	TestTrue(TEXT("unregister sends only the token"),
		F.LastBodyContaining(TEXT("device_token/unregister")).Contains(TEXT("\"token\":\"fcm-abc123\"")));

	Cleanup(F.Dir);
	return true;
}

// NF-22: registering is **idempotent**, unlike scheduling — the row is keyed by token, so a retry after an
// ambiguous failure lands on the same state instead of creating a second registration.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockDeviceTokenRetriedTest, "Flock.Notification.DeviceToken.RegisterIsRetried",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockDeviceTokenRetriedTest::RunTest(const FString& Parameters)
{
	FFixture F(FString(), WithRetries());
	F.SignIn();
	F.Fake->On(TEXT("device_token/register"), FFlockFakeTransport::Status(500, TEXT("{}")));

	F.Provider->RegisterDeviceToken(EFlockDevicePlatform::IOS, TEXT("apns-xyz"), [](TFlockResult<FFlockDeviceToken>) {});
	PumpRetries();
	TestTrue(TEXT("register retries under a retrying policy"), F.Fake->CountTo(TEXT("device_token/register")) > 1);

	Cleanup(F.Dir);
	return true;
}

// NF-23: both calls are player-scoped, so they gate on sign-in and reject an empty token without spending
// a request. A token belongs to a player; there is nobody to register it against when signed out.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockDeviceTokenGuardsTest, "Flock.Notification.DeviceToken.Guards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockDeviceTokenGuardsTest::RunTest(const FString& Parameters)
{
	{
		FFixture F;   // signed out
		int32 Gated = 0;
		F.Provider->RegisterDeviceToken(EFlockDevicePlatform::Android, TEXT("t"), [&](TFlockResult<FFlockDeviceToken> R)
		{
			TestEqual(TEXT("register gated on auth"), R.Error.Type, EFlockErrorType::Auth);
			++Gated;
		});
		F.Provider->UnregisterDeviceToken(TEXT("t"), [&](TFlockResult<FFlockUnregisterDeviceTokenResult> R)
		{
			TestEqual(TEXT("unregister gated on auth"), R.Error.Type, EFlockErrorType::Auth);
			++Gated;
		});
		TestEqual(TEXT("both gated"), Gated, 2);
		TestEqual(TEXT("no requests sent"), F.Fake->Requests.Num(), 0);
		Cleanup(F.Dir);
	}
	{
		FFixture F;
		F.SignIn();
		int32 Rejected = 0;
		F.Provider->RegisterDeviceToken(EFlockDevicePlatform::Android, FString(), [&](TFlockResult<FFlockDeviceToken> R)
		{
			TestEqual(TEXT("empty token rejected"), R.Error.Type, EFlockErrorType::Validation);
			++Rejected;
		});
		F.Provider->UnregisterDeviceToken(FString(), [&](TFlockResult<FFlockUnregisterDeviceTokenResult> R)
		{
			TestEqual(TEXT("empty token rejected on unregister"), R.Error.Type, EFlockErrorType::Validation);
			++Rejected;
		});
		TestEqual(TEXT("both rejected"), Rejected, 2);
		TestEqual(TEXT("no requests sent"), F.Fake->Requests.Num(), 0);
		Cleanup(F.Dir);
	}
	return true;
}

// NF-24: the auto-detecting overload refuses on an unsupported platform rather than guessing one. This
// runs on Windows, so it exercises the refusal path directly — the accept path is covered by NF-20.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockDeviceTokenUnsupportedPlatformTest, "Flock.Notification.DeviceToken.RefusesUnsupportedPlatform",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockDeviceTokenUnsupportedPlatformTest::RunTest(const FString& Parameters)
{
	EFlockDevicePlatform Ignored = EFlockDevicePlatform::Android;
	const bool bSupportedHere = FFlockNotificationProvider::GetCurrentDevicePlatform(Ignored);
	// The automation host is a desktop editor or -game run, never Android or iOS.
	TestFalse(TEXT("push is not available on the test host"), bSupportedHere);

	FFixture F;
	F.SignIn();
	bool bDone = false;
	F.Provider->RegisterDeviceToken(TEXT("some-token"), [&](TFlockResult<FFlockDeviceToken> Result)
	{
		bDone = true;
		TestFalse(TEXT("auto-detect refuses here"), Result.bSuccess);
		TestEqual(TEXT("as validation"), Result.Error.Type, EFlockErrorType::Validation);
	});
	TestTrue(TEXT("completed"), bDone);
	// The point of refusing: nothing is filed under a platform that would never deliver.
	TestEqual(TEXT("no token was registered"), F.Fake->CountTo(TEXT("device_token/register")), 0);

	Cleanup(F.Dir);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
