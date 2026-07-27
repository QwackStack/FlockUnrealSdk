// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockAuthSession.h"
#include "FlockLogger.h"
#include "HAL/FileManager.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockSnapshotStore.h"
#include "Misc/Base64.h"
#include "Misc/Paths.h"
#include "Providers/FlockPlayerProvider.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"

namespace FlockPlayerProviderTestHelpers
{
	inline FFlockRetryPolicy NoRetry()
	{
		FFlockRetryPolicy Policy;
		Policy.MaxRetries = 0;
		return Policy;
	}

	/** Minimal signed-in-looking token so the auth session reports a player id. */
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
			FString::Printf(TEXT("player_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	// ── Real wire shapes: templates/data/ban are enveloped ({error,response,result}); the data list is the
	// bare paginated {items,total,page,limit}. Fixtures mirror the real shape, not enveloped stand-ins. ──

	inline FString TemplateObj(const FString& Id, const FString& Name, const FString& Tag)
	{
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"name\":\"%s\",\"game_version_id\":\"ver-1\",\"tag\":\"%s\",")
			TEXT("\"schema\":[{\"type\":\"int\",\"field_name\":\"coins\",\"type_name\":\"int\"}],")
			TEXT("\"data\":[{\"type\":\"int\",\"field_name\":\"coins\",\"value\":100}]}"),
			*Id, *Name, *Tag);
	}

	inline FString PlayerDataObj(const FString& Id, const FString& TemplateId, int32 Coins)
	{
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"player_template_id\":\"%s\",\"game_id\":\"g\",\"player_id\":\"player-a\",")
			TEXT("\"data\":[{\"type\":\"int\",\"field_name\":\"coins\",\"value\":%d}],\"created_at\":\"\",\"updated_at\":\"\"}"),
			*Id, *TemplateId, Coins);
	}

	inline FString BanObj()
	{
		return TEXT("{\"id\":\"ban-1\",\"player_id\":\"player-a\",\"game_id\":\"g\",")
			TEXT("\"data\":{\"currency\":{\"reason\":\"cheat\",\"ban_duration\":\"7d\",\"effective_datetime\":\"t\"}},")
			TEXT("\"created_at\":\"\",\"updated_at\":\"\"}");
	}

	inline FString Enveloped(const FString& ResultJson)
	{
		return FString::Printf(TEXT("{\"error\":null,\"response\":null,\"result\":%s}"), *ResultJson);
	}

	/** A paginated data page ({items,total,page,limit}) of Count rows, ids/templates indexed from StartIndex. */
	inline FString PlayerDataPage(int32 StartIndex, int32 Count, int32 Total, int32 Page, int32 Limit)
	{
		FString Items;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const int32 N = StartIndex + Index;
			if (Index > 0)
			{
				Items += TEXT(",");
			}
			Items += PlayerDataObj(FString::Printf(TEXT("pd-%d"), N), FString::Printf(TEXT("t%d"), N), N);
		}
		return FString::Printf(TEXT("{\"items\":[%s],\"total\":%d,\"page\":%d,\"limit\":%d}"), *Items, Total, Page, Limit);
	}

	struct FFixture
	{
		FString Dir;
		FString ApiUrl;
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockHttpClient> Client;
		TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();
		TSharedRef<FFlockAuthSession> Session;
		TSharedPtr<FFlockSnapshotStore> Snapshot;
		TSharedPtr<FFlockPlayerProvider> Provider;

		explicit FFixture(const FFlockRetryPolicy& Policy = NoRetry(), const FString& ExistingDir = FString(),
			const FString& InApiUrl = TEXT("http://x/v1"))
			: Dir(ExistingDir.IsEmpty() ? TempRoot() : ExistingDir)
			, ApiUrl(InApiUrl)
			, Client(MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>()))
			, Session(MakeShared<FFlockAuthSession>(Client, Store, MakeShared<FFlockNullLogger>(),
				InApiUrl, TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") } }))
		{
			Snapshot = MakeShared<FFlockSnapshotStore>(Dir, MakeShared<FFlockNullLogger>(), TEXT("9.9.9"));
			Provider = MakeShared<FFlockPlayerProvider>(Client, Policy, MakeShared<FFlockNullLogger>(),
				Session, InApiUrl, Snapshot, TEXT("ver-1"));
		}

		void SignIn(const FString& PlayerId = TEXT("player-a"))
		{
			FString Error;
			Session->SetTokens(MakeTestJwt(PlayerId), TEXT("r-1"), Error);
		}
	};

	inline void Cleanup(const FString& Dir)
	{
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
	}
}

using namespace FlockPlayerProviderTestHelpers;

// ── The all-templates enveloped list parses, memoizes, and populates the by-id / by-name caches ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerGetTemplatesTest, "Flock.Player.Provider.GetTemplatesListAndCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerGetTemplatesTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	const FString List = FString::Printf(TEXT("[%s,%s]"),
		*TemplateObj(TEXT("tmpl-1"), TEXT("Wallet"), TEXT("currency")),
		*TemplateObj(TEXT("tmpl-2"), TEXT("Trophies"), TEXT("achievement")));
	Fx.Fake->On(TEXT("player_template"), FFlockFakeTransport::Ok(Enveloped(List)));

	TArray<FFlockPlayerTemplateSchema> Templates;
	bool bDone = false;
	Fx.Provider->GetTemplates([&](TFlockResult<TArray<FFlockPlayerTemplateSchema>> R) { bDone = R.bSuccess; Templates = R.Value; });
	TestTrue(TEXT("templates succeed"), bDone);
	TestEqual(TEXT("two templates"), Templates.Num(), 2);

	// by-id and by-name are served from the index the list built — no extra request.
	FFlockPlayerTemplateSchema ById;
	Fx.Provider->GetTemplateById(TEXT("tmpl-1"), [&](TFlockResult<FFlockPlayerTemplateSchema> R) { ById = R.Value; });
	TestEqual(TEXT("by-id from cache"), ById.Name, FString(TEXT("Wallet")));
	FFlockPlayerTemplateSchema ByName;
	Fx.Provider->GetTemplateByName(TEXT("Trophies"), [&](TFlockResult<FFlockPlayerTemplateSchema> R) { ByName = R.Value; });
	TestEqual(TEXT("by-name from cache"), ByName.Id, FString(TEXT("tmpl-2")));

	Fx.Provider->GetTemplates([&](TFlockResult<TArray<FFlockPlayerTemplateSchema>> R) {});
	TestEqual(TEXT("everything served from one list request"), Fx.Fake->CountTo(TEXT("player_template")), 1);

	Cleanup(Fx.Dir);
	return true;
}

// ── A single enveloped template parses its flattened data and memoizes ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerGetTemplateByIdTest, "Flock.Player.Provider.GetTemplateByIdEnvelopedAndCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerGetTemplateByIdTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("player_template/tmpl-1"),
		FFlockFakeTransport::Ok(Enveloped(TemplateObj(TEXT("tmpl-1"), TEXT("Wallet"), TEXT("currency")))));

	FFlockPlayerTemplateSchema Template;
	bool bDone = false;
	Fx.Provider->GetTemplateById(TEXT("tmpl-1"), [&](TFlockResult<FFlockPlayerTemplateSchema> R) { bDone = R.bSuccess; Template = R.Value; });
	TestTrue(TEXT("template succeeds"), bDone);
	TestEqual(TEXT("id"), Template.Id, FString(TEXT("tmpl-1")));
	TestEqual(TEXT("tag stays a string"), Template.Tag, FString(TEXT("currency")));
	int32 Coins = 0;
	TestTrue(TEXT("flattened data reads"), Template.Data.TryGetInt(TEXT("Coins"), Coins));
	TestEqual(TEXT("data value"), Coins, 100);

	Fx.Provider->GetTemplateById(TEXT("tmpl-1"), [&](TFlockResult<FFlockPlayerTemplateSchema> R) {});
	TestEqual(TEXT("cache hit, one request"), Fx.Fake->CountTo(TEXT("player_template/tmpl-1")), 1);

	Cleanup(Fx.Dir);
	return true;
}

// ── The all-templates list survives the offline snapshot round-trip ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerTemplatesSnapshotTest, "Flock.Player.Provider.GetTemplatesServedFromSnapshotOffline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerTemplatesSnapshotTest::RunTest(const FString& Parameters)
{
	const FString Dir = TempRoot();
	const FString List = FString::Printf(TEXT("[%s]"), *TemplateObj(TEXT("tmpl-1"), TEXT("Wallet"), TEXT("currency")));
	{
		FFixture Warm(NoRetry(), Dir);
		Warm.Fake->On(TEXT("player_template"), FFlockFakeTransport::Ok(Enveloped(List)));
		bool bDone = false;
		Warm.Provider->GetTemplates([&](TFlockResult<TArray<FFlockPlayerTemplateSchema>> R) { bDone = R.bSuccess; });
		TestTrue(TEXT("warm fetch succeeds"), bDone);
	}
	{
		FFixture Cold(NoRetry(), Dir);
		Cold.Provider->SetReachabilityProbe([]() { return false; });
		Cold.Fake->On(TEXT("player_template"), FFlockFakeTransport::Offline());
		TArray<FFlockPlayerTemplateSchema> Templates;
		bool bServed = false;
		Cold.Provider->GetTemplates([&](TFlockResult<TArray<FFlockPlayerTemplateSchema>> R) { bServed = R.bSuccess; Templates = R.Value; });
		TestTrue(TEXT("served from snapshot offline"), bServed);
		TestEqual(TEXT("no network call"), Cold.Fake->CountTo(TEXT("player_template")), 0);
		if (TestEqual(TEXT("snapshot preserved one template"), Templates.Num(), 1))
		{
			int32 Coins = 0;
			TestTrue(TEXT("flattened data survived snapshot"), Templates[0].Data.TryGetInt(TEXT("Coins"), Coins));
			TestEqual(TEXT("data value intact"), Coins, 100);
		}
	}
	Cleanup(Dir);
	return true;
}

// ── By-tag resolves from the all-templates list; a missing tag is a Validation failure ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerTemplateByTagTest, "Flock.Player.Provider.GetTemplateByTag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerTemplateByTagTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	const FString List = FString::Printf(TEXT("[%s]"), *TemplateObj(TEXT("tmpl-1"), TEXT("Wallet"), TEXT("currency")));
	Fx.Fake->On(TEXT("player_template"), FFlockFakeTransport::Ok(Enveloped(List)));

	FFlockPlayerTemplateSchema Found;
	bool bFound = false;
	Fx.Provider->GetTemplateByTag(TEXT("currency"), [&](TFlockResult<FFlockPlayerTemplateSchema> R) { bFound = R.bSuccess; Found = R.Value; });
	TestTrue(TEXT("tag resolves"), bFound);
	TestEqual(TEXT("resolved template id"), Found.Id, FString(TEXT("tmpl-1")));

	bool bValidation = false;
	Fx.Provider->GetTemplateByTag(TEXT("nonexistent"), [&](TFlockResult<FFlockPlayerTemplateSchema> R)
		{ bValidation = !R.bSuccess && R.Error.Type == EFlockErrorType::Validation; });
	TestTrue(TEXT("missing tag -> validation"), bValidation);

	Cleanup(Fx.Dir);
	return true;
}

// ── GetMyDataByTemplate paginates all the player's rows, aggregates across pages, memoizes, and reports an
// empty record for a template with no row ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerMyDataPaginatesTest, "Flock.Player.Provider.GetMyDataByTemplatePaginates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerMyDataPaginatesTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	// Page 1 is full (100 rows t0..t99), so a second page is fetched; page 2 has the single row t100.
	Fx.Fake->On(TEXT("player_data?page=1"), FFlockFakeTransport::Ok(PlayerDataPage(0, 100, 101, 1, 100)));
	Fx.Fake->On(TEXT("player_data?page=2"), FFlockFakeTransport::Ok(PlayerDataPage(100, 1, 101, 2, 100)));

	FFlockPlayerData Row;
	bool bDone = false;
	Fx.Provider->GetMyDataByTemplate(TEXT("t100"), [&](TFlockResult<FFlockPlayerData> R) { bDone = R.bSuccess; Row = R.Value; });
	TestTrue(TEXT("my data succeeds"), bDone);
	TestEqual(TEXT("row from page 2"), Row.Id, FString(TEXT("pd-100")));
	TestEqual(TEXT("both pages fetched (p1)"), Fx.Fake->CountTo(TEXT("player_data?page=1")), 1);
	TestEqual(TEXT("both pages fetched (p2)"), Fx.Fake->CountTo(TEXT("player_data?page=2")), 1);

	// A different template is served from the same cached map — no further requests.
	FFlockPlayerData Row50;
	Fx.Provider->GetMyDataByTemplate(TEXT("t50"), [&](TFlockResult<FFlockPlayerData> R) { Row50 = R.Value; });
	TestEqual(TEXT("row from page 1, cached"), Row50.Id, FString(TEXT("pd-50")));
	TestEqual(TEXT("still one page-1 request"), Fx.Fake->CountTo(TEXT("player_data?page=1")), 1);

	// A template with no row for this player -> Ok with an empty record (mirrors Unity's null).
	bool bEmpty = false;
	Fx.Provider->GetMyDataByTemplate(TEXT("does-not-exist"), [&](TFlockResult<FFlockPlayerData> R)
		{ bEmpty = R.bSuccess && R.Value.Id.IsEmpty(); });
	TestTrue(TEXT("absent template row -> Ok empty"), bEmpty);

	Cleanup(Fx.Dir);
	return true;
}

// ── Concurrent asks for the signed-in player's data collapse into one pagination ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerMyDataCoalescedTest, "Flock.Player.Provider.GetMyDataCoalesced",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerMyDataCoalescedTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	// One short page (2 rows < 100), so the fetch is a single request we can hold deferred.
	Fx.Fake->On(TEXT("player_data?page=1"), FFlockFakeTransport::Ok(PlayerDataPage(0, 2, 2, 1, 100)));
	Fx.Fake->bDeferred = true;

	int32 Done = 0;
	Fx.Provider->GetMyDataByTemplate(TEXT("t0"), [&](TFlockResult<FFlockPlayerData> R) { if (R.bSuccess) { ++Done; } });
	Fx.Provider->GetMyDataByTemplate(TEXT("t1"), [&](TFlockResult<FFlockPlayerData> R) { if (R.bSuccess) { ++Done; } });

	// The second caller queued behind the first — only one network request in flight.
	TestEqual(TEXT("coalesced to one request"), Fx.Fake->CountTo(TEXT("player_data?page=1")), 1);

	Fx.Fake->FlushPending();
	TestEqual(TEXT("both callers completed"), Done, 2);
	TestEqual(TEXT("still one request after flush"), Fx.Fake->CountTo(TEXT("player_data?page=1")), 1);

	Cleanup(Fx.Dir);
	return true;
}

// ── GetMyDataByTag resolves the tagged template, then the signed-in player's row for it ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerMyDataByTagTest, "Flock.Player.Provider.GetMyDataByTag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerMyDataByTagTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();
	const FString List = FString::Printf(TEXT("[%s]"), *TemplateObj(TEXT("tmpl-1"), TEXT("Wallet"), TEXT("currency")));
	Fx.Fake->On(TEXT("player_template"), FFlockFakeTransport::Ok(Enveloped(List)));
	const FString Page = FString::Printf(TEXT("{\"items\":[%s],\"total\":1,\"page\":1,\"limit\":100}"),
		*PlayerDataObj(TEXT("pd-1"), TEXT("tmpl-1"), 250));
	Fx.Fake->On(TEXT("player_data?page=1"), FFlockFakeTransport::Ok(Page));

	FFlockPlayerData Row;
	bool bDone = false;
	Fx.Provider->GetMyDataByTag(TEXT("currency"), [&](TFlockResult<FFlockPlayerData> R) { bDone = R.bSuccess; Row = R.Value; });
	TestTrue(TEXT("my data by tag succeeds"), bDone);
	TestEqual(TEXT("resolved row id"), Row.Id, FString(TEXT("pd-1")));
	int32 Coins = 0;
	TestTrue(TEXT("row data reads"), Row.Data.TryGetInt(TEXT("Coins"), Coins));
	TestEqual(TEXT("row data value"), Coins, 250);

	Cleanup(Fx.Dir);
	return true;
}

// ── The ban route is enveloped-but-nullable and never cached: null -> not banned, an object -> banned,
// and each call goes back to the network ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerGetBanTest, "Flock.Player.Provider.GetBanNullableAndFresh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerGetBanTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.SignIn();

	// result: null -> a successful "not banned".
	Fx.Fake->On(TEXT("player-ban"), FFlockFakeTransport::Ok(Enveloped(TEXT("null"))));
	FFlockPlayerBan Ban;
	bool bDone = false;
	Fx.Provider->GetBan(FString(), [&](TFlockResult<FFlockPlayerBan> R) { bDone = R.bSuccess; Ban = R.Value; });
	TestTrue(TEXT("no-ban call succeeds"), bDone);
	TestFalse(TEXT("not banned"), Ban.IsBanned());
	TestEqual(TEXT("player id defaulted into the query"), Fx.Fake->CountTo(TEXT("player_id=player-a")), 1);

	// A present record -> banned, with verbatim feature keys; a second call re-hits the network (never cached).
	Fx.Fake->On(TEXT("player-ban"), FFlockFakeTransport::Ok(Enveloped(BanObj())));
	FFlockPlayerBan Banned;
	Fx.Provider->GetBan(FString(), [&](TFlockResult<FFlockPlayerBan> R) { Banned = R.Value; });
	TestTrue(TEXT("banned"), Banned.IsBanned());
	TestTrue(TEXT("verbatim feature key"), Banned.Data.Contains(TEXT("currency")));
	TestEqual(TEXT("ban never cached -> two requests"), Fx.Fake->CountTo(TEXT("player-ban")), 2);

	Cleanup(Fx.Dir);
	return true;
}

// ── A direct data-by-id read is enveloped and never cached ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerGetDataByIdTest, "Flock.Player.Provider.GetDataByIdEnvelopedFresh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerGetDataByIdTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("player_data/pd-1"),
		FFlockFakeTransport::Ok(Enveloped(PlayerDataObj(TEXT("pd-1"), TEXT("tmpl-1"), 250))));

	FFlockPlayerData Data;
	bool bDone = false;
	Fx.Provider->GetDataById(TEXT("pd-1"), [&](TFlockResult<FFlockPlayerData> R) { bDone = R.bSuccess; Data = R.Value; });
	TestTrue(TEXT("data succeeds"), bDone);
	TestEqual(TEXT("id"), Data.Id, FString(TEXT("pd-1")));
	int32 Coins = 0;
	TestTrue(TEXT("data reads"), Data.Data.TryGetInt(TEXT("Coins"), Coins));
	TestEqual(TEXT("data value"), Coins, 250);

	Fx.Provider->GetDataById(TEXT("pd-1"), [&](TFlockResult<FFlockPlayerData> R) {});
	TestEqual(TEXT("never cached -> two requests"), Fx.Fake->CountTo(TEXT("player_data/pd-1")), 2);

	Cleanup(Fx.Dir);
	return true;
}

// ── GetAllData reads the bare paginated shape; GetTemplatePlayerData reads the enveloped list ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerListShapesTest, "Flock.Player.Provider.DataListShapes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerListShapesTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("player_data?page=1"), FFlockFakeTransport::Ok(PlayerDataPage(0, 1, 1, 1, 100)));

	FFlockPlayerDataPage Page;
	bool bPaged = false;
	Fx.Provider->GetAllData(FString(), 1, 100, [&](TFlockResult<FFlockPlayerDataPage> R) { bPaged = R.bSuccess; Page = R.Value; });
	TestTrue(TEXT("paginated read succeeds"), bPaged);
	TestEqual(TEXT("total"), Page.Total, 1);
	TestEqual(TEXT("one row"), Page.Items.Num(), 1);

	Fx.Fake->On(TEXT("tmpl-1/player-data"),
		FFlockFakeTransport::Ok(Enveloped(FString::Printf(TEXT("[%s]"), *PlayerDataObj(TEXT("pd-9"), TEXT("tmpl-1"), 10)))));
	TArray<FFlockPlayerData> Rows;
	bool bList = false;
	Fx.Provider->GetTemplatePlayerData(TEXT("tmpl-1"), [&](TFlockResult<TArray<FFlockPlayerData>> R) { bList = R.bSuccess; Rows = R.Value; });
	TestTrue(TEXT("enveloped-list read succeeds"), bList);
	if (TestEqual(TEXT("one row"), Rows.Num(), 1))
	{
		TestEqual(TEXT("row id"), Rows[0].Id, FString(TEXT("pd-9")));
	}

	Cleanup(Fx.Dir);
	return true;
}

// ── Required arguments and the current-player gate short-circuit before any request ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerValidationTest, "Flock.Player.Provider.ValidationShortCircuits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerValidationTest::RunTest(const FString& Parameters)
{
	{
		FFixture Fx;
		bool bFailed = false;
		Fx.Provider->GetDataById(FString(), [&](TFlockResult<FFlockPlayerData> R) { bFailed = !R.bSuccess && R.Error.Type == EFlockErrorType::Validation; });
		TestTrue(TEXT("empty data id -> validation"), bFailed);
		TestEqual(TEXT("no request"), Fx.Fake->Requests.Num(), 0);
		Cleanup(Fx.Dir);
	}
	{
		FFixture Fx;
		bool bFailed = false;
		Fx.Provider->GetTemplateById(FString(), [&](TFlockResult<FFlockPlayerTemplateSchema> R) { bFailed = !R.bSuccess && R.Error.Type == EFlockErrorType::Validation; });
		TestTrue(TEXT("empty template id -> validation"), bFailed);
		Cleanup(Fx.Dir);
	}
	{
		// Signed out: GetMyDataByTemplate can't resolve a current player.
		FFixture Fx;
		bool bFailed = false;
		Fx.Provider->GetMyDataByTemplate(TEXT("tmpl-1"), [&](TFlockResult<FFlockPlayerData> R) { bFailed = !R.bSuccess && R.Error.Type == EFlockErrorType::Validation; });
		TestTrue(TEXT("my data signed out -> validation"), bFailed);
		TestEqual(TEXT("no request"), Fx.Fake->Requests.Num(), 0);
		Cleanup(Fx.Dir);
	}
	{
		// Signed out: GetBan defaults to the (absent) current player and short-circuits.
		FFixture Fx;
		bool bFailed = false;
		Fx.Provider->GetBan(FString(), [&](TFlockResult<FFlockPlayerBan> R) { bFailed = !R.bSuccess && R.Error.Type == EFlockErrorType::Validation; });
		TestTrue(TEXT("ban signed out -> validation"), bFailed);
		TestEqual(TEXT("no request"), Fx.Fake->Requests.Num(), 0);
		Cleanup(Fx.Dir);
	}
	return true;
}

// ── ClearCache forces the next template read back to the network ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerClearCacheTest, "Flock.Player.Provider.ClearCacheRefetches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerClearCacheTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("player_template/tmpl-1"),
		FFlockFakeTransport::Ok(Enveloped(TemplateObj(TEXT("tmpl-1"), TEXT("Wallet"), TEXT("currency")))));

	Fx.Provider->GetTemplateById(TEXT("tmpl-1"), [&](TFlockResult<FFlockPlayerTemplateSchema> R) {});
	Fx.Provider->ClearCache();
	Fx.Provider->GetTemplateById(TEXT("tmpl-1"), [&](TFlockResult<FFlockPlayerTemplateSchema> R) {});
	TestEqual(TEXT("cache cleared -> two requests"), Fx.Fake->CountTo(TEXT("player_template/tmpl-1")), 2);

	Cleanup(Fx.Dir);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
