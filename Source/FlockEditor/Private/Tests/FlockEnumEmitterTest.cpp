// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Codegen/FlockEnumEmitter.h"
#include "Engine/UserDefinedEnum.h"
#include "UObject/Package.h"

namespace FlockEnumEmitterTestHelpers
{
	inline FFlockShopItem Item(const FString& Id, const FString& Name, const FString& Currency)
	{
		FFlockShopItem Result;
		Result.Id = Id;
		Result.Name = Name;
		Result.Currency = Currency;
		return Result;
	}

	inline FFlockPlayerTemplateSchema Template(const FString& Id, const FString& Name, const FString& Tag,
		const FString& SchemaJson)
	{
		FFlockPlayerTemplateSchema Result;
		Result.Id = Id;
		Result.Name = Name;
		Result.Tag = Tag;
		Result.SchemaJson = SchemaJson;
		return Result;
	}

	/** Two shops sharing a currency and one shared item, so de-duplication is genuinely exercised. */
	inline FFlockSchemaSnapshot Snapshot()
	{
		FFlockSchemaSnapshot Result;
		Result.GameVersionId = TEXT("ver-1");

		FFlockShop Starter;
		Starter.Id = TEXT("shop-1");
		Starter.Name = TEXT("Starter");
		Starter.ShopItems.Add(Item(TEXT("item-2"), TEXT("shield charm"), TEXT("Shard")));
		Starter.ShopItems.Add(Item(TEXT("item-1"), TEXT("Gem Pack"), TEXT("Gold")));

		FFlockShop Premium;
		Premium.Id = TEXT("shop-2");
		Premium.Name = TEXT("Premium");
		Premium.ShopItems.Add(Item(TEXT("item-1"), TEXT("Gem Pack"), TEXT("Gold"))); // same item, listed twice
		Premium.ShopItems.Add(Item(TEXT("item-3"), TEXT("100 Gems"), TEXT("Gold")));

		Result.Shops = { Starter, Premium };
		Result.PlayerTemplates.Add(Template(TEXT("tmpl-1"), TEXT("Trophies"), TEXT("achievement"),
			TEXT("[{\"type\":\"bool\",\"field_name\":\"first_win\"},{\"type\":\"bool\",\"field_name\":\"flawless_run\"}]")));
		return Result;
	}

	inline FString DisplayNameAt(const UUserDefinedEnum* Enum, int32 Index)
	{
		return Enum ? Enum->GetDisplayNameTextByIndex(Index).ToString() : FString();
	}

	/** Enumerator count excluding the implicit _MAX the engine appends. */
	inline int32 MemberCount(const UUserDefinedEnum* Enum)
	{
		return Enum ? Enum->NumEnums() - 1 : 0;
	}
}

using namespace FlockEnumEmitterTestHelpers;

// ── Shop items become members keyed by name but carrying their id ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockEnumShopItemTest, "Flock.Editor.EnumEmitter.CollectsShopItemsById",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockEnumShopItemTest::RunTest(const FString& Parameters)
{
	const TArray<TPair<FString, FString>> Items = FFlockEnumEmitter::CollectShopItems(Snapshot());

	// Three distinct ids, not four listings — the same item in two shops is one member.
	TestEqual(TEXT("three distinct items"), Items.Num(), 3);

	// The display name is human-facing; the wire value is the opaque id a purchase needs.
	const TPair<FString, FString>* GemPack = Items.FindByPredicate(
		[](const TPair<FString, FString>& Member) { return Member.Value == TEXT("item-1"); });
	if (TestNotNull(TEXT("gem pack present"), GemPack))
	{
		TestEqual(TEXT("display name is Pascal-cased from the item name"), GemPack->Key, FString(TEXT("GemPack")));
	}

	// A name starting with a digit cannot be an identifier — same problem the canonical SDK hits.
	const TPair<FString, FString>* Gems = Items.FindByPredicate(
		[](const TPair<FString, FString>& Member) { return Member.Value == TEXT("item-3"); });
	if (TestNotNull(TEXT("digit-led item present"), Gems))
	{
		TestEqual(TEXT("leading digit is prefixed"), Gems->Key, FString(TEXT("_100Gems")));
	}

	return true;
}

// ── Currencies and achievements carry their own name as the wire value ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockEnumCurrencyAchievementTest, "Flock.Editor.EnumEmitter.CollectsCurrenciesAndAchievements",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockEnumCurrencyAchievementTest::RunTest(const FString& Parameters)
{
	const TArray<TPair<FString, FString>> Currencies = FFlockEnumEmitter::CollectCurrencies(Snapshot());
	TestEqual(TEXT("two distinct currencies"), Currencies.Num(), 2);
	// Sorted, so a backend reordering its shops does not reorder the enum (which would renumber it).
	TestEqual(TEXT("sorted"), Currencies[0].Key, FString(TEXT("Gold")));
	TestEqual(TEXT("wire value is the currency name"), Currencies[0].Value, FString(TEXT("Gold")));

	const TArray<TPair<FString, FString>> Achievements = FFlockEnumEmitter::CollectAchievements(Snapshot());
	TestEqual(TEXT("two achievements"), Achievements.Num(), 2);
	// Display is friendly, wire is the declared field name the command actually sends.
	TestEqual(TEXT("display name"), Achievements[0].Key, FString(TEXT("FirstWin")));
	TestEqual(TEXT("wire value is the declared name"), Achievements[0].Value, FString(TEXT("first_win")));

	// No tagged template means no achievements — not an error.
	FFlockSchemaSnapshot Untagged = Snapshot();
	Untagged.PlayerTemplates[0].Tag = TEXT("progress");
	TestEqual(TEXT("no tagged template, no achievements"),
		FFlockEnumEmitter::CollectAchievements(Untagged).Num(), 0);

	return true;
}

// ── The built enums carry the display names a dropdown shows ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockEnumBuildTest, "Flock.Editor.EnumEmitter.BuildsTypedEnums",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockEnumBuildTest::RunTest(const FString& Parameters)
{
	const FFlockEnumEmitter::FEmitResult Result = FFlockEnumEmitter::BuildAll(Snapshot(), GetTransientPackage());

	TestEqual(TEXT("all three enums built"), Result.EnumCount(), 3);
	TestEqual(TEXT("no warnings"), Result.Warnings.Num(), 0);

	// The seeded enumerator is reused rather than left over, so the member count matches the source set.
	TestEqual(TEXT("shop item members"), MemberCount(Result.ShopItems.Enum), 3);
	TestEqual(TEXT("currency members"), MemberCount(Result.Currencies.Enum), 2);
	TestEqual(TEXT("achievement members"), MemberCount(Result.Achievements.Enum), 2);

	// Display names are what a graph author picks from.
	TestEqual(TEXT("first currency display name"), DisplayNameAt(Result.Currencies.Enum, 0), FString(TEXT("Gold")));
	TestEqual(TEXT("first achievement display name"), DisplayNameAt(Result.Achievements.Enum, 0), FString(TEXT("FirstWin")));

	// The mapping travels with the enum, because the wire value cannot be recovered from it afterwards.
	TestEqual(TEXT("mapping matches member count"),
		Result.Achievements.WireValueByDisplayName.Num(), MemberCount(Result.Achievements.Enum));
	TestEqual(TEXT("mapping order matches enum order"),
		Result.Achievements.WireValueByDisplayName[0].Value, FString(TEXT("first_win")));

	return true;
}

// ── An empty set emits no asset at all ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockEnumEmptyTest, "Flock.Editor.EnumEmitter.EmitsNothingForAnEmptySet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockEnumEmptyTest::RunTest(const FString& Parameters)
{
	TArray<FString> Warnings;
	const FFlockEnumEmitter::FEnumResult Empty = FFlockEnumEmitter::BuildEnum(
		GetTransientPackage(), TEXT("FlockEmptyEnum"), {}, Warnings);

	// An enum with no members is not a useful dropdown, and leaving one behind would look like a bug to
	// whoever browses the generated folder. Not a warning either — a game with no shops is normal.
	TestFalse(TEXT("no enum built"), Empty.IsValid());
	TestEqual(TEXT("and no warning"), Warnings.Num(), 0);

	// A game with nothing at all emits nothing at all.
	FFlockSchemaSnapshot Bare;
	Bare.GameVersionId = TEXT("ver-1");
	const FFlockEnumEmitter::FEmitResult Result = FFlockEnumEmitter::BuildAll(Bare, GetTransientPackage());
	TestEqual(TEXT("no enums"), Result.EnumCount(), 0);
	TestEqual(TEXT("no warnings"), Result.Warnings.Num(), 0);

	return true;
}

// ── Colliding display names are disambiguated rather than dropped ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockEnumCollisionTest, "Flock.Editor.EnumEmitter.DisambiguatesCollidingNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockEnumCollisionTest::RunTest(const FString& Parameters)
{
	FFlockSchemaSnapshot Snap;
	FFlockShop Shop;
	Shop.Id = TEXT("shop-1");
	Shop.Name = TEXT("Collide");
	// Distinct items whose names collapse to the same identifier.
	Shop.ShopItems.Add(Item(TEXT("item-1"), TEXT("Gem Pack"), TEXT("Gold")));
	Shop.ShopItems.Add(Item(TEXT("item-2"), TEXT("gem-pack"), TEXT("Gold")));
	Snap.Shops = { Shop };

	const TArray<TPair<FString, FString>> Items = FFlockEnumEmitter::CollectShopItems(Snap);

	// Both survive: dropping one would make an item unpurchasable from Blueprint with no explanation.
	TestEqual(TEXT("both items kept"), Items.Num(), 2);
	TestNotEqual(TEXT("names disambiguated"), Items[0].Key, Items[1].Key);
	TestEqual(TEXT("ids intact"), Items[0].Value, FString(TEXT("item-1")));
	TestEqual(TEXT("second id intact"), Items[1].Value, FString(TEXT("item-2")));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
