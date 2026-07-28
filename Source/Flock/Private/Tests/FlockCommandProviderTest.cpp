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
#include "Providers/FlockCommandProvider.h"
#include "Providers/FlockPlayerProvider.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"

namespace FlockCommandProviderTestHelpers
{
	inline FFlockRetryPolicy NoRetry()
	{
		FFlockRetryPolicy Policy;
		Policy.MaxRetries = 0;
		return Policy;
	}

	/** A policy that *would* retry, so a test can prove a non-idempotent call still goes out exactly once. */
	inline FFlockRetryPolicy WithRetries()
	{
		FFlockRetryPolicy Policy;
		Policy.MaxRetries = 2;
		Policy.InitialDelaySeconds = 0.f;
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
			FString::Printf(TEXT("command_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	// ── Wire shapes. The game_command routes are BARE: the row sits at the root, not under `result`. The
	// player reads that prime the cache are enveloped (templates) / paginated (data), so the fixtures
	// mirror each route's real shape rather than a single enveloped stand-in. ──

	inline FString PlayerDataObj(const FString& Id, const FString& TemplateId, const FString& PlayerId, int32 Coins)
	{
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"player_template_id\":\"%s\",\"game_id\":\"g\",\"player_id\":\"%s\",")
			TEXT("\"data\":[{\"type\":\"int\",\"field_name\":\"coins\",\"value\":%d}],\"created_at\":\"\",\"updated_at\":\"\"}"),
			*Id, *TemplateId, *PlayerId, Coins);
	}

	inline FString TemplateObj(const FString& Id, const FString& Name, const FString& Tag)
	{
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"name\":\"%s\",\"game_version_id\":\"ver-1\",\"tag\":\"%s\",")
			TEXT("\"schema\":[],\"data\":[]}"), *Id, *Name, *Tag);
	}

	inline FString Enveloped(const FString& ResultJson)
	{
		return FString::Printf(TEXT("{\"error\":null,\"response\":null,\"result\":%s}"), *ResultJson);
	}

	inline FString OnePage(const FString& RowJson)
	{
		return FString::Printf(TEXT("{\"items\":[%s],\"total\":1,\"page\":1,\"limit\":100}"), *RowJson);
	}

	struct FFixture
	{
		FString Dir;
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockHttpClient> Client;
		TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();
		TSharedRef<FFlockAuthSession> Session;
		TSharedPtr<FFlockSnapshotStore> Snapshot;
		TSharedPtr<FFlockPlayerProvider> Players;
		TSharedPtr<FFlockCommandProvider> Commands;
		bool bReachable = true;

		explicit FFixture(const FFlockRetryPolicy& Policy = NoRetry(), const FString& ExistingDir = FString())
			: Dir(ExistingDir.IsEmpty() ? TempRoot() : ExistingDir)
			, Client(MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>()))
			, Session(MakeShared<FFlockAuthSession>(Client, Store, MakeShared<FFlockNullLogger>(),
				TEXT("http://x/v1"), TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") } }))
		{
			Snapshot = MakeShared<FFlockSnapshotStore>(Dir, MakeShared<FFlockNullLogger>(), TEXT("9.9.9"));
			Players = MakeShared<FFlockPlayerProvider>(Client, Policy, MakeShared<FFlockNullLogger>(),
				Session, TEXT("http://x/v1"), Snapshot, TEXT("ver-1"));
			Commands = MakeShared<FFlockCommandProvider>(Client, Policy, MakeShared<FFlockNullLogger>(),
				Session, TEXT("http://x/v1"), Snapshot, TEXT("ver-1"));
			Commands->SetPlayerProvider(Players);
			Commands->SetReachabilityProbe([this]() { return bReachable; });
		}

		void SignIn(const FString& PlayerId = TEXT("player-a"))
		{
			FString Error;
			Session->SetTokens(MakeTestJwt(PlayerId), TEXT("r-1"), Error);
		}

		/**
		 * Routes the player reads and pulls the signed-in player's rows into the cache, which is what the
		 * write-through and the offline overlay both act on. Registered before the bare "player_data" read
		 * route so the game_command URLs (which contain "player_data" too) can't be swallowed by it.
		 */
		void PrimePlayerCache(const FString& PlayerId = TEXT("player-a"), int32 Coins = 100)
		{
			Fake->On(TEXT("v1/player_data"), FFlockFakeTransport::Ok(
				OnePage(PlayerDataObj(TEXT("pd-1"), TEXT("tmpl-1"), PlayerId, Coins))));
			bool bDone = false;
			Players->GetMyDataByTemplate(TEXT("tmpl-1"), [&](TFlockResult<FFlockPlayerData> R) { bDone = R.bSuccess; });
			check(bDone);
		}

		/** The coins value currently cached for the signed-in player's tmpl-1 row; -1 when it isn't cached. */
		int32 CachedCoins()
		{
			FFlockPlayerData Row;
			if (!Players->TryGetCachedRow(TEXT("pd-1"), Row))
			{
				return -1;
			}
			int32 Coins = 0;
			return Row.Data.TryGetInt(TEXT("Coins"), Coins) ? Coins : -1;
		}

		/** The body of the last request sent to a URL fragment; empty when none was. */
		FString LastBodyTo(const FString& Fragment) const
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
	};

	inline void Cleanup(const FString& Dir)
	{
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
	}

	/**
	 * Runs the core ticker so the retry handler's scheduled attempts actually fire. Without this a retry is
	 * merely queued and every call looks un-retried, which would make a "not retried" assertion vacuous.
	 */
	inline void PumpRetries()
	{
		for (int32 Index = 0; Index < 8; ++Index)
		{
			FTSTicker::GetCoreTicker().Tick(1.f);
		}
	}
}

using namespace FlockCommandProviderTestHelpers;

// ── A generic update posts the flat {field: value} body and writes the returned row into the cache ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandUpdateDataTest, "Flock.Command.Provider.UpdatePlayerDataPostsFlatBodyAndWritesThrough",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandUpdateDataTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.Fake->On(TEXT("game_command/update_player_data"), FFlockFakeTransport::Ok(
		PlayerDataObj(TEXT("pd-1"), TEXT("tmpl-1"), TEXT("player-a"), 250)));
	Fx.PrimePlayerCache();
	TestEqual(TEXT("cache primed"), Fx.CachedCoins(), 100);

	bool bDone = false;
	FFlockPlayerData Row;
	Fx.Commands->UpdatePlayerData(TEXT("pd-1"),
		FFlockCommandData().Set(TEXT("coins"), 250).Set(TEXT("prestige"), true).Set(TEXT("title"), TEXT("Champion")),
		[&](TFlockResult<FFlockPlayerData> R) { bDone = R.bSuccess; Row = R.Value; });

	TestTrue(TEXT("update succeeds"), bDone);
	const FString Body = Fx.LastBodyTo(TEXT("game_command/update_player_data"));
	TestTrue(TEXT("targets the row"), Body.Contains(TEXT("\"player_data_id\":\"pd-1\"")));
	// Types are preserved: an int is not quoted, a bool is not the string "true".
	TestTrue(TEXT("int stays an int"), Body.Contains(TEXT("\"coins\":250")));
	TestTrue(TEXT("bool stays a bool"), Body.Contains(TEXT("\"prestige\":true")));
	TestTrue(TEXT("string is quoted"), Body.Contains(TEXT("\"title\":\"Champion\"")));
	// Bare route: the row parsed from the root, not from an envelope.
	TestEqual(TEXT("returned row id"), Row.Id, FString(TEXT("pd-1")));
	TestEqual(TEXT("cache written through"), Fx.CachedCoins(), 250);

	Cleanup(Fx.Dir);
	return true;
}

// ── A single-field update sends the value untouched by any key transform ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandUpdateFieldTest, "Flock.Command.Provider.UpdatePlayerDataFieldSendsTypedValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandUpdateFieldTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.Fake->On(TEXT("game_command/update_player_data_key"), FFlockFakeTransport::Ok(
		PlayerDataObj(TEXT("pd-1"), TEXT("tmpl-1"), TEXT("player-a"), 7)));

	bool bDone = false;
	Fx.Commands->UpdatePlayerDataField(TEXT("pd-1"), TEXT("max_health"), 7,
		[&](TFlockResult<FFlockPlayerData> R) { bDone = R.bSuccess; });

	TestTrue(TEXT("field update succeeds"), bDone);
	const FString Body = Fx.LastBodyTo(TEXT("game_command/update_player_data_key"));
	// The author's key goes out verbatim — only they know what the template declares.
	TestTrue(TEXT("key verbatim"), Body.Contains(TEXT("\"key\":\"max_health\"")));
	TestTrue(TEXT("value typed"), Body.Contains(TEXT("\"value\":7")));

	Cleanup(Fx.Dir);
	return true;
}

// ── A string literal binds to the string overload, not to bool (the const TCHAR* guard) ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandLiteralOverloadTest, "Flock.Command.Provider.StringLiteralDoesNotBindToBool",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandLiteralOverloadTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.Fake->On(TEXT("game_command/update_player_data_key"), FFlockFakeTransport::Ok(
		PlayerDataObj(TEXT("pd-1"), TEXT("tmpl-1"), TEXT("player-a"), 0)));

	Fx.Commands->UpdatePlayerDataField(TEXT("pd-1"), TEXT("rank"), TEXT("gold"), nullptr);

	const FString Body = Fx.LastBodyTo(TEXT("game_command/update_player_data_key"));
	TestTrue(TEXT("literal stays a string"), Body.Contains(TEXT("\"value\":\"gold\"")));
	TestFalse(TEXT("literal did not become true"), Body.Contains(TEXT("\"value\":true")));

	Cleanup(Fx.Dir);
	return true;
}

// ── Add game funds resolves the wallet row from the currency template and posts against it ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandAddFundsTest, "Flock.Command.Provider.AddGameFundsResolvesWalletRow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandAddFundsTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.Fake->On(TEXT("game_command/add_game_funds"), FFlockFakeTransport::Ok(
		PlayerDataObj(TEXT("pd-1"), TEXT("tmpl-1"), TEXT("player-a"), 350)));
	Fx.Fake->On(TEXT("player_template"), FFlockFakeTransport::Ok(
		Enveloped(FString::Printf(TEXT("[%s]"), *TemplateObj(TEXT("tmpl-1"), TEXT("Wallet"), TEXT("currency"))))));
	Fx.PrimePlayerCache();

	bool bDone = false;
	Fx.Commands->AddGameFunds(TEXT("coins"), 250, [&](TFlockResult<FFlockPlayerData> R) { bDone = R.bSuccess; });

	TestTrue(TEXT("add funds succeeds"), bDone);
	const FString Body = Fx.LastBodyTo(TEXT("game_command/add_game_funds"));
	// No player-data id is passed in: it comes from the "currency"-tagged template's row.
	TestTrue(TEXT("wallet row resolved"), Body.Contains(TEXT("\"player_data_id\":\"pd-1\"")));
	TestTrue(TEXT("currency sent"), Body.Contains(TEXT("\"currency\":\"coins\"")));
	TestTrue(TEXT("amount sent"), Body.Contains(TEXT("\"amount\":250")));
	TestEqual(TEXT("cache written through"), Fx.CachedCoins(), 350);

	Cleanup(Fx.Dir);
	return true;
}

// ── Money safety: offline, a funds grant fails outright and leaves nothing queued ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandFundsNeverQueuedTest, "Flock.Command.Provider.AddGameFundsFailsOfflineAndIsNeverQueued",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandFundsNeverQueuedTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.PrimePlayerCache();
	Fx.bReachable = false;

	bool bCompleted = false;
	FFlockError Error;
	Fx.Commands->AddGameFunds(TEXT("coins"), 250, TEXT("tmpl-1"),
		[&](TFlockResult<FFlockPlayerData> R) { bCompleted = true; Error = R.Error; TestFalse(TEXT("funds fail offline"), R.bSuccess); });

	TestTrue(TEXT("caller was told"), bCompleted);
	TestEqual(TEXT("reported as a connection failure"), Error.Type, EFlockErrorType::Connection);
	TestEqual(TEXT("nothing queued"), Fx.Commands->GetPendingWriteCount(), 0);
	TestEqual(TEXT("nothing sent"), Fx.Fake->CountTo(TEXT("add_game_funds")), 0);
	// A queueable command in the same state does queue — proving the exclusion is about money, not offline.
	Fx.Commands->UpdatePlayerData(TEXT("pd-1"), FFlockCommandData().Set(TEXT("coins"), 1), nullptr);
	TestEqual(TEXT("a data write still queues"), Fx.Commands->GetPendingWriteCount(), 1);

	Cleanup(Fx.Dir);
	return true;
}

// ── Money safety: an ambiguous failure is never re-sent, even with retries configured ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandFundsNotRetriedTest, "Flock.Command.Provider.AddGameFundsNotRetriedOnAmbiguousFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandFundsNotRetriedTest::RunTest(const FString& Parameters)
{
	FFixture Fx(WithRetries());
	Fx.SignIn();
	Fx.PrimePlayerCache();
	// 500 after the request was accepted: the credit may already have landed, so a resend could double it.
	Fx.Fake->On(TEXT("game_command/add_game_funds"), FFlockFakeTransport::Status(500, TEXT("{}")));

	bool bCompleted = false;
	Fx.Commands->AddGameFunds(TEXT("coins"), 250, TEXT("tmpl-1"),
		[&](TFlockResult<FFlockPlayerData> R) { bCompleted = true; TestFalse(TEXT("failure surfaces"), R.bSuccess); });

	TestTrue(TEXT("caller was told"), bCompleted);
	// Pumped, so a scheduled resend would have landed by now — the count is a real absence, not a pending one.
	PumpRetries();
	TestEqual(TEXT("posted exactly once"), Fx.Fake->CountTo(TEXT("add_game_funds")), 1);
	TestEqual(TEXT("still nothing queued"), Fx.Commands->GetPendingWriteCount(), 0);

	// The same policy does retry a queueable command, so the single attempt above is the money rule at work
	// and not simply a retry policy that never fires.
	Fx.Fake->On(TEXT("game_command/update_player_data_key"), FFlockFakeTransport::Status(500, TEXT("{}")));
	Fx.Commands->UpdatePlayerDataField(TEXT("pd-1"), TEXT("coins"), 1, nullptr);
	PumpRetries();
	TestTrue(TEXT("a data write is retried"), Fx.Fake->CountTo(TEXT("update_player_data_key")) > 1);
	TestEqual(TEXT("and the money call still went once"), Fx.Fake->CountTo(TEXT("add_game_funds")), 1);

	Cleanup(Fx.Dir);
	return true;
}

// ── Offline, a queueable write is persisted and the cached row is overlaid so a read-after-write is honest ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandOfflineQueueTest, "Flock.Command.Provider.OfflineWriteQueuesAndOverlaysCachedRow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandOfflineQueueTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.PrimePlayerCache();
	Fx.bReachable = false;

	bool bDone = false;
	FFlockPlayerData Row;
	Fx.Commands->UpdatePlayerData(TEXT("pd-1"), FFlockCommandData().Set(TEXT("coins"), 250),
		[&](TFlockResult<FFlockPlayerData> R) { bDone = R.bSuccess; Row = R.Value; });

	TestTrue(TEXT("offline write reports success"), bDone);
	TestEqual(TEXT("queued"), Fx.Commands->GetPendingWriteCount(), 1);
	TestEqual(TEXT("nothing was sent"), Fx.Fake->CountTo(TEXT("game_command")), 0);
	int32 Coins = 0;
	TestTrue(TEXT("returned row carries the overlay"), Row.Data.TryGetInt(TEXT("Coins"), Coins));
	TestEqual(TEXT("overlay value"), Coins, 250);
	// The author typed "coins"; the flattened key is "Coins". The overlay resolves the two to one field.
	TestEqual(TEXT("cached row overlaid in place"), Fx.CachedCoins(), 250);

	Cleanup(Fx.Dir);
	return true;
}

// ── The queue survives the provider: a later run reloads and replays it ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandQueuePersistsTest, "Flock.Command.Provider.QueueSurvivesRestartAndReplaysInOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandQueuePersistsTest::RunTest(const FString& Parameters)
{
	const FString Dir = TempRoot();
	{
		FFixture Fx(NoRetry(), Dir);
		Fx.SignIn();
		Fx.bReachable = false;
		Fx.Commands->UpdatePlayerData(TEXT("pd-1"), FFlockCommandData().Set(TEXT("coins"), 1), nullptr);
		Fx.Commands->UpdatePlayerDataField(TEXT("pd-1"), TEXT("coins"), 2, nullptr);
		TestEqual(TEXT("both queued"), Fx.Commands->GetPendingWriteCount(), 2);
	}

	FFixture Fx(NoRetry(), Dir);
	Fx.SignIn();
	TestEqual(TEXT("queue reloaded from disk"), Fx.Commands->GetPendingWriteCount(), 2);

	Fx.Fake->On(TEXT("game_command"), FFlockFakeTransport::Ok(
		PlayerDataObj(TEXT("pd-1"), TEXT("tmpl-1"), TEXT("player-a"), 2)));

	int32 Delivered = 0;
	Fx.Commands->FlushPendingWrites([&](TFlockResult<int32> R) { Delivered = R.Value; });

	TestEqual(TEXT("both replayed"), Delivered, 2);
	TestEqual(TEXT("queue drained"), Fx.Commands->GetPendingWriteCount(), 0);
	// Oldest first: the whole-bag write was queued before the single-field one.
	TestEqual(TEXT("two requests"), Fx.Fake->Requests.Num(), 2);
	TestTrue(TEXT("replayed in order"), Fx.Fake->Requests[0].Url.Contains(TEXT("update_player_data"))
		&& Fx.Fake->Requests[1].Url.Contains(TEXT("update_player_data_key")));

	Cleanup(Dir);
	return true;
}

// ── A transient failure halts the flush with the queue intact; a permanent one drops that entry and evicts ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandFlushHaltTest, "Flock.Command.Provider.FlushHaltsOnTransientKeepingOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandFlushHaltTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.bReachable = false;
	Fx.Commands->UpdatePlayerData(TEXT("pd-1"), FFlockCommandData().Set(TEXT("coins"), 1), nullptr);
	Fx.Commands->UpdatePlayerDataField(TEXT("pd-1"), TEXT("coins"), 2, nullptr);
	Fx.bReachable = true;

	Fx.Fake->On(TEXT("game_command/update_player_data"), FFlockFakeTransport::Status(503, TEXT("{}")));

	int32 Delivered = -1;
	Fx.Commands->FlushPendingWrites([&](TFlockResult<int32> R) { Delivered = R.Value; });

	TestEqual(TEXT("nothing delivered"), Delivered, 0);
	TestEqual(TEXT("both still queued"), Fx.Commands->GetPendingWriteCount(), 2);
	// The head failed, so the flush stopped there rather than reordering by skipping it.
	TestEqual(TEXT("only the head was attempted"), Fx.Fake->Requests.Num(), 1);

	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandFlushDropTest, "Flock.Command.Provider.FlushDropsRejectedWriteAndEvictsOptimisticRow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandFlushDropTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.PrimePlayerCache();
	Fx.bReachable = false;
	Fx.Commands->UpdatePlayerData(TEXT("pd-1"), FFlockCommandData().Set(TEXT("coins"), 250), nullptr);
	TestEqual(TEXT("optimistic value cached"), Fx.CachedCoins(), 250);
	Fx.bReachable = true;

	// 422: the server has authoritatively rejected it, so replaying can only fail again.
	Fx.Fake->On(TEXT("game_command/update_player_data"), FFlockFakeTransport::Status(422, TEXT("{}")));

	int32 Delivered = -1;
	Fx.Commands->FlushPendingWrites([&](TFlockResult<int32> R) { Delivered = R.Value; });

	TestEqual(TEXT("nothing delivered"), Delivered, 0);
	TestEqual(TEXT("rejected entry dropped, not stuck at the head"), Fx.Commands->GetPendingWriteCount(), 0);
	// The optimistic value was never accepted, so the row is evicted rather than left lying about state.
	TestEqual(TEXT("optimistic row evicted"), Fx.CachedCoins(), -1);

	Cleanup(Fx.Dir);
	return true;
}

// ── An auth failure is recoverable by signing in again, so the write stays queued rather than being dropped ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandFlushAuthTest, "Flock.Command.Provider.FlushKeepsWriteQueuedOnAuthFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandFlushAuthTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.bReachable = false;
	Fx.Commands->UpdatePlayerData(TEXT("pd-1"), FFlockCommandData().Set(TEXT("coins"), 1), nullptr);
	Fx.bReachable = true;

	Fx.Fake->On(TEXT("game_command/update_player_data"), FFlockFakeTransport::Status(401, TEXT("{}")));
	Fx.Fake->On(TEXT("player/token/refresh"), FFlockFakeTransport::Status(401, TEXT("{}")));

	int32 Delivered = -1;
	Fx.Commands->FlushPendingWrites([&](TFlockResult<int32> R) { Delivered = R.Value; });

	TestEqual(TEXT("nothing delivered"), Delivered, 0);
	// A rejected token can end the session, which swaps the active queue to the signed-out one. The write is
	// kept under the player it belongs to, so signing back in is what must bring it back — not a live count.
	Fx.SignIn(TEXT("player-a"));
	TestEqual(TEXT("write kept for the next sign-in"), Fx.Commands->GetPendingWriteCount(), 1);

	Cleanup(Fx.Dir);
	return true;
}

// ── A queue belongs to one player: signing in as someone else must not replay their writes ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandQueueScopeTest, "Flock.Command.Provider.QueueIsPlayerScoped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandQueueScopeTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn(TEXT("player-a"));
	Fx.bReachable = false;
	Fx.Commands->UpdatePlayerData(TEXT("pd-1"), FFlockCommandData().Set(TEXT("coins"), 1), nullptr);
	TestEqual(TEXT("queued for player-a"), Fx.Commands->GetPendingWriteCount(), 1);

	Fx.SignIn(TEXT("player-b"));
	TestEqual(TEXT("player-b starts empty"), Fx.Commands->GetPendingWriteCount(), 0);

	Fx.SignIn(TEXT("player-a"));
	TestEqual(TEXT("player-a's write is still theirs"), Fx.Commands->GetPendingWriteCount(), 1);

	Cleanup(Fx.Dir);
	return true;
}

// ── Two flushes can't run at once and post the same command twice ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandFlushSingleFlightTest, "Flock.Command.Provider.FlushIsSingleFlight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandFlushSingleFlightTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.bReachable = false;
	Fx.Commands->UpdatePlayerData(TEXT("pd-1"), FFlockCommandData().Set(TEXT("coins"), 1), nullptr);
	Fx.bReachable = true;

	Fx.Fake->On(TEXT("game_command"), FFlockFakeTransport::Ok(
		PlayerDataObj(TEXT("pd-1"), TEXT("tmpl-1"), TEXT("player-a"), 1)));

	// Re-entering from inside the first flush's own completion is the sharpest version of the race.
	int32 Reentrant = -1;
	Fx.Commands->FlushPendingWrites([&](TFlockResult<int32> R)
	{
		Fx.Commands->FlushPendingWrites([&](TFlockResult<int32> Inner) { Reentrant = Inner.Value; });
	});

	TestEqual(TEXT("re-entrant flush delivered nothing"), Reentrant, 0);
	TestEqual(TEXT("posted exactly once"), Fx.Fake->CountTo(TEXT("game_command")), 1);

	Cleanup(Fx.Dir);
	return true;
}

// ── The pump's own triggers replay the queue: returning to the foreground, and connectivity coming back ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandAutoFlushTest, "Flock.Command.Provider.PumpTriggersAutoFlush",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandAutoFlushTest::RunTest(const FString& Parameters)
{
	{
		// Foreground: queue offline, come back online, then raise background->foreground.
		FFixture Fx;
		Fx.SignIn();
		Fx.bReachable = false;
		Fx.Commands->UpdatePlayerData(TEXT("pd-1"), FFlockCommandData().Set(TEXT("coins"), 1), nullptr);
		Fx.Commands->Initialize();
		Fx.Fake->On(TEXT("game_command"), FFlockFakeTransport::Ok(
			PlayerDataObj(TEXT("pd-1"), TEXT("tmpl-1"), TEXT("player-a"), 1)));

		Fx.bReachable = true;
		Fx.Commands->GetPumpForTesting().SetBackgroundedForTesting(true);
		TestEqual(TEXT("backgrounding alone sends nothing"), Fx.Fake->CountTo(TEXT("game_command")), 0);
		Fx.Commands->GetPumpForTesting().SetBackgroundedForTesting(false);
		TestEqual(TEXT("returning to the foreground replayed it"), Fx.Commands->GetPendingWriteCount(), 0);
		Cleanup(Fx.Dir);
	}

	{
		// Reconnect: Initialize while offline so the pump's baseline is "unreachable", then flip and tick.
		FFixture Fx;
		Fx.SignIn();
		Fx.bReachable = false;
		Fx.Commands->UpdatePlayerData(TEXT("pd-1"), FFlockCommandData().Set(TEXT("coins"), 1), nullptr);
		Fx.Commands->Initialize();
		Fx.Fake->On(TEXT("game_command"), FFlockFakeTransport::Ok(
			PlayerDataObj(TEXT("pd-1"), TEXT("tmpl-1"), TEXT("player-a"), 1)));

		Fx.Commands->GetPumpForTesting().TickForTesting(0.1f);
		TestEqual(TEXT("still offline, still queued"), Fx.Commands->GetPendingWriteCount(), 1);

		Fx.bReachable = true;
		Fx.Commands->GetPumpForTesting().TickForTesting(0.1f);
		TestEqual(TEXT("the offline->online edge replayed it"), Fx.Commands->GetPendingWriteCount(), 0);

		// The edge fires once, not on every subsequent tick.
		Fx.Commands->GetPumpForTesting().TickForTesting(0.1f);
		TestEqual(TEXT("no repeat send"), Fx.Fake->CountTo(TEXT("game_command")), 1);
		Cleanup(Fx.Dir);
	}

	return true;
}

// ── Unlocking an achievement resolves the "achievement"-tagged row, so no id is passed in ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandUnlockTest, "Flock.Command.Provider.UnlockAchievementResolvesTaggedRow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandUnlockTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	Fx.Fake->On(TEXT("game_command/unlock_achievement"), FFlockFakeTransport::Ok(
		PlayerDataObj(TEXT("pd-2"), TEXT("tmpl-2"), TEXT("player-a"), 0)));
	Fx.Fake->On(TEXT("player_template"), FFlockFakeTransport::Ok(
		Enveloped(FString::Printf(TEXT("[%s]"), *TemplateObj(TEXT("tmpl-2"), TEXT("Trophies"), TEXT("achievement"))))));
	Fx.Fake->On(TEXT("v1/player_data"), FFlockFakeTransport::Ok(
		OnePage(PlayerDataObj(TEXT("pd-2"), TEXT("tmpl-2"), TEXT("player-a"), 0))));

	bool bDone = false;
	Fx.Commands->UnlockAchievement(TEXT("first_blood"), [&](TFlockResult<FFlockPlayerData> R) { bDone = R.bSuccess; });

	TestTrue(TEXT("unlock succeeds"), bDone);
	const FString Body = Fx.LastBodyTo(TEXT("game_command/unlock_achievement"));
	TestTrue(TEXT("row resolved from the tag"), Body.Contains(TEXT("\"player_data_id\":\"pd-2\"")));
	TestTrue(TEXT("achievement name sent"), Body.Contains(TEXT("\"achievement_name\":\"first_blood\"")));

	Cleanup(Fx.Dir);
	return true;
}

// ── Guards: an empty required argument fails as Validation without touching the network ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandGuardTest, "Flock.Command.Provider.EmptyArgumentsFailAsValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandGuardTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();

	EFlockErrorType NoRow = EFlockErrorType::None;
	Fx.Commands->UpdatePlayerData(FString(), FFlockCommandData().Set(TEXT("coins"), 1),
		[&](TFlockResult<FFlockPlayerData> R) { NoRow = R.Error.Type; });
	TestEqual(TEXT("missing row id"), NoRow, EFlockErrorType::Validation);

	EFlockErrorType NoKey = EFlockErrorType::None;
	Fx.Commands->UpdatePlayerDataField(TEXT("pd-1"), FString(), 1,
		[&](TFlockResult<FFlockPlayerData> R) { NoKey = R.Error.Type; });
	TestEqual(TEXT("missing key"), NoKey, EFlockErrorType::Validation);

	EFlockErrorType NoCurrency = EFlockErrorType::None;
	Fx.Commands->AddGameFunds(FString(), 1, TEXT("tmpl-1"),
		[&](TFlockResult<FFlockPlayerData> R) { NoCurrency = R.Error.Type; });
	TestEqual(TEXT("missing currency"), NoCurrency, EFlockErrorType::Validation);

	EFlockErrorType NoName = EFlockErrorType::None;
	Fx.Commands->UnlockAchievement(FString(), [&](TFlockResult<FFlockPlayerData> R) { NoName = R.Error.Type; });
	TestEqual(TEXT("missing achievement name"), NoName, EFlockErrorType::Validation);

	TestEqual(TEXT("nothing was sent"), Fx.Fake->Requests.Num(), 0);

	Cleanup(Fx.Dir);
	return true;
}

// ── Clearing the queue discards without sending ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCommandClearQueueTest, "Flock.Command.Provider.ClearPendingWritesDiscardsWithoutSending",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCommandClearQueueTest::RunTest(const FString& Parameters)
{
	const FString Dir = TempRoot();
	{
		FFixture Fx(NoRetry(), Dir);
		Fx.SignIn();
		Fx.bReachable = false;
		Fx.Commands->UpdatePlayerData(TEXT("pd-1"), FFlockCommandData().Set(TEXT("coins"), 1), nullptr);
		Fx.Commands->ClearPendingWrites();
		TestEqual(TEXT("cleared in memory"), Fx.Commands->GetPendingWriteCount(), 0);
		TestEqual(TEXT("nothing sent"), Fx.Fake->Requests.Num(), 0);
	}

	FFixture Fx(NoRetry(), Dir);
	Fx.SignIn();
	TestEqual(TEXT("cleared on disk too"), Fx.Commands->GetPendingWriteCount(), 0);

	Cleanup(Dir);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
