// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Http/FlockJsonUtils.h"
#include "Models/FlockPlayerModels.h"

namespace
{
	// A player template whose `data` mixes a scalar and an object node, with `schema` present so the
	// verbatim passthrough can be checked.
	const TCHAR* const TemplateBody =
		TEXT("{")
		TEXT("\"id\":\"tmpl-1\",\"name\":\"Wallet\",\"game_version_id\":\"ver-1\",\"tag\":\"currency\",")
		TEXT("\"schema\":[{\"type\":\"int\",\"field_name\":\"coins\",\"type_name\":\"int\"}],")
		TEXT("\"data\":[")
		TEXT("{\"type\":\"int\",\"field_name\":\"coins\",\"value\":100},")
		TEXT("{\"type\":\"object\",\"field_name\":\"limits\",\"value\":[")
		TEXT("{\"type\":\"int\",\"field_name\":\"daily_max\",\"value\":500}]}")
		TEXT("]}");

	// A per-player row for that template.
	const TCHAR* const PlayerDataBody =
		TEXT("{")
		TEXT("\"id\":\"pd-1\",\"player_template_id\":\"tmpl-1\",\"game_id\":\"game-1\",\"player_id\":\"player-1\",")
		TEXT("\"created_at\":\"2026-01-01T00:00:00Z\",\"updated_at\":\"2026-01-02T00:00:00Z\",")
		TEXT("\"data\":[{\"type\":\"int\",\"field_name\":\"coins\",\"value\":250}]}");

	// A bare ban record — its `data` is keyed by feature name (author data, one snake_case to prove it is
	// kept verbatim), each value a typed FeatureBan.
	const TCHAR* const BanObject =
		TEXT("{")
		TEXT("\"id\":\"ban-1\",\"player_id\":\"player-1\",\"game_id\":\"game-1\",")
		TEXT("\"data\":{")
		TEXT("\"currency\":{\"reason\":\"cheating\",\"ban_duration\":\"7d\",\"effective_datetime\":\"2026-01-01T00:00:00Z\"},")
		TEXT("\"trade_house\":{\"reason\":\"abuse\",\"ban_duration\":\"perm\",\"effective_datetime\":\"2026-02-01T00:00:00Z\"}},")
		TEXT("\"created_at\":\"2026-01-01T00:00:00Z\",\"updated_at\":\"2026-01-02T00:00:00Z\"}");

	template <typename T>
	bool ParseWire(const FString& Body, T& OutStruct)
	{
		TSharedPtr<FJsonObject> Object;
		if (!FFlockJsonUtils::TryParseObject(Body, Object) || !Object.IsValid())
		{
			return false;
		}
		FString Error;
		// Through WireObjectToStruct on purpose: proves the model is routed to its custom FromWireObject.
		return FFlockJsonUtils::WireObjectToStruct(Object.ToSharedRef(), OutStruct, Error);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerTemplateParseTest, "Flock.Player.Models.TemplateParse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerTemplateParseTest::RunTest(const FString& Parameters)
{
	FFlockPlayerTemplateSchema Template;
	TestTrue(TEXT("template parses"), ParseWire(TemplateBody, Template));

	TestEqual(TEXT("id"), Template.Id, FString(TEXT("tmpl-1")));
	TestEqual(TEXT("name"), Template.Name, FString(TEXT("Wallet")));
	TestEqual(TEXT("game_version_id -> GameVersionId"), Template.GameVersionId, FString(TEXT("ver-1")));
	TestEqual(TEXT("tag stays a string"), Template.Tag, FString(TEXT("currency")));

	// `data` flattened through the shared handle: scalar + nested object, snake input resolves via Pascal.
	int32 Coins = 0;
	TestTrue(TEXT("scalar reads"), Template.Data.TryGetInt(TEXT("coins"), Coins));
	TestEqual(TEXT("scalar value"), Coins, 100);
	int32 DailyMax = 0;
	TestTrue(TEXT("nested reads"), Template.Data.TryGetInt(TEXT("Limits.DailyMax"), DailyMax));
	TestEqual(TEXT("nested value"), DailyMax, 500);

	// `schema` kept verbatim for codegen.
	TestFalse(TEXT("schema captured"), Template.SchemaJson.IsEmpty());
	TestTrue(TEXT("schema keeps snake field_name"), Template.SchemaJson.Contains(TEXT("field_name")));
	TestTrue(TEXT("schema keeps type_name"), Template.SchemaJson.Contains(TEXT("type_name")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerDataParseTest, "Flock.Player.Models.DataParse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerDataParseTest::RunTest(const FString& Parameters)
{
	FFlockPlayerData Data;
	TestTrue(TEXT("player data parses"), ParseWire(PlayerDataBody, Data));

	TestEqual(TEXT("id"), Data.Id, FString(TEXT("pd-1")));
	TestEqual(TEXT("player_template_id -> PlayerTemplateId"), Data.PlayerTemplateId, FString(TEXT("tmpl-1")));
	TestEqual(TEXT("game_id -> GameId"), Data.GameId, FString(TEXT("game-1")));
	TestEqual(TEXT("player_id -> PlayerId"), Data.PlayerId, FString(TEXT("player-1")));

	int32 Coins = 0;
	TestTrue(TEXT("data reads"), Data.Data.TryGetInt(TEXT("Coins"), Coins));
	TestEqual(TEXT("data value"), Coins, 250);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerBanParseTest, "Flock.Player.Models.BanParse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerBanParseTest::RunTest(const FString& Parameters)
{
	FFlockPlayerBan Ban;
	TestTrue(TEXT("ban parses"), ParseWire(BanObject, Ban));

	TestEqual(TEXT("id"), Ban.Id, FString(TEXT("ban-1")));
	TestTrue(TEXT("IsBanned true for a real record"), Ban.IsBanned());

	// Feature keys are kept verbatim (a snake_case key is NOT Pascal-cased).
	TestEqual(TEXT("two feature entries"), Ban.Data.Num(), 2);
	TestTrue(TEXT("verbatim feature key currency"), Ban.Data.Contains(TEXT("currency")));
	TestTrue(TEXT("verbatim feature key trade_house"), Ban.Data.Contains(TEXT("trade_house")));
	TestFalse(TEXT("feature key NOT Pascal-cased (TradeHouse)"), Ban.Data.Contains(TEXT("TradeHouse")));

	// Each value's fixed fields were read (snake -> Pascal within the value).
	const FFlockFeatureBan* Currency = Ban.Data.Find(TEXT("currency"));
	TestTrue(TEXT("currency entry present"), Currency != nullptr);
	if (Currency != nullptr)
	{
		TestEqual(TEXT("reason"), Currency->Reason, FString(TEXT("cheating")));
		TestEqual(TEXT("ban_duration -> BanDuration"), Currency->BanDuration, FString(TEXT("7d")));
		TestEqual(TEXT("effective_datetime -> EffectiveDatetime"), Currency->EffectiveDatetime,
			FString(TEXT("2026-01-01T00:00:00Z")));
	}

	return true;
}

// The ban route is enveloped but its `result` is nullable: a 2xx with `result: null` is "not banned",
// which must not be rejected as a missing result. FFlockPlayerBanResponse (fetched via GetRaw) tolerates
// the null; a present object parses as a real ban.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerBanNullResultTest, "Flock.Player.Models.BanNullResult",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerBanNullResultTest::RunTest(const FString& Parameters)
{
	FString Error;

	// result: null -> not banned.
	const FString NullBody = TEXT("{\"error\":null,\"response\":\"ok\",\"result\":null}");
	FFlockPlayerBanResponse NullResponse;
	TestTrue(TEXT("null envelope parses"), FFlockJsonUtils::WireJsonToStruct(NullBody, NullResponse, Error));
	TestFalse(TEXT("null result -> not banned"), NullResponse.Ban.IsBanned());

	// result absent -> also not banned.
	const FString AbsentBody = TEXT("{\"error\":null,\"response\":\"ok\"}");
	FFlockPlayerBanResponse AbsentResponse;
	TestTrue(TEXT("absent-result envelope parses"), FFlockJsonUtils::WireJsonToStruct(AbsentBody, AbsentResponse, Error));
	TestFalse(TEXT("absent result -> not banned"), AbsentResponse.Ban.IsBanned());

	// result: {ban object} -> banned, with the data intact.
	const FString BannedBody = FString::Printf(TEXT("{\"error\":null,\"response\":\"ok\",\"result\":%s}"), BanObject);
	FFlockPlayerBanResponse BannedResponse;
	TestTrue(TEXT("banned envelope parses"), FFlockJsonUtils::WireJsonToStruct(BannedBody, BannedResponse, Error));
	TestTrue(TEXT("present result -> banned"), BannedResponse.Ban.IsBanned());
	TestEqual(TEXT("ban id"), BannedResponse.Ban.Id, FString(TEXT("ban-1")));
	TestTrue(TEXT("ban feature preserved"), BannedResponse.Ban.Data.Contains(TEXT("currency")));

	return true;
}

// A parsed template survives the snapshot's plain (PascalCase, no-transform) round-trip: the flattened
// data and verbatim schema both come back intact, which is what makes offline template caching correct.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlayerTemplateSnapshotRoundTripTest, "Flock.Player.Models.TemplateSnapshotRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlayerTemplateSnapshotRoundTripTest::RunTest(const FString& Parameters)
{
	FFlockPlayerTemplateSchema Template;
	TestTrue(TEXT("template parses"), ParseWire(TemplateBody, Template));

	FString Snapshot;
	TestTrue(TEXT("serializes to snapshot"), FFlockJsonUtils::StructToPlainJson(Template, Snapshot));

	FFlockPlayerTemplateSchema Restored;
	TestTrue(TEXT("restores from snapshot"), FFlockJsonUtils::PlainJsonToStruct(Snapshot, Restored));

	TestEqual(TEXT("id survives"), Restored.Id, FString(TEXT("tmpl-1")));
	int32 Coins = 0;
	TestTrue(TEXT("flattened data survives"), Restored.Data.TryGetInt(TEXT("Coins"), Coins));
	TestEqual(TEXT("data value intact"), Coins, 100);
	TestTrue(TEXT("schema survives verbatim"), Restored.SchemaJson.Contains(TEXT("field_name")));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
