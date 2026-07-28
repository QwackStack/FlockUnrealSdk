// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Codegen/FlockCppTypeEmitter.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

/**
 * These assert the *text* of a generated header, which is as far as a headless test can go — only a real
 * build proves a header compiles, and that check is run by syncing a project and building it.
 *
 * So the assertions here concentrate on the rules whose violation UHT would reject or, worse, accept
 * while writing the wrong thing on the wire: declared names kept verbatim, nested structs hoisted out and
 * ordered before their user, and inexpressible shapes degrading instead of being guessed at.
 */
namespace FlockCppTypeEmitterTestHelpers
{
	inline FString Root()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FlockCppTypeTest"));
	}

	inline FFlockPlayerTemplateSchema Template(const FString& Name, const FString& SchemaJson)
	{
		FFlockPlayerTemplateSchema Result;
		Result.Id = TEXT("tmpl-") + Name;
		Result.Name = Name;
		Result.SchemaJson = SchemaJson;
		return Result;
	}

	inline FString Read(const FString& Relative)
	{
		FString Contents;
		FFileHelper::LoadFileToString(Contents, *FPaths::Combine(Root(), Relative));
		return Contents;
	}

	inline FString EmitAndRead(const FFlockSchemaSnapshot& Snapshot, const TCHAR* HeaderName,
		FFlockCppTypeEmitter::FEmitResult& OutResult)
	{
		IFileManager::Get().DeleteDirectory(*Root(), /*RequireExists*/ false, /*Tree*/ true);
		OutResult = FFlockCppTypeEmitter::Emit(Snapshot, Root(), TEXT("FlockGenerated"));

		FString Contents;
		FFileHelper::LoadFileToString(Contents,
			*FPaths::Combine(Root(), TEXT("Public"), FString(HeaderName) + TEXT(".h")));
		return Contents;
	}
}

using namespace FlockCppTypeEmitterTestHelpers;

// ── Members read idiomatically in C++, with the declared name mapped alongside ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCppTypeNamesTest, "Flock.Editor.CppTypes.MapsMembersToDeclaredNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCppTypeNamesTest::RunTest(const FString& Parameters)
{
	FFlockSchemaSnapshot Snapshot;
	Snapshot.GameVersionId = TEXT("ver-1");
	Snapshot.PlayerTemplates.Add(Template(TEXT("Wallet"),
		TEXT("[{\"type\":\"int\",\"field_name\":\"game_currencies\"},")
		TEXT("{\"type\":\"object\",\"field_name\":\"progress\",\"schema\":[")
		TEXT("{\"type\":\"string\",\"field_name\":\"last_map\"},{\"type\":\"int\",\"field_name\":\"Stage\"}]}]")));

	FFlockCppTypeEmitter::FEmitResult Result;
	const FString Header = EmitAndRead(Snapshot, TEXT("FWalletTemplate"), Result);
	for (const FString& Warning : Result.Warnings)
	{
		AddInfo(Warning);
	}
	if (!TestFalse(TEXT("a header was written"), Header.IsEmpty()))
	{
		return false;
	}

	auto Has = [&Header](const TCHAR* Needle) { return Header.Contains(Needle, ESearchCase::CaseSensitive); };

	// Members are Pascal so C++ reads idiomatically. That is only safe because the declared name travels
	// separately in the wire-name table — checked below, at both depths, since a top-level-only map is
	// exactly what failed here once before.
	TestTrue(TEXT("a snake_case field becomes a Pascal member"), Has(TEXT("int32 GameCurrencies = 0;")));
	TestTrue(TEXT("a nested one does too"), Has(TEXT("FString LastMap;")));
	TestTrue(TEXT("and an already-Pascal field is unchanged"), Has(TEXT("int32 Stage = 0;")));

	const FString Table = Read(TEXT("Private/FlockGeneratedWireNames.cpp"));
	auto Maps = [&Table](const TCHAR* Member, const TCHAR* Declared)
	{
		return Table.Contains(FString::Printf(TEXT("{ TEXT(\"%s\"), TEXT(\"%s\") }"), Member, Declared),
			ESearchCase::CaseSensitive);
	};
	TestTrue(TEXT("the top-level member maps to its declared name"), Maps(TEXT("GameCurrencies"), TEXT("game_currencies")));
	// The nested level is the one that matters: a map consulted only at the top would write `LastMap`.
	TestTrue(TEXT("and so does the nested one"), Maps(TEXT("LastMap"), TEXT("last_map")));
	TestFalse(TEXT("an unchanged name needs no mapping"), Maps(TEXT("Stage"), TEXT("Stage")));

	// UHT will not accept a USTRUCT declared inside another, so a nested object is hoisted to the top
	// level — and C++ needs it defined before the member that uses it.
	const int32 NestedAt = Header.Find(TEXT("struct FLOCKGENERATED_API FWalletTemplateProgress"), ESearchCase::CaseSensitive);
	const int32 ParentAt = Header.Find(TEXT("struct FLOCKGENERATED_API FWalletTemplate\n"), ESearchCase::CaseSensitive);
	TestTrue(TEXT("the nested struct is hoisted out"), NestedAt != INDEX_NONE);
	TestTrue(TEXT("the parent exists"), ParentAt != INDEX_NONE);
	TestTrue(TEXT("and the nested type is defined before it is used"), NestedAt < ParentAt);
	TestTrue(TEXT("the parent holds it by generated type"), Has(TEXT("FWalletTemplateProgress Progress;")));

	// Scalars are default-initialized, or an unfetched struct carries stack garbage into a write.
	TestTrue(TEXT("the header declares its generated companion"), Has(TEXT("#include \"FWalletTemplate.generated.h\"")));
	TestTrue(TEXT("and is BlueprintType, so a graph sees it too"), Has(TEXT("USTRUCT(BlueprintType)")));

	return true;
}

// ── Shapes that cannot be a UPROPERTY degrade rather than breaking the build ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCppTypeDegradeTest, "Flock.Editor.CppTypes.DegradesInexpressibleShapes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCppTypeDegradeTest::RunTest(const FString& Parameters)
{
	FFlockSchemaSnapshot Snapshot;
	Snapshot.GameVersionId = TEXT("ver-1");
	Snapshot.PlayerTemplates.Add(Template(TEXT("Odd"),
		// A list of lists: UHT rejects a container whose element is itself a container.
		TEXT("[{\"type\":\"list\",\"field_name\":\"grid\",\"schema\":{\"type\":\"list\",\"field_name\":\"row\",")
		TEXT("\"schema\":{\"type\":\"int\",\"field_name\":\"cell\"}}},")
		// A name that is not a C++ identifier, and one that is a keyword.
		TEXT("{\"type\":\"int\",\"field_name\":\"200\"},")
		TEXT("{\"type\":\"int\",\"field_name\":\"class\"},")
		// A type the SDK does not know.
		TEXT("{\"type\":\"quaternion\",\"field_name\":\"spin\"},")
		// The expressible ones, so the header is not merely empty.
		TEXT("{\"type\":\"list\",\"field_name\":\"tags\",\"schema\":{\"type\":\"string\",\"field_name\":\"tag\"}},")
		TEXT("{\"type\":\"dict\",\"field_name\":\"counts\",\"schema\":{\"type\":\"int\",\"field_name\":\"n\"}}]")));

	FFlockCppTypeEmitter::FEmitResult Result;
	const FString Header = EmitAndRead(Snapshot, TEXT("FOddTemplate"), Result);
	for (const FString& Warning : Result.Warnings)
	{
		AddInfo(Warning);
	}
	auto Has = [&Header](const TCHAR* Needle) { return Header.Contains(Needle, ESearchCase::CaseSensitive); };

	// A near-miss type here does not fail at runtime, it fails the user's build — so it degrades.
	TestTrue(TEXT("a container of containers degrades"), Has(TEXT("FFlockJsonData Grid;")));
	TestTrue(TEXT("an unknown type degrades"), Has(TEXT("FFlockJsonData Spin;")));

	// Names that are not legal C++ identifiers are now *sanitized rather than skipped*, which the
	// wire-name table makes safe — the member is renamed, the declared name still goes on the wire.
	// Before that table existed these were dropped, because a rename wrote a key the server rejects.
	TestTrue(TEXT("a leading-digit name becomes a legal member"), Has(TEXT("int32 _200 = 0;")));
	TestTrue(TEXT("a C++ keyword is Pascal-cased out of the way"), Has(TEXT("int32 Class = 0;")));

	const FString Table = Read(TEXT("Private/FlockGeneratedWireNames.cpp"));
	TestTrue(TEXT("the sanitized member still writes its declared name"),
		Table.Contains(TEXT("{ TEXT(\"_200\"), TEXT(\"200\") }"), ESearchCase::CaseSensitive));

	// The expressible containers still come through, or degradation would be indistinguishable from a bug.
	TestTrue(TEXT("a list of scalars is a TArray"), Has(TEXT("TArray<FString> Tags;")));
	TestTrue(TEXT("a dict of scalars is a string-keyed TMap"), Has(TEXT("TMap<FString, int32> Counts;")));

	return true;
}

// ── Enums carry the wire value, which is not the member name ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCppTypeEnumTest, "Flock.Editor.CppTypes.EmitsEnumsWithWireLookup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCppTypeEnumTest::RunTest(const FString& Parameters)
{
	FFlockSchemaSnapshot Snapshot;
	Snapshot.GameVersionId = TEXT("ver-1");

	FFlockShop Shop;
	Shop.Id = TEXT("shop-1");
	FFlockShopItem Item;
	Item.Id = TEXT("item-1");
	Item.Name = TEXT("Gem Pack");
	Item.Currency = TEXT("Gold");
	Shop.ShopItems.Add(Item);
	Snapshot.Shops.Add(Shop);

	FFlockCppTypeEmitter::FEmitResult Result;
	const FString Header = EmitAndRead(Snapshot, TEXT("FlockShopItemId"), Result);
	auto Has = [&Header](const TCHAR* Needle) { return Header.Contains(Needle, ESearchCase::CaseSensitive); };

	// uint8, because UENUM(BlueprintType) requires it — a graph should see the same dropdown C++ does.
	TestTrue(TEXT("emitted as a BlueprintType enum"), Has(TEXT("UENUM(BlueprintType)")));
	TestTrue(TEXT("with a uint8 base"), Has(TEXT("enum class EFlockShopItemId : uint8")));
	TestTrue(TEXT("named for the item"), Has(TEXT("GemPack,")));

	// The display name is not the wire value: an item shows as GemPack and purchases by an opaque id.
	// Baking the mapping is what keeps the catalog out of a packaged build.
	TestTrue(TEXT("the wire lookup exists"), Has(TEXT("FlockShopItemIdToWire(EFlockShopItemId Value)")));
	TestTrue(TEXT("and maps the member to its id"),
		Has(TEXT("case EFlockShopItemId::GemPack: return TEXT(\"item-1\");")));

	// A game with no shop gets no shop enum: an empty `enum class` is not a useful dropdown, and a switch
	// over one does not compile.
	FFlockSchemaSnapshot Bare;
	Bare.GameVersionId = TEXT("ver-1");
	FFlockCppTypeEmitter::FEmitResult BareResult;
	TestTrue(TEXT("an empty catalog emits no enum"),
		EmitAndRead(Bare, TEXT("FlockShopItemId"), BareResult).IsEmpty());

	return true;
}

// ── A deleted template must lose its header, or the module compiles a type that no longer exists ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCppTypeResyncTest, "Flock.Editor.CppTypes.ReSyncReplacesHeaders",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCppTypeResyncTest::RunTest(const FString& Parameters)
{
	FFlockSchemaSnapshot First;
	First.GameVersionId = TEXT("ver-1");
	First.PlayerTemplates.Add(Template(TEXT("Wallet"), TEXT("[{\"type\":\"int\",\"field_name\":\"gold\"}]")));

	FFlockCppTypeEmitter::FEmitResult FirstResult;
	TestFalse(TEXT("the first sync wrote a header"),
		EmitAndRead(First, TEXT("FWalletTemplate"), FirstResult).IsEmpty());

	// The manifest header belongs to the module skeleton, not this emitter, and must survive a re-sync —
	// anything including it would otherwise stop compiling until a full sync ran.
	const FString ManifestPath = FPaths::Combine(Root(), TEXT("Public"), TEXT("FlockGeneratedManifest.h"));
	FFileHelper::SaveStringToFile(TEXT("// skeleton\n"), *ManifestPath);

	FFlockSchemaSnapshot Second;
	Second.GameVersionId = TEXT("ver-1");
	Second.PlayerTemplates.Add(Template(TEXT("Inventory"), TEXT("[{\"type\":\"int\",\"field_name\":\"slots\"}]")));

	const FFlockCppTypeEmitter::FEmitResult SecondResult =
		FFlockCppTypeEmitter::Emit(Second, Root(), TEXT("FlockGenerated"));
	TestTrue(TEXT("second sync succeeded"), SecondResult.bSucceeded);

	const FString PublicDir = FPaths::Combine(Root(), TEXT("Public"));
	TestTrue(TEXT("the new template has a header"),
		IFileManager::Get().FileExists(*FPaths::Combine(PublicDir, TEXT("FInventoryTemplate.h"))));
	TestFalse(TEXT("the dropped template's header is gone"),
		IFileManager::Get().FileExists(*FPaths::Combine(PublicDir, TEXT("FWalletTemplate.h"))));
	TestTrue(TEXT("but the skeleton's manifest header survives"),
		IFileManager::Get().FileExists(*ManifestPath));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
