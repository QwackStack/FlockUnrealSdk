// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Codegen/FlockCodegenManifest.h"
#include "Codegen/FlockCodegenPaths.h"
#include "Codegen/FlockSchemaHasher.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace FlockCodegenManifestTestHelpers
{
	inline FString TempRoot()
	{
		return FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
			FString::Printf(TEXT("codegen_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	inline void Cleanup(const FString& Dir)
	{
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
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

	inline FFlockShopItem Item(const FString& Id, const FString& Name, const FString& Currency, int32 Price)
	{
		FFlockShopItem Result;
		Result.Id = Id;
		Result.Name = Name;
		Result.Currency = Currency;
		Result.Price = Price;
		return Result;
	}

	/** A snapshot with one of everything, enough to move each part of the hash independently. */
	inline FFlockSchemaSnapshot Snapshot()
	{
		FFlockSchemaSnapshot Result;
		Result.GameVersionId = TEXT("ver-1");
		Result.BakedGameVersionId = TEXT("ver-1");
		Result.FetchedAtUtc = FDateTime(2026, 7, 28, 12, 0, 0);
		Result.PlayerTemplates.Add(Template(TEXT("tmpl-1"), TEXT("Wallet"), TEXT("currency"),
			TEXT("[{\"type\":\"int\",\"field_name\":\"coins\",\"type_name\":\"int\"}]")));

		FFlockGameConfigSchema Config;
		Config.Id = TEXT("cfg-1");
		Config.Name = TEXT("Gameplay");
		Config.Tag = TEXT("gameplay");
		Config.SchemaJson = TEXT("[{\"type\":\"string\",\"field_name\":\"mode\",\"type_name\":\"str\"}]");
		Result.GameConfigs.Add(Config);

		FFlockShop Shop;
		Shop.Id = TEXT("shop-1");
		Shop.Name = TEXT("Starter");
		Shop.ShopItems.Add(Item(TEXT("item-1"), TEXT("GemPack"), TEXT("Gold"), 100));
		Result.Shops.Add(Shop);
		return Result;
	}
}

using namespace FlockCodegenManifestTestHelpers;

// ── The hash moves for anything generated code is built from, and only for those things ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockHasherSensitivityTest, "Flock.Editor.CodegenHash.MovesOnGeneratedInputsOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockHasherSensitivityTest::RunTest(const FString& Parameters)
{
	const FString Baseline = FFlockSchemaHasher::ComputeContentHash(Snapshot());
	TestEqual(TEXT("stable across identical snapshots"), FFlockSchemaHasher::ComputeContentHash(Snapshot()), Baseline);
	TestEqual(TEXT("hash is 40 hex chars (SHA-1)"), Baseline.Len(), 40);

	auto Moves = [this, &Baseline](const TCHAR* What, const FFlockSchemaSnapshot& Changed)
	{
		TestNotEqual(FString::Printf(TEXT("hash moves: %s"), What),
			FFlockSchemaHasher::ComputeContentHash(Changed), Baseline);
	};
	auto Holds = [this, &Baseline](const TCHAR* What, const FFlockSchemaSnapshot& Changed)
	{
		TestEqual(FString::Printf(TEXT("hash holds: %s"), What),
			FFlockSchemaHasher::ComputeContentHash(Changed), Baseline);
	};

	// Everything the generated surface is built from.
	{ FFlockSchemaSnapshot S = Snapshot(); S.PlayerTemplates[0].Name = TEXT("Purse"); Moves(TEXT("template renamed"), S); }
	{ FFlockSchemaSnapshot S = Snapshot(); S.PlayerTemplates[0].Tag = TEXT("achievement"); Moves(TEXT("template retagged"), S); }
	{
		FFlockSchemaSnapshot S = Snapshot();
		S.PlayerTemplates[0].SchemaJson = TEXT("[{\"type\":\"int\",\"field_name\":\"gems\",\"type_name\":\"int\"}]");
		Moves(TEXT("field renamed"), S);
	}
	{
		FFlockSchemaSnapshot S = Snapshot();
		S.PlayerTemplates[0].SchemaJson = TEXT("[{\"type\":\"string\",\"field_name\":\"coins\",\"type_name\":\"str\"}]");
		Moves(TEXT("field retyped"), S);
	}
	{ FFlockSchemaSnapshot S = Snapshot(); S.GameConfigs[0].Name = TEXT("Combat"); Moves(TEXT("config renamed"), S); }
	{ FFlockSchemaSnapshot S = Snapshot(); S.Shops[0].ShopItems[0].Name = TEXT("BigGemPack"); Moves(TEXT("item renamed"), S); }
	{ FFlockSchemaSnapshot S = Snapshot(); S.Shops[0].ShopItems[0].Currency = TEXT("Shard"); Moves(TEXT("currency changed"), S); }
	{
		FFlockSchemaSnapshot S = Snapshot();
		S.Shops[0].ShopItems.Add(Item(TEXT("item-2"), TEXT("Extra"), TEXT("Gold"), 5));
		Moves(TEXT("item added"), S);
	}

	// Live-read data must NOT move it, or every price edit would demand a regen that changed nothing.
	{ FFlockSchemaSnapshot S = Snapshot(); S.Shops[0].ShopItems[0].Price = 999; Holds(TEXT("price changed"), S); }
	{ FFlockSchemaSnapshot S = Snapshot(); S.FetchedAtUtc = FDateTime::UtcNow(); Holds(TEXT("fetch time"), S); }
	{ FFlockSchemaSnapshot S = Snapshot(); S.BakedGameVersionId = TEXT("ver-9"); Holds(TEXT("baked id"), S); }

	return true;
}

// ── Serialization noise is not drift; real reordering of fields is ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockHasherCanonicalTest, "Flock.Editor.CodegenHash.IgnoresSerializationNoise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockHasherCanonicalTest::RunTest(const FString& Parameters)
{
	const FString Baseline = FFlockSchemaHasher::ComputeContentHash(Snapshot());

	// Same schema, different whitespace and different key order within the field object.
	FFlockSchemaSnapshot Reformatted = Snapshot();
	Reformatted.PlayerTemplates[0].SchemaJson =
		TEXT("[ {\n  \"field_name\" : \"coins\",\n  \"type_name\": \"int\",\n  \"type\":\"int\"\n } ]");
	TestEqual(TEXT("whitespace and key order are not drift"),
		FFlockSchemaHasher::ComputeContentHash(Reformatted), Baseline);

	// Entity order from the backend is not drift either — the emitters sort, so files are unchanged.
	FFlockSchemaSnapshot Reordered = Snapshot();
	Reordered.PlayerTemplates.Add(Template(TEXT("tmpl-0"), TEXT("Trophies"), TEXT("achievement"), TEXT("[]")));
	FFlockSchemaSnapshot ReorderedOtherWay = Reordered;
	Algo::Reverse(ReorderedOtherWay.PlayerTemplates);
	TestEqual(TEXT("template order from the server is not drift"),
		FFlockSchemaHasher::ComputeContentHash(ReorderedOtherWay),
		FFlockSchemaHasher::ComputeContentHash(Reordered));

	// But field order inside a schema IS drift: it becomes member order in the generated struct.
	FFlockSchemaSnapshot FieldsSwapped = Snapshot();
	FieldsSwapped.PlayerTemplates[0].SchemaJson =
		TEXT("[{\"type\":\"int\",\"field_name\":\"a\"},{\"type\":\"int\",\"field_name\":\"b\"}]");
	FFlockSchemaSnapshot FieldsSwappedBack = Snapshot();
	FieldsSwappedBack.PlayerTemplates[0].SchemaJson =
		TEXT("[{\"type\":\"int\",\"field_name\":\"b\"},{\"type\":\"int\",\"field_name\":\"a\"}]");
	TestNotEqual(TEXT("field order is drift"),
		FFlockSchemaHasher::ComputeContentHash(FieldsSwapped),
		FFlockSchemaHasher::ComputeContentHash(FieldsSwappedBack));

	// An unparseable schema still hashes stably rather than collapsing to empty.
	FFlockSchemaSnapshot Broken = Snapshot();
	Broken.PlayerTemplates[0].SchemaJson = TEXT("{not json");
	TestEqual(TEXT("unparseable schema is stable"),
		FFlockSchemaHasher::ComputeContentHash(Broken), FFlockSchemaHasher::ComputeContentHash(Broken));
	TestNotEqual(TEXT("unparseable schema still differs from the good one"),
		FFlockSchemaHasher::ComputeContentHash(Broken), Baseline);

	return true;
}

// ── The manifest round-trips through disk ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockManifestRoundTripTest, "Flock.Editor.CodegenManifest.RoundTripsOnDisk",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockManifestRoundTripTest::RunTest(const FString& Parameters)
{
	const FString Root = TempRoot();
	const FFlockCodegenManifest Written = FFlockCodegenManifest::FromSnapshot(Snapshot());

	FString Error;
	// The directory does not exist yet — writing must create it, since a first sync has nowhere to write.
	TestTrue(TEXT("write succeeds into a missing folder"), FFlockCodegenManifest::Write(Root, Written, Error));
	TestEqual(TEXT("no error"), Error, FString());

	FFlockCodegenManifest Read;
	TestTrue(TEXT("read succeeds"), FFlockCodegenManifest::TryRead(Root, Read));
	TestEqual(TEXT("version id"), Read.GameVersionId, Written.GameVersionId);
	TestEqual(TEXT("content hash"), Read.ContentHash, Written.ContentHash);
	TestEqual(TEXT("template count"), Read.PlayerTemplateCount, 1);
	TestEqual(TEXT("config count"), Read.GameConfigCount, 1);
	TestEqual(TEXT("shop count"), Read.ShopCount, 1);
	TestFalse(TEXT("sdk version stamped"), Read.SdkVersion.IsEmpty());
	TestFalse(TEXT("generated-at stamped"), Read.GeneratedAtUtc.IsEmpty());

	Cleanup(Root);
	return true;
}

// ── Every drift verdict, including the ones that must not read as Current ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockManifestDriftTest, "Flock.Editor.CodegenManifest.DetectsEveryDriftKind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockManifestDriftTest::RunTest(const FString& Parameters)
{
	const FFlockSchemaSnapshot Fresh = Snapshot();
	const FFlockCodegenManifest Stored = FFlockCodegenManifest::FromSnapshot(Fresh);

	TestEqual(TEXT("unchanged is Current"),
		FFlockCodegenManifest::Compare(Stored, Fresh), EFlockCodegenDrift::Current);

	// A new version under the same name.
	{
		FFlockSchemaSnapshot Newer = Snapshot();
		Newer.GameVersionId = TEXT("ver-2");
		TestEqual(TEXT("new version is VersionChanged"),
			FFlockCodegenManifest::Compare(Stored, Newer), EFlockCodegenDrift::VersionChanged);
	}

	// The case an id comparison alone cannot see: same version, edited schema.
	{
		FFlockSchemaSnapshot Edited = Snapshot();
		Edited.PlayerTemplates[0].SchemaJson =
			TEXT("[{\"type\":\"int\",\"field_name\":\"coins\"},{\"type\":\"int\",\"field_name\":\"gems\"}]");
		TestEqual(TEXT("edited schema is ContentChanged"),
			FFlockCodegenManifest::Compare(Stored, Edited), EFlockCodegenDrift::ContentChanged);
	}

	// An empty manifest is "never generated", not "current".
	TestEqual(TEXT("empty manifest is NeverGenerated"),
		FFlockCodegenManifest::Compare(FFlockCodegenManifest(), Fresh), EFlockCodegenDrift::NeverGenerated);

	// A manifest with no hash cannot be verified, so it must report drift rather than claim Current.
	{
		FFlockCodegenManifest NoHash = Stored;
		NoHash.ContentHash.Reset();
		TestEqual(TEXT("hashless manifest is ContentChanged"),
			FFlockCodegenManifest::Compare(NoHash, Fresh), EFlockCodegenDrift::ContentChanged);
	}

	// Every verdict says something actionable.
	for (const EFlockCodegenDrift Drift : { EFlockCodegenDrift::Current, EFlockCodegenDrift::NeverGenerated,
		EFlockCodegenDrift::VersionChanged, EFlockCodegenDrift::ContentChanged })
	{
		TestFalse(TEXT("drift is described"), FFlockCodegenManifest::Describe(Drift, Stored, Fresh).IsEmpty());
	}

	return true;
}

// ── An absent or corrupt manifest reads as "never generated", never as a crash ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockManifestMissingTest, "Flock.Editor.CodegenManifest.MissingOrCorruptIsNeverGenerated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockManifestMissingTest::RunTest(const FString& Parameters)
{
	const FString Root = TempRoot();
	FFlockCodegenManifest Read;
	TestFalse(TEXT("absent folder"), FFlockCodegenManifest::TryRead(Root, Read));

	FFileHelper::SaveStringToFile(FString(TEXT("{not json")), *FFlockCodegenPaths::ManifestPath(Root));
	TestFalse(TEXT("corrupt file"), FFlockCodegenManifest::TryRead(Root, Read));

	// Well-formed JSON that is not a manifest must also be rejected — a missing version id would
	// otherwise compare equal to a snapshot whose resolve also failed.
	FFileHelper::SaveStringToFile(FString(TEXT("{\"unrelated\":true}")), *FFlockCodegenPaths::ManifestPath(Root));
	TestFalse(TEXT("json without a version id"), FFlockCodegenManifest::TryRead(Root, Read));

	Cleanup(Root);
	return true;
}

// ── The generated root is guarded, because regenerating deletes what is inside it ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCodegenPathGuardTest, "Flock.Editor.CodegenPaths.RejectsPathsOutsideTheProject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCodegenPathGuardTest::RunTest(const FString& Parameters)
{
	FString Root;
	FString Error;

	TestTrue(TEXT("default path resolves"), FFlockCodegenPaths::TryResolveGeneratedRoot(FString(), Root, Error));
	TestTrue(TEXT("default lands in the project"),
		Root.Contains(TEXT("Source/FlockGenerated")) || Root.Contains(TEXT("Source\\FlockGenerated")));

	TestTrue(TEXT("a subfolder resolves"),
		FFlockCodegenPaths::TryResolveGeneratedRoot(TEXT("Source/MyGenerated"), Root, Error));
	TestTrue(TEXT("backslashes are accepted"),
		FFlockCodegenPaths::TryResolveGeneratedRoot(TEXT("Source\\MyGenerated"), Root, Error));

	// The three that would delete something that is not ours.
	TestFalse(TEXT("escaping the project is rejected"),
		FFlockCodegenPaths::TryResolveGeneratedRoot(TEXT("../../../Somewhere"), Root, Error));
	TestFalse(TEXT("rejection explains itself"), Error.IsEmpty());

	TestFalse(TEXT("an absolute path is rejected"),
		FFlockCodegenPaths::TryResolveGeneratedRoot(TEXT("C:/Windows"), Root, Error));

	// Every spelling of "the project root", because each one would wipe the whole project on a clean.
	TestFalse(TEXT("the project root itself is rejected"),
		FFlockCodegenPaths::TryResolveGeneratedRoot(TEXT("."), Root, Error));
	TestFalse(TEXT("./ is rejected"),
		FFlockCodegenPaths::TryResolveGeneratedRoot(TEXT("./"), Root, Error));
	TestFalse(TEXT("a round trip back to the root is rejected"),
		FFlockCodegenPaths::TryResolveGeneratedRoot(TEXT("Source/.."), Root, Error));

	// A sibling folder whose name starts with the project's must not pass as being inside it.
	TestFalse(TEXT("a same-prefixed sibling is rejected"),
		FFlockCodegenPaths::TryResolveGeneratedRoot(TEXT("../UEBuildEnviromentEvil/Generated"), Root, Error));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
