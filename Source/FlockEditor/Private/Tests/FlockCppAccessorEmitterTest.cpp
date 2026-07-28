// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Codegen/FlockCppAccessorEmitter.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

/**
 * Text assertions, for the same reason as the type emitter's: only a real build proves this compiles,
 * and that check is run by syncing a project and building it.
 *
 * What is asserted here is the shape of the call surface — that the row id stays out of the struct, that
 * a config gets no write, and that a command bakes its ids. Those are the decisions; the C++ around them
 * is checked by the compiler.
 */
namespace FlockCppAccessorEmitterTestHelpers
{
	inline FString Root()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FlockCppAccessorTest"));
	}

	inline FFlockPlayerTemplateSchema Template(const FString& Name, const FString& Tag)
	{
		FFlockPlayerTemplateSchema Result;
		Result.Id = TEXT("tmpl-") + Name;
		Result.Name = Name;
		Result.Tag = Tag;
		Result.SchemaJson = TEXT("[{\"type\":\"int\",\"field_name\":\"gold\"}]");
		return Result;
	}

	/** A game with a writable template, a config, and a shop — one of each surface. */
	inline FFlockSchemaSnapshot Snapshot()
	{
		FFlockSchemaSnapshot Result;
		Result.GameVersionId = TEXT("ver-1");
		Result.PlayerTemplates.Add(Template(TEXT("Wallet"), TEXT("currency")));

		FFlockGameConfigSchema Config;
		Config.Id = TEXT("cfg-1");
		Config.Name = TEXT("Gameplay");
		Config.SchemaJson = TEXT("[{\"type\":\"int\",\"field_name\":\"move_speed\"}]");
		Result.GameConfigs.Add(Config);

		FFlockShop Shop;
		Shop.Id = TEXT("shop-1");
		FFlockShopItem Item;
		Item.Id = TEXT("item-1");
		Item.Name = TEXT("Gem Pack");
		Item.Currency = TEXT("Gold");
		Shop.ShopItems.Add(Item);
		Result.Shops.Add(Shop);
		return Result;
	}

	inline FString Read(const FString& Relative)
	{
		FString Contents;
		FFileHelper::LoadFileToString(Contents, *FPaths::Combine(Root(), Relative));
		return Contents;
	}
}

using namespace FlockCppAccessorEmitterTestHelpers;

// ── The call surface: what each entity gets, and what it deliberately does not ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCppAccessorTest, "Flock.Editor.CppAccessors.EmitsTypedCallSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCppAccessorTest::RunTest(const FString& Parameters)
{
	IFileManager::Get().DeleteDirectory(*Root(), /*RequireExists*/ false, /*Tree*/ true);
	const FFlockCppAccessorEmitter::FEmitResult Result =
		FFlockCppAccessorEmitter::Emit(Snapshot(), Root(), TEXT("FlockGenerated"));
	for (const FString& Warning : Result.Warnings)
	{
		AddInfo(Warning);
	}
	if (!TestTrue(TEXT("emitted"), Result.bSucceeded))
	{
		return false;
	}

	const FString Header = Read(TEXT("Public/FlockGeneratedAccessors.h"));
	const FString Source = Read(TEXT("Private/FlockGeneratedAccessors.cpp"));
	auto HasDecl = [&Header](const TCHAR* Needle) { return Header.Contains(Needle, ESearchCase::CaseSensitive); };
	auto HasImpl = [&Source](const TCHAR* Needle) { return Source.Contains(Needle, ESearchCase::CaseSensitive); };

	// The row id travels as an out-parameter beside the struct, never as a member of it: generated structs
	// are BlueprintType, so a member would surface in a Break node as if it were template data, and the
	// write-side conversion would have to learn to exclude it.
	TestTrue(TEXT("Get hands back the struct and the row id"),
		HasDecl(TEXT("TFunction<void(TFlockResult<FWalletTemplate>, const FString& RowId)> OnDone")));
	TestTrue(TEXT("Save takes both back"),
		HasDecl(TEXT("SaveWallet(const UObject* WorldContextObject, const FString& RowId")));

	// A config is game-wide and changed from the dashboard; a client-side write would be a call the
	// server always rejects.
	TestTrue(TEXT("a config gets a read"), HasDecl(TEXT("static void GetGameplay(")));
	TestFalse(TEXT("and no write"), HasDecl(TEXT("SaveGameplay")));

	// Ids are baked, which is the entire point — a call site never types one.
	TestTrue(TEXT("the template id is baked into the fetch"),
		HasImpl(TEXT("GetMyDataByTemplate(TEXT(\"tmpl-Wallet\")")));
	TestTrue(TEXT("the config id is baked"), HasImpl(TEXT("ResolveConfigData(TEXT(\"cfg-1\")")));
	// The currency-tagged template's id, so Add Funds skips the SDK's runtime tag scan.
	TestTrue(TEXT("the wallet template id is baked into Add Funds"),
		HasImpl(TEXT("Amount, TEXT(\"tmpl-Wallet\")")));

	// Commands take the generated enum rather than a string, so an id cannot be mistyped.
	TestTrue(TEXT("purchase takes the item enum"), HasDecl(TEXT("Purchase(const UObject* WorldContextObject, EFlockShopItemId Item")));
	TestTrue(TEXT("and converts it to the wire id"), HasImpl(TEXT("FlockShopItemIdToWire(Item)")));

	// No name map anywhere: members are named exactly as declared, at every depth.
	TestTrue(TEXT("the write reflects the struct directly"),
		HasImpl(TEXT("FFlockStructBinder::ToCommandData(FWalletTemplate::StaticStruct(), &Value)")));

	// A call before the SDK is up must fail through the callback, not crash — these are static functions
	// a designer can call from anywhere, including too early.
	TestTrue(TEXT("an uninitialized SDK fails through the callback"), HasImpl(TEXT("NotReady()")));
	TestTrue(TEXT("and every provider lookup is null-checked"), HasImpl(TEXT("Flock ? Flock->GetPlayerProvider() : nullptr")));

	return true;
}

// ── A game with no shop gets no Purchase, and an untagged wallet is called out ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCppAccessorSparseTest, "Flock.Editor.CppAccessors.SkipsAbsentSurfaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCppAccessorSparseTest::RunTest(const FString& Parameters)
{
	FFlockSchemaSnapshot Sparse;
	Sparse.GameVersionId = TEXT("ver-1");
	// A template with no `currency` tag, and no shop at all.
	Sparse.PlayerTemplates.Add(Template(TEXT("Wallet"), FString()));

	IFileManager::Get().DeleteDirectory(*Root(), /*RequireExists*/ false, /*Tree*/ true);
	const FFlockCppAccessorEmitter::FEmitResult Result =
		FFlockCppAccessorEmitter::Emit(Sparse, Root(), TEXT("FlockGenerated"));
	TestTrue(TEXT("emitted"), Result.bSucceeded);

	const FString Header = Read(TEXT("Public/FlockGeneratedAccessors.h"));
	// An enum with no members is not emitted, so a command keyed by one cannot be either — referencing a
	// type that does not exist would stop the module compiling.
	TestFalse(TEXT("no shop means no Purchase"), Header.Contains(TEXT("Purchase("), ESearchCase::CaseSensitive));
	TestFalse(TEXT("no achievements means no UnlockAchievement"),
		Header.Contains(TEXT("UnlockAchievement("), ESearchCase::CaseSensitive));

	// The template still gets its accessors — the absent pieces must not take the present ones with them.
	TestTrue(TEXT("the template surface survives"),
		Header.Contains(TEXT("static void GetWallet("), ESearchCase::CaseSensitive));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
