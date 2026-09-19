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
#include "FlockEvents.h"
#include "Providers/FlockNotificationProvider.h"
#include "Tests/Support/FlockEventTestListener.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockTestSafeIndex.h"
#include "Tests/Support/FlockTestSpelling.h"

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

	/**
	 * A row with a caller-chosen created_at. The fixed-date Row() above cannot express ordering, and the
	 * seen-watermark is entirely about ordering — a literal is passed through verbatim so a deliberately
	 * unparseable timestamp can be tested too.
	 */
	inline FString RowAt(const FString& Id, const FString& CreatedAt)
	{
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"studio_id\":\"s1\",\"game_id\":\"g1\",\"recipient_type\":\"player\",")
			TEXT("\"recipient_id\":\"player-a\",\"type\":\"reward\",\"severity\":\"info\",")
			TEXT("\"title\":\"Daily bonus\",\"body\":\"Your reward is ready\",\"data\":{\"Coins\":25},")
			TEXT("\"read_at\":null,\"campaign_id\":null,\"created_at\":\"%s\",")
			TEXT("\"updated_at\":\"2026-08-12T00:00:00Z\"}"),
			*Id, *CreatedAt);
	}

	/** A bare inbox page built from explicit rows, newest-first the way the real route orders them. */
	inline FString InboxOf(const TArray<FString>& Rows)
	{
		return FString::Printf(TEXT("{\"items\":[%s],\"total\":%d,\"page\":1,\"limit\":50}"),
			*FString::Join(Rows, TEXT(",")), Rows.Num());
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

	/**
	 * A `deliver_at` far enough ahead that it cannot elapse mid-suite.
	 *
	 * **Derived, never hardcoded.** A literal future date is a time bomb: once it passes, the pending-schedule
	 * tests start failing for a reason that has nothing to do with the SDK — `LoadPendingSchedules` would be
	 * correctly dropping the entry as delivered, and every "still pending" assertion would read zero.
	 */
	inline FString FutureIso()
	{
		return (FDateTime::UtcNow() + FTimespan::FromDays(365)).ToIso8601();
	}

	/** The mirror image, for exercising the drop-on-elapsed path. */
	inline FString PastIso()
	{
		return (FDateTime::UtcNow() - FTimespan::FromDays(1)).ToIso8601();
	}

	/**
	 * A past timestamp in the shape the **real backend** echoes: microsecond fraction and **no timezone
	 * suffix** (`2026-08-17T07:46:12.493000`), not the `...Z` that `FDateTime::ToIso8601` emits.
	 *
	 * Worth its own fixture because the two forms take different paths through `FDateTime::ParseIso8601`:
	 * a >3-digit fraction is rounded to milliseconds, and the timezone branch has to accept a bare
	 * terminator. A fixture that only ever used the `Z` form would pass while production never dropped a
	 * single elapsed entry — the pending list would grow forever. Confirmed live 2026-08-17.
	 */
	inline FString PastIsoServerShape()
	{
		const FDateTime When = FDateTime::UtcNow() - FTimespan::FromDays(1);
		return FString::Printf(TEXT("%04d-%02d-%02dT%02d:%02d:%02d.%03d000"),
			When.GetYear(), When.GetMonth(), When.GetDay(),
			When.GetHour(), When.GetMinute(), When.GetSecond(), When.GetMillisecond());
	}

	/** A scheduled row. The three delivery-state timestamps are nullable and carry the real state. */
	inline FString ScheduledBody(bool bCanceled = false, const FString& DeliverAt = FString(),
		const FString& Id = TEXT("sch-1"))
	{
		return Env(FString::Printf(
			TEXT("{\"id\":\"%s\",\"game_id\":\"g1\",\"studio_id\":\"s1\",\"player_id\":\"player-a\",")
			TEXT("\"template_id\":\"tpl-1\",\"variables\":{\"PlayerName\":\"Ada\"},")
			TEXT("\"channels\":[\"in_app\",\"push\"],\"deliver_at\":\"%s\",")
			TEXT("\"status\":\"pending\",\"source\":\"sdk\",\"notification_id\":null,\"delivered_at\":null,")
			TEXT("\"canceled_at\":%s,\"created_at\":\"2026-08-12T00:00:00Z\",\"updated_at\":\"2026-08-12T00:00:00Z\"}"),
			*Id, *(DeliverAt.IsEmpty() ? FutureIso() : DeliverAt),
			bCanceled ? TEXT("\"2026-08-12T02:00:00Z\"") : TEXT("null")));
	}
	/**
	 * A **bare** paginated schedule page: {items,total,page,limit} at the root, no envelope — the shape
	 * GET /v1/notification/schedule actually answers with. An enveloped fixture would still parse (the
	 * unwrapper descends into `result` only when it is there) and would prove nothing about the real wire.
	 *
	 * Takes explicit row bodies, because the point of most of these tests is *which* ids the server lists.
	 */
	inline FString SchedulePageOf(const TArray<FString>& Ids)
	{
		TArray<FString> Rows;
		for (const FString& Id : Ids)
		{
			// ScheduledBody envelopes; the rows inside a page are bare, so strip back to the object itself.
			Rows.Add(FString::Printf(
				TEXT("{\"id\":\"%s\",\"game_id\":\"g1\",\"studio_id\":\"s1\",\"player_id\":\"player-a\",")
				TEXT("\"template_id\":\"tpl-1\",\"variables\":{},\"channels\":[\"in_app\"],\"deliver_at\":\"%s\",")
				TEXT("\"status\":\"pending\",\"source\":\"sdk\",\"notification_id\":null,\"delivered_at\":null,")
				TEXT("\"canceled_at\":null,\"created_at\":\"2026-08-12T00:00:00Z\",\"updated_at\":\"2026-08-12T00:00:00Z\"}"),
				*Id, *FutureIso()));
		}
		return FString::Printf(TEXT("{\"items\":[%s],\"total\":%d,\"page\":1,\"limit\":100}"),
			*FString::Join(Rows, TEXT(",")), Rows.Num());
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
		UFlockEvents* Events = nullptr;
		UFlockEventTestListener* Listener = nullptr;

		explicit FFixture(const FString& ExistingDir = FString(), const FFlockRetryPolicy& Policy = NoRetry())
			: Dir(ExistingDir.IsEmpty() ? TempRoot() : ExistingDir)
			, Client(MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>()))
			, Session(MakeShared<FFlockAuthSession>(Client, Store, MakeShared<FFlockNullLogger>(),
				TEXT("http://x/v1"), TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") } }))
		{
			Events = NewObject<UFlockEvents>();
			Listener = NewObject<UFlockEventTestListener>();
			Events->OnUnreadCountChanged.AddDynamic(Listener, &UFlockEventTestListener::HandleUnreadCountChanged);
			Events->OnNotificationReceived.AddDynamic(Listener, &UFlockEventTestListener::HandleNotificationReceived);

			Snapshot = MakeShared<FFlockSnapshotStore>(Dir, MakeShared<FFlockNullLogger>(), TEXT("9.9.9"));
			Provider = MakeShared<FFlockNotificationProvider>(Client, Policy, MakeShared<FFlockNullLogger>(),
				Session, Events, TEXT("http://x/v1"), Snapshot, TEXT("ver-1"));
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
			// The listing GET and the scheduling POST share a path and differ only by a query string, and
			// the fake matches on URL fragments with no notion of method — so "notification/schedule?" has
			// to be registered ahead of the bare fragment or the POST fixture answers the list as well.
			Fake->On(TEXT("notification/schedule?"), FFlockFakeTransport::Ok(SchedulePageOf({ TEXT("sch-1") })));
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

		/**
		 * The same trap one level down: `notification/schedule` is a prefix of `notification/schedule/{id}`,
		 * so a cancel route registered after the schedule POST is unreachable and *every* cancel silently
		 * succeeds — a test that asserts nothing. Re-registering the general route puts it back at the end.
		 * Route cancels through this, never through a bare `On`.
		 */
		void RouteCancel(const FString& ScheduledId, const FFlockHttpResponse& Response)
		{
			Fake->On(FString::Printf(TEXT("notification/schedule/%s"), *ScheduledId), Response);
			Fake->On(TEXT("notification/schedule"), FFlockFakeTransport::Ok(ScheduledBody()));
		}

		/**
		 * Sets what the **server** reports as pending, which is what CancelAllScheduled now cancels.
		 *
		 * Re-registers the bare fragment afterwards for the same reason RouteCancel does: On() appends, and
		 * the fake answers the first fragment the URL contains, so registering the query route alone would
		 * leave it behind the bare one and the POST fixture would answer the listing.
		 */
		void RouteScheduleList(const TArray<FString>& Ids)
		{
			Fake->On(TEXT("notification/schedule?"), FFlockFakeTransport::Ok(SchedulePageOf(Ids)));
			Fake->On(TEXT("notification/schedule"), FFlockFakeTransport::Ok(ScheduledBody()));
		}

		/** The server listing fails, which is the only thing that lets the local list have a say. */
		void RouteScheduleListFailure(const FFlockHttpResponse& Response)
		{
			Fake->On(TEXT("notification/schedule?"), Response);
			Fake->On(TEXT("notification/schedule"), FFlockFakeTransport::Ok(ScheduledBody()));
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
			Fake->On(TEXT("notification/schedule?"), FFlockFakeTransport::Offline());
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

// NF-08b: ClearCache clears for one player. Everyone else on the device is out of its reach.
//
// This is the two-player case, and it is the only one that can catch the defect: every single-player test
// passes just as well against a whole-category delete that restores the current player's records
// afterwards. Player A's pending schedules are the ones that matter — a reminder still fires server-side,
// and its id is the only handle on it, so a lost list is a reminder A can never cancel.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationClearCacheOtherPlayerTest, "Flock.Notification.Provider.ClearCacheSparesOtherPlayers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationClearCacheOtherPlayerTest::RunTest(const FString& Parameters)
{
	FString Dir;
	{
		// Player A schedules a reminder, reads their inbox, and signs out.
		FFixture A;
		Dir = A.Dir;
		A.SignIn(TEXT("player-a"));
		A.Provider->ScheduleByTemplateName(TEXT("DailyBonus"), FDateTime::UtcNow() + FTimespan::FromHours(4),
			[](TFlockResult<FFlockScheduledNotification>) {});
		A.Provider->GetNotifications([](TFlockResult<FFlockNotificationPage>) {});
		TestEqual(TEXT("player A has a pending reminder"), A.Provider->GetPendingSchedules().Num(), 1);
	}

	{
		// Player B signs in on the same device and the game clears the notification cache — a settings
		// screen, a "refresh" button, or simply B signing out again later.
		FFixture B(Dir);
		B.SignIn(TEXT("player-b"));
		B.Provider->GetNotifications([](TFlockResult<FFlockNotificationPage>) {});
		B.Provider->ClearCache();
	}

	{
		// Player A comes back. Their reminder is still cancellable.
		FFixture A2(Dir);
		A2.SignIn(TEXT("player-a"));
		TestEqual(TEXT("player A's pending reminder survived player B's ClearCache"),
			A2.Provider->GetPendingSchedules().Num(), 1);

		// And their inbox cache is still there — proved the only way a cache can be: offline, where there
		// is nothing else that could answer.
		A2.GoOffline();
		bool bDone = false;
		A2.Provider->GetNotifications([&](TFlockResult<FFlockNotificationPage> Result)
		{
			bDone = true;
			TestTrue(TEXT("player A's inbox cache survived too"), Result.bSuccess);
		});
		TestTrue(TEXT("completed"), bDone);
	}

	Cleanup(Dir);
	return true;
}

// NF-08c: signed out there is no player to clear for, and no scope that could stand in for one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationClearCacheSignedOutTest, "Flock.Notification.Provider.ClearCacheSignedOutClearsNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationClearCacheSignedOutTest::RunTest(const FString& Parameters)
{
	FString Dir;
	{
		FFixture F;
		Dir = F.Dir;
		F.SignIn(TEXT("player-a"));
		F.Provider->ScheduleByTemplateName(TEXT("DailyBonus"), FDateTime::UtcNow() + FTimespan::FromHours(4),
			[](TFlockResult<FFlockScheduledNotification>) {});
		F.Provider->GetNotifications([](TFlockResult<FFlockNotificationPage>) {});
	}

	{
		// Never signed in. An empty player id used to resolve to the bare category, so this call took every
		// account on the device with it — the worst version of the bug, from the call that does least.
		FFixture SignedOut(Dir);
		SignedOut.Provider->ClearCache();
	}

	FFixture Back(Dir);
	Back.SignIn(TEXT("player-a"));
	TestEqual(TEXT("a signed-out clear left the pending list alone"), Back.Provider->GetPendingSchedules().Num(), 1);

	Back.GoOffline();
	bool bDone = false;
	Back.Provider->GetNotifications([&](TFlockResult<FFlockNotificationPage> Result)
	{
		bDone = true;
		TestTrue(TEXT("and left the inbox cache alone"), Result.bSuccess);
	});
	TestTrue(TEXT("completed"), bDone);
	Cleanup(Dir);
	return true;
}

// NF-08d: state written by an earlier version is picked up, not stranded.
//
// Before this change the watermark and the pending list lived in the cache category under "<key>_<player>".
// An upgrade that simply started reading a new location would silently lose every pending reminder on the
// device — the same harm as the bug, arriving with the fix for it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationLegacyStateTest, "Flock.Notification.Provider.MigratesLegacyStateLocation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationLegacyStateTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));

	// Written exactly where the previous version put it: shared cache category, player id on the key.
	const FString LegacyScope = TEXT("ver-1/notification");
	F.Snapshot->Write(LegacyScope, TEXT("pending_schedules_player-a"),
		FString::Printf(TEXT("[{\"Id\":\"sch-legacy\",\"TemplateName\":\"DailyBonus\",\"TemplateId\":\"tpl-1\",\"DeliverAt\":\"%s\"}]"),
			*FutureIso()));

	const TArray<FFlockPendingSchedule> Pending = F.Provider->GetPendingSchedules();
	TestEqual(TEXT("the legacy list is read"), Pending.Num(), 1);
	if (Pending.Num() == 1)
	{
		TestEqual(TEXT("and carries its id"), Pending[0].Id, FString(TEXT("sch-legacy")));
	}

	// Migrated, not merely read: the new location holds it and the old copy is gone, so a later
	// ClearCache — which no longer reaches the old location either — cannot resurrect a stale list.
	FString Payload;
	TestTrue(TEXT("moved to the state scope"),
		F.Snapshot->TryRead(TEXT("ver-1/notification_state/player-a"), TEXT("pending_schedules"), Payload));
	TestFalse(TEXT("legacy copy removed"),
		F.Snapshot->TryRead(LegacyScope, TEXT("pending_schedules_player-a"), Payload));

	Cleanup(F.Dir);
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
	TestTrue(TEXT("name is percent-encoded"), Localized.Contains(TEXT("Daily%20Bonus"), ESearchCase::CaseSensitive));
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
	TestTrue(TEXT("template_id resolved from the name"), Body.Contains(TEXT("\"template_id\":\"tpl-1\""), ESearchCase::CaseSensitive));
	TestFalse(TEXT("the name itself is not sent"), Body.Contains(TEXT("DailyBonus")));
	TestTrue(TEXT("deliver_at is ISO-8601"), Body.Contains(TEXT("\"deliver_at\":\"2026-08-13T09:00:00"), ESearchCase::CaseSensitive));
	// Author keys are the template's own and must never be case-transformed on the way out.
	TestTrue(TEXT("variable keys verbatim"), Body.Contains(TEXT("\"PlayerName\":\"Ada\""), ESearchCase::CaseSensitive));
	TestTrue(TEXT("numeric variable keeps its type"), Body.Contains(TEXT("\"Reward\":100"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("channels use wire spellings"), Body.Contains(TEXT("\"channels\":[\"in_app\",\"push\"]"), ESearchCase::CaseSensitive));

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
	TestTrue(TEXT("template_id still sent"), Body.Contains(TEXT("\"template_id\""), ESearchCase::CaseSensitive));
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
			TestTrue(TEXT("channel spelling verbatim"), FlockTestSpelling::HoldsExactly(Result.Value.Channels, TEXT("in_app")));
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
	TestTrue(TEXT("wire platform spelling"), Body.Contains(TEXT("\"platform\":\"android\""), ESearchCase::CaseSensitive));
	TestTrue(TEXT("token sent verbatim"), Body.Contains(TEXT("\"token\":\"fcm-abc123\""), ESearchCase::CaseSensitive));

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
		F.LastBodyContaining(TEXT("device_token/unregister")).Contains(TEXT("\"token\":\"fcm-abc123\""), ESearchCase::CaseSensitive));

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

// Notification events
//
// "Received" is fetch-derived — there is no realtime channel and this SDK never polls — so the seen
// watermark is the entire mechanism. These pin the four properties that make it safe: seed silently,
// raise once, raise oldest-first, and never let one player's cutoff apply to another's inbox.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationSeedsSilentlyTest, "Flock.Notification.Events.FirstFetchSeedsSilently",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationSeedsSilentlyTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();

	bool bDone = false;
	F.Provider->GetNotifications([&](TFlockResult<FFlockNotificationPage> Result) { bDone = Result.bSuccess; });
	TestTrue(TEXT("inbox fetched"), bDone);

	// A player who already has mail must not be handed their whole history as a burst of "received" events
	// the first time the game asks.
	TestEqual(TEXT("first fetch announces nothing"), F.Listener->ReceivedNotifications.Num(), 0);

	Cleanup(F.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationRaisesNewOldestFirstTest, "Flock.Notification.Events.RaisesNewOldestFirst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationRaisesNewOldestFirstTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();

	// Seed at T1.
	F.Fake->On(TEXT("notification?"), FFlockFakeTransport::Ok(InboxOf({ RowAt(TEXT("n-1"), TEXT("2026-08-12T00:00:00Z")) })));
	F.Provider->GetNotifications([](TFlockResult<FFlockNotificationPage>) {});
	TestEqual(TEXT("seed is silent"), F.Listener->ReceivedNotifications.Num(), 0);

	// Two newer rows arrive. The route answers newest-first, which is why raise order is worth asserting:
	// handing a game its mail backwards is the bug this walks the page in reverse to avoid.
	F.Fake->On(TEXT("notification?"), FFlockFakeTransport::Ok(InboxOf({
		RowAt(TEXT("n-3"), TEXT("2026-08-14T00:00:00Z")),
		RowAt(TEXT("n-2"), TEXT("2026-08-13T00:00:00Z")),
		RowAt(TEXT("n-1"), TEXT("2026-08-12T00:00:00Z")) })));
	F.Provider->GetNotifications([](TFlockResult<FFlockNotificationPage>) {});

	if (TestEqual(TEXT("only the two new rows raise"), F.Listener->ReceivedNotifications.Num(), 2))
	{
		TestEqual(TEXT("oldest first"), FlockTestAt(F.Listener->ReceivedNotifications, 0).Id, FString(TEXT("n-2")));
		TestEqual(TEXT("then the newer one"), FlockTestAt(F.Listener->ReceivedNotifications, 1).Id, FString(TEXT("n-3")));
	}

	// A third fetch of the same page raises nothing: the watermark advanced past both.
	F.Provider->GetNotifications([](TFlockResult<FFlockNotificationPage>) {});
	TestEqual(TEXT("re-reading the same page announces nothing again"), F.Listener->ReceivedNotifications.Num(), 2);

	Cleanup(F.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationWatermarkPlayerScopedTest, "Flock.Notification.Events.WatermarkIsPlayerScoped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationWatermarkPlayerScopedTest::RunTest(const FString& Parameters)
{
	FFixture F;

	// Player A seeds at a late date.
	F.SignIn(TEXT("player-a"));
	F.Fake->On(TEXT("notification?"), FFlockFakeTransport::Ok(InboxOf({ RowAt(TEXT("a-1"), TEXT("2026-08-20T00:00:00Z")) })));
	F.Provider->GetNotifications([](TFlockResult<FFlockNotificationPage>) {});

	// Player B signs in on the same device and seeds at an early date.
	F.SignIn(TEXT("player-b"));
	F.Fake->On(TEXT("notification?"), FFlockFakeTransport::Ok(InboxOf({ RowAt(TEXT("b-1"), TEXT("2026-08-01T00:00:00Z")) })));
	F.Provider->GetNotifications([](TFlockResult<FFlockNotificationPage>) {});
	TestEqual(TEXT("B's first fetch is silent too"), F.Listener->ReceivedNotifications.Num(), 0);

	// B now gets a row dated *before* A's cutoff but after B's. A shared watermark would swallow it; a
	// player-scoped one raises it. That is the whole point of suffixing the key with the player id.
	F.Fake->On(TEXT("notification?"), FFlockFakeTransport::Ok(InboxOf({
		RowAt(TEXT("b-2"), TEXT("2026-08-05T00:00:00Z")),
		RowAt(TEXT("b-1"), TEXT("2026-08-01T00:00:00Z")) })));
	F.Provider->GetNotifications([](TFlockResult<FFlockNotificationPage>) {});

	if (TestEqual(TEXT("B hears about their own new mail"), F.Listener->ReceivedNotifications.Num(), 1))
	{
		TestEqual(TEXT("and it is B's row"), FlockTestAt(F.Listener->ReceivedNotifications, 0).Id, FString(TEXT("b-2")));
	}

	Cleanup(F.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationClearCacheKeepsWatermarkTest, "Flock.Notification.Events.ClearCacheKeepsWatermark",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationClearCacheKeepsWatermarkTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();
	F.Fake->On(TEXT("notification?"), FFlockFakeTransport::Ok(InboxOf({ RowAt(TEXT("n-1"), TEXT("2026-08-12T00:00:00Z")) })));
	F.Provider->GetNotifications([](TFlockResult<FFlockNotificationPage>) {});

	// Logout drops the inbox rows — they belong to the departing player — but the watermark is state, not
	// cache. Losing it would re-announce this player's entire inbox the next time they signed in.
	F.Provider->ClearCache();

	F.Provider->GetNotifications([](TFlockResult<FFlockNotificationPage>) {});
	TestEqual(TEXT("the already-seen row is not re-announced after ClearCache"),
		F.Listener->ReceivedNotifications.Num(), 0);

	Cleanup(F.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationUnparseableDateSkippedTest, "Flock.Notification.Events.UnparseableCreatedAtIsSkipped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationUnparseableDateSkippedTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();
	F.Fake->On(TEXT("notification?"), FFlockFakeTransport::Ok(InboxOf({ RowAt(TEXT("n-1"), TEXT("2026-08-12T00:00:00Z")) })));
	F.Provider->GetNotifications([](TFlockResult<FFlockNotificationPage>) {});

	// A row the SDK cannot place in time is skipped rather than announced: with no comparable timestamp it
	// would raise on every single fetch, and a duplicate is worse than a miss.
	F.Fake->On(TEXT("notification?"), FFlockFakeTransport::Ok(InboxOf({
		RowAt(TEXT("bad"), TEXT("not-a-date")),
		RowAt(TEXT("n-1"), TEXT("2026-08-12T00:00:00Z")) })));
	F.Provider->GetNotifications([](TFlockResult<FFlockNotificationPage>) {});
	F.Provider->GetNotifications([](TFlockResult<FFlockNotificationPage>) {});

	TestEqual(TEXT("an undateable row never raises, however often it is fetched"),
		F.Listener->ReceivedNotifications.Num(), 0);

	Cleanup(F.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationUnreadCountEventTest, "Flock.Notification.Events.ServerReportedCountsRaise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationUnreadCountEventTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();

	F.Provider->GetUnreadCount([](TFlockResult<int32>) {});
	TestEqual(TEXT("an unread-count fetch raises once"), F.Listener->UnreadCountChangedCount, 1);
	TestEqual(TEXT("with the server's number"), F.Listener->LastUnreadCount, 4);

	// The summary is the one call that reports a count *and* carries rows, so it feeds both events.
	F.Provider->GetSummary([](TFlockResult<FFlockNotificationSummary>) {});
	TestEqual(TEXT("summary raises the count too"), F.Listener->UnreadCountChangedCount, 2);

	// Mark-all-read has exactly one possible outcome, so zero is reported rather than left to a refetch.
	F.Provider->MarkAllRead([](TFlockResult<FFlockMarkAllReadResult>) {});
	TestEqual(TEXT("mark-all-read raises"), F.Listener->UnreadCountChangedCount, 3);
	TestEqual(TEXT("as zero unread"), F.Listener->LastUnreadCount, 0);

	Cleanup(F.Dir);
	return true;
}

// Pending schedules
//
// The id a schedule returns is the only handle on a pending reminder and /v1 has no route to list or read
// one back, so the SDK keeps its own list. These pin what that list must survive and when it must forget.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationTracksPendingTest, "Flock.Notification.Pending.TrackedOnScheduleDroppedOnCancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationTracksPendingTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();
	TestEqual(TEXT("nothing tracked to begin with"), F.Provider->GetPendingSchedules().Num(), 0);

	F.Provider->ScheduleByTemplateName(TEXT("DailyBonus"), FDateTime::UtcNow() + FTimespan::FromHours(2),
		FFlockCommandData(), {}, [](TFlockResult<FFlockScheduledNotification>) {});

	const TArray<FFlockPendingSchedule> Pending = F.Provider->GetPendingSchedules();
	if (TestEqual(TEXT("the schedule is tracked"), Pending.Num(), 1))
	{
		const FFlockPendingSchedule& Entry = FlockTestAt(Pending, 0);
		TestEqual(TEXT("by its scheduled id"), Entry.Id, FString(TEXT("sch-1")));
		// The name is what a caller has; carrying it means telling entries apart needs no second call.
		TestEqual(TEXT("with the template name it was scheduled by"), Entry.TemplateName, FString(TEXT("DailyBonus")));
		TestEqual(TEXT("and the id that name resolved to"), Entry.TemplateId, FString(TEXT("tpl-1")));
		TestFalse(TEXT("deliver_at recorded"), Entry.DeliverAt.IsEmpty());
	}

	F.Provider->CancelScheduled(TEXT("sch-1"), [](TFlockResult<FFlockScheduledNotification>) {});
	TestEqual(TEXT("cancelling forgets it"), F.Provider->GetPendingSchedules().Num(), 0);

	Cleanup(F.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationDropsElapsedTest, "Flock.Notification.Pending.ElapsedEntriesAreDropped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationDropsElapsedTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();

	// Delivery can only be inferred from the clock — there is no route to ask — so a schedule whose time
	// has passed stops counting as pending.
	F.Fake->On(TEXT("notification/schedule"), FFlockFakeTransport::Ok(ScheduledBody(false, PastIso())));
	F.Provider->ScheduleByTemplateName(TEXT("DailyBonus"), FDateTime::UtcNow() + FTimespan::FromHours(2),
		FFlockCommandData(), {}, [](TFlockResult<FFlockScheduledNotification>) {});

	TestEqual(TEXT("an already-delivered schedule is not pending"), F.Provider->GetPendingSchedules().Num(), 0);

	// The same thing again in the shape the real backend actually echoes — microsecond fraction, no
	// timezone suffix. The `Z` form above would pass even if this one silently never elapsed.
	F.Fake->On(TEXT("notification/schedule"),
		FFlockFakeTransport::Ok(ScheduledBody(false, PastIsoServerShape(), TEXT("sch-srv"))));
	F.Provider->ScheduleByTemplateName(TEXT("DailyBonus"), FDateTime::UtcNow() + FTimespan::FromHours(2),
		FFlockCommandData(), {}, [](TFlockResult<FFlockScheduledNotification>) {});

	TestEqual(TEXT("the backend's own timestamp shape elapses too"), F.Provider->GetPendingSchedules().Num(), 0);

	// An unparseable deliver_at goes the other way and is kept: losing the only handle on a cancellable
	// reminder is worse than carrying a stale row.
	F.Fake->On(TEXT("notification/schedule"), FFlockFakeTransport::Ok(ScheduledBody(false, TEXT("not-a-date"), TEXT("sch-2"))));
	F.Provider->ScheduleByTemplateName(TEXT("DailyBonus"), FDateTime::UtcNow() + FTimespan::FromHours(2),
		FFlockCommandData(), {}, [](TFlockResult<FFlockScheduledNotification>) {});

	TestEqual(TEXT("an undateable entry is kept, not dropped"), F.Provider->GetPendingSchedules().Num(), 1);

	Cleanup(F.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationClearCacheKeepsPendingTest, "Flock.Notification.Pending.ClearCacheKeepsPending",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationClearCacheKeepsPendingTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();
	F.Provider->ScheduleByTemplateName(TEXT("DailyBonus"), FDateTime::UtcNow() + FTimespan::FromHours(2),
		FFlockCommandData(), {}, [](TFlockResult<FFlockScheduledNotification>) {});
	TestEqual(TEXT("tracked"), F.Provider->GetPendingSchedules().Num(), 1);

	// Logout drops the inbox rows, but not this: the server has no route that could tell us again, so
	// dropping it strands a reminder nothing can cancel.
	F.Provider->ClearCache();
	TestEqual(TEXT("the pending list survives ClearCache"), F.Provider->GetPendingSchedules().Num(), 1);

	Cleanup(F.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationPendingPlayerScopedTest, "Flock.Notification.Pending.IsPlayerScoped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationPendingPlayerScopedTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(TEXT("player-a"));
	F.Provider->ScheduleByTemplateName(TEXT("DailyBonus"), FDateTime::UtcNow() + FTimespan::FromHours(2),
		FFlockCommandData(), {}, [](TFlockResult<FFlockScheduledNotification>) {});
	TestEqual(TEXT("A has one"), F.Provider->GetPendingSchedules().Num(), 1);

	// A second player on the same device must not see — or be able to cancel — the first player's reminders.
	F.SignIn(TEXT("player-b"));
	TestEqual(TEXT("B sees none of A's"), F.Provider->GetPendingSchedules().Num(), 0);

	F.SignIn(TEXT("player-a"));
	TestEqual(TEXT("and A still has theirs"), F.Provider->GetPendingSchedules().Num(), 1);

	Cleanup(F.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationCancelAllTest, "Flock.Notification.Pending.CancelAllDropsPermanentlyRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationCancelAllTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();

	// Two schedules, then make one of them unknown to the server — delivered, already cancelled, or simply
	// gone. Route order matters: the specific id must be registered before the general prefix.
	F.Provider->ScheduleByTemplateName(TEXT("DailyBonus"), FDateTime::UtcNow() + FTimespan::FromHours(2),
		FFlockCommandData(), {}, [](TFlockResult<FFlockScheduledNotification>) {});
	F.Fake->On(TEXT("notification/schedule"), FFlockFakeTransport::Ok(ScheduledBody(false, FString(), TEXT("sch-2"))));
	F.Provider->ScheduleByTemplateName(TEXT("DailyBonus"), FDateTime::UtcNow() + FTimespan::FromHours(2),
		FFlockCommandData(), {}, [](TFlockResult<FFlockScheduledNotification>) {});
	TestEqual(TEXT("two tracked"), F.Provider->GetPendingSchedules().Num(), 2);

	// The server reports both as pending — that list, not the local one, is what gets cancelled now.
	F.RouteScheduleList({ TEXT("sch-1"), TEXT("sch-2") });
	F.RouteCancel(TEXT("sch-2"), FFlockFakeTransport::Status(404, TEXT("{\"detail\":\"gone\"}")));
	F.RouteCancel(TEXT("sch-1"), FFlockFakeTransport::Ok(ScheduledBody(/*bCanceled*/ true)));

	bool bDone = false;
	int32 Count = -1;
	F.Provider->CancelAllScheduled([&](TFlockResult<int32> Result)
	{
		bDone = true;
		TestTrue(TEXT("the batch succeeds despite the 404"), Result.bSuccess);
		Count = Result.Value;
	});

	TestTrue(TEXT("completed"), bDone);
	// A row the server no longer recognises is not pending either way, so it is dropped rather than
	// failing the batch — but it is not counted as cancelled, because nothing was.
	TestEqual(TEXT("only the real cancel is counted"), Count, 1);
	TestEqual(TEXT("both entries are gone from the list"), F.Provider->GetPendingSchedules().Num(), 0);

	Cleanup(F.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationCancelAllTransientTest, "Flock.Notification.Pending.CancelAllKeepsEntriesOnTransientFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationCancelAllTransientTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();
	F.Provider->ScheduleByTemplateName(TEXT("DailyBonus"), FDateTime::UtcNow() + FTimespan::FromHours(2),
		FFlockCommandData(), {}, [](TFlockResult<FFlockScheduledNotification>) {});

	// A 500 is not an authoritative answer about the schedule, so the entry must stay tracked for a retry
	// rather than being silently forgotten.
	F.RouteScheduleList({ TEXT("sch-1") });
	F.RouteCancel(TEXT("sch-1"), FFlockFakeTransport::Status(500, TEXT("{\"detail\":\"boom\"}")));

	bool bDone = false;
	F.Provider->CancelAllScheduled([&](TFlockResult<int32> Result)
	{
		bDone = true;
		TestFalse(TEXT("a transient failure stops the batch"), Result.bSuccess);
	});

	TestTrue(TEXT("completed"), bDone);
	TestEqual(TEXT("the entry is still tracked, so a later call can retry it"),
		F.Provider->GetPendingSchedules().Num(), 1);

	Cleanup(F.Dir);
	return true;
}

// ── The schedule listing: the route exists now, so the SDK stops guessing ──
// Bare-shaped on purpose. GetPaged descends into `result` only when it is present, so an enveloped
// fixture would parse too and would prove nothing about what the server actually sends.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationGetScheduledTest, "Flock.Notification.Schedule.GetScheduledParsesBarePage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationGetScheduledTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();
	F.RouteScheduleList({ TEXT("sch-1"), TEXT("sch-7") });

	FFlockScheduledNotificationPage Page;
	bool bDone = false;
	F.Provider->GetScheduled([&](TFlockResult<FFlockScheduledNotificationPage> Result)
	{
		bDone = Result.bSuccess;
		Page = Result.Value;
	});

	TestTrue(TEXT("the listing succeeds"), bDone);
	if (TestEqual(TEXT("both rows parsed from the bare page"), Page.Items.Num(), 2))
	{
		TestEqual(TEXT("first id"), FlockTestAt(Page.Items, 0).Id, FString(TEXT("sch-1")));
		TestEqual(TEXT("second id"), FlockTestAt(Page.Items, 1).Id, FString(TEXT("sch-7")));
		TestTrue(TEXT("and a row still reads its state off the timestamps"), FlockTestAt(Page.Items, 0).IsPending());
	}
	TestEqual(TEXT("page metadata came from the root, not from Items.Num()"), Page.Total, 2);
	TestEqual(TEXT("limit echoed"), Page.Limit, 100);

	// The defaults the SDK sends: pending, first page of 100.
	const FString Url = F.LastUrlContaining(TEXT("notification/schedule?"));
	TestTrue(TEXT("status filter sent"), Url.Contains(TEXT("status=pending"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("page sent"), Url.Contains(TEXT("page=1")));
	TestTrue(TEXT("limit sent"), Url.Contains(TEXT("limit=100")));

	Cleanup(F.Dir);
	return true;
}

// ── An empty status omits the filter rather than sending a blank one ──
// A blank `status=` leaves the server to guess whether it meant "all" or "none"; omitting it is the
// SDK's standing convention for optional query parameters.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationGetScheduledStatusTest, "Flock.Notification.Schedule.StatusIsAnOpenStringFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationGetScheduledStatusTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();
	F.RouteScheduleList({ TEXT("sch-1") });

	F.Provider->GetScheduled(FlockScheduledNotificationStatuses::Delivered, 2, 25,
		[](TFlockResult<FFlockScheduledNotificationPage>) {});
	FString Url = F.LastUrlContaining(TEXT("notification/schedule?"));
	TestTrue(TEXT("a non-default status is sent verbatim"), Url.Contains(TEXT("status=delivered"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("with the requested page"), Url.Contains(TEXT("page=2")));
	TestTrue(TEXT("and limit"), Url.Contains(TEXT("limit=25")));

	// A status the server adds later is just a string — nothing here can reject it, which is the whole
	// reason this is not an enum.
	F.Provider->GetScheduled(TEXT("expired"), 1, 100, [](TFlockResult<FFlockScheduledNotificationPage>) {});
	Url = F.LastUrlContaining(TEXT("notification/schedule?"));
	TestTrue(TEXT("an unknown status is passed through untouched"), Url.Contains(TEXT("status=expired"), ESearchCase::CaseSensitive));

	F.Provider->GetScheduled(FString(), 1, 100, [](TFlockResult<FFlockScheduledNotificationPage>) {});
	Url = F.LastUrlContaining(TEXT("notification/schedule?"));
	TestFalse(TEXT("an empty status is omitted, never sent blank"), Url.Contains(TEXT("status=")));

	Cleanup(F.Dir);
	return true;
}

// ── Player-scoped by its own schema, so it fails fast when signed out ──
// Same carve-out as the rest of this provider: read off the schema, not inferred from an observed 401.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationGetScheduledAuthTest, "Flock.Notification.Schedule.GetScheduledRequiresSignIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationGetScheduledAuthTest::RunTest(const FString& Parameters)
{
	FFixture F;

	bool bFailed = false;
	F.Provider->GetScheduled([&](TFlockResult<FFlockScheduledNotificationPage> Result)
	{
		bFailed = !Result.bSuccess && Result.Error.Type == EFlockErrorType::Auth;
	});

	TestTrue(TEXT("signed out is an Auth failure"), bFailed);
	TestEqual(TEXT("and no request was spent"), F.Fake->CountTo(TEXT("notification/schedule?")), 0);

	Cleanup(F.Dir);
	return true;
}

// ── The point of the feature: CancelAllScheduled cancels what the SERVER lists ──
// A reinstall or a second device leaves reminders this install never wrote down, and they still fire
// server-side with their id as the only handle. Cancelling only the local list stranded them forever.
// sch-99 is exactly that case: the server knows it, this install does not.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationCancelAllUsesServerListTest, "Flock.Notification.Schedule.CancelAllCancelsServerList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationCancelAllUsesServerListTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();

	// Nothing was scheduled through this install, so the local list is empty — the pre-feature behaviour
	// would report zero cancelled and leave the reminder to fire.
	TestEqual(TEXT("this install tracked nothing"), F.Provider->GetPendingSchedules().Num(), 0);

	F.RouteScheduleList({ TEXT("sch-99") });
	F.RouteCancel(TEXT("sch-99"), FFlockFakeTransport::Ok(ScheduledBody(/*bCanceled*/ true)));

	bool bDone = false;
	int32 Count = -1;
	F.Provider->CancelAllScheduled([&](TFlockResult<int32> Result)
	{
		bDone = true;
		TestTrue(TEXT("the batch succeeds"), Result.bSuccess);
		Count = Result.Value;
	});

	TestTrue(TEXT("completed"), bDone);
	TestEqual(TEXT("a reminder this install never saw is still cancelled"), Count, 1);
	TestEqual(TEXT("and it was cancelled by id"), F.Fake->CountTo(TEXT("notification/schedule/sch-99")), 1);

	Cleanup(F.Dir);
	return true;
}

// ── The server's list is authoritative, not merged with the local one ──
// A locally-tracked id the server does not list is one it no longer considers pending: cancelling it
// anyway would spend a request to be told 404. Merging the two lists is the tempting wrong answer here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationCancelAllNotMergedTest, "Flock.Notification.Schedule.CancelAllDoesNotMergeLocalList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationCancelAllNotMergedTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();

	// One schedule this install created and still tracks locally.
	F.Provider->ScheduleByTemplateName(TEXT("DailyBonus"), FDateTime::UtcNow() + FTimespan::FromHours(2),
		FFlockCommandData(), {}, [](TFlockResult<FFlockScheduledNotification>) {});
	TestEqual(TEXT("tracked locally"), F.Provider->GetPendingSchedules().Num(), 1);

	// The server says something else entirely is pending — sch-1 has been delivered since, and it has one
	// this install has never heard of.
	F.RouteScheduleList({ TEXT("sch-42") });
	F.RouteCancel(TEXT("sch-42"), FFlockFakeTransport::Ok(ScheduledBody(/*bCanceled*/ true)));

	bool bDone = false;
	int32 Count = -1;
	F.Provider->CancelAllScheduled([&](TFlockResult<int32> Result) { bDone = true; Count = Result.Value; });

	TestTrue(TEXT("completed"), bDone);
	TestEqual(TEXT("only the server's entry was cancelled"), Count, 1);
	TestEqual(TEXT("the server's id was cancelled"), F.Fake->CountTo(TEXT("notification/schedule/sch-42")), 1);
	// The local id is never sent: it is not in the server's answer, so it is not pending.
	TestEqual(TEXT("the stale local id is not also cancelled"), F.Fake->CountTo(TEXT("notification/schedule/sch-1")), 0);

	Cleanup(F.Dir);
	return true;
}

// ── Only a failed read falls back to local bookkeeping ──
// The fallback is what keeps the old behaviour available when the network is down; it is a floor, not a
// merge, and it must not engage while the server is answering.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationCancelAllFallsBackTest, "Flock.Notification.Schedule.CancelAllFallsBackToLocalWhenReadFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationCancelAllFallsBackTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();

	F.Provider->ScheduleByTemplateName(TEXT("DailyBonus"), FDateTime::UtcNow() + FTimespan::FromHours(2),
		FFlockCommandData(), {}, [](TFlockResult<FFlockScheduledNotification>) {});
	TestEqual(TEXT("tracked locally"), F.Provider->GetPendingSchedules().Num(), 1);

	// The listing is unreachable, so the local list is all there is to go on.
	F.RouteScheduleListFailure(FFlockFakeTransport::Offline());
	F.RouteCancel(TEXT("sch-1"), FFlockFakeTransport::Ok(ScheduledBody(/*bCanceled*/ true)));

	bool bDone = false;
	int32 Count = -1;
	F.Provider->CancelAllScheduled([&](TFlockResult<int32> Result)
	{
		bDone = true;
		TestTrue(TEXT("a failed listing does not fail the whole call"), Result.bSuccess);
		Count = Result.Value;
	});

	TestTrue(TEXT("completed"), bDone);
	TestEqual(TEXT("the locally tracked reminder is still cancelled"), Count, 1);
	TestEqual(TEXT("by id"), F.Fake->CountTo(TEXT("notification/schedule/sch-1")), 1);
	TestEqual(TEXT("and it is no longer tracked"), F.Provider->GetPendingSchedules().Num(), 0);

	Cleanup(F.Dir);
	return true;
}

// ── An empty server list is a clean success, and cancels nothing ──
// The caller asked for an end state that already holds. It must not fall through to the local list,
// which is the difference between "the server says nothing is pending" and "the server did not answer".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockNotificationCancelAllEmptyServerListTest, "Flock.Notification.Schedule.CancelAllEmptyServerListCancelsNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockNotificationCancelAllEmptyServerListTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn();

	// Locally tracked, but the server reports nothing pending — it has already been delivered.
	F.Provider->ScheduleByTemplateName(TEXT("DailyBonus"), FDateTime::UtcNow() + FTimespan::FromHours(2),
		FFlockCommandData(), {}, [](TFlockResult<FFlockScheduledNotification>) {});
	F.RouteScheduleList({});

	bool bDone = false;
	int32 Count = -1;
	F.Provider->CancelAllScheduled([&](TFlockResult<int32> Result)
	{
		bDone = true;
		TestTrue(TEXT("an empty list is a success"), Result.bSuccess);
		Count = Result.Value;
	});

	TestTrue(TEXT("completed"), bDone);
	TestEqual(TEXT("nothing was cancelled"), Count, 0);
	TestEqual(TEXT("and no cancel was attempted for the stale local entry"),
		F.Fake->CountTo(TEXT("notification/schedule/sch-1")), 0);

	Cleanup(F.Dir);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
