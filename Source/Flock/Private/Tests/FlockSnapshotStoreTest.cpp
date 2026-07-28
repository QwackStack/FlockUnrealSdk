// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Http/FlockSnapshotStore.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/Support/FlockRecordingLogger.h"

namespace
{
	/** A throwaway root per case so cases never see each other's snapshots. */
	FString MakeTempRoot()
	{
		return FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
			FString::Printf(TEXT("snap_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	void DeleteTempRoot(const FString& Root)
	{
		IFileManager::Get().DeleteDirectory(*Root, false, true);
	}

	TSharedRef<IFlockLogger> MakeLogger()
	{
		return MakeShared<FFlockRecordingLogger>();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSnapshotRoundTripTest, "Flock.Http.SnapshotStore.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSnapshotRoundTripTest::RunTest(const FString& Parameters)
{
	const FString Root = MakeTempRoot();
	{
		FFlockSnapshotStore Store(Root, MakeLogger(), TEXT("9.9.9"));

		// A miss before anything is written.
		FString Miss;
		TestFalse(TEXT("absent key is a miss"), Store.TryRead(TEXT("ver-1/config"), TEXT("game_config_x"), Miss));

		// An object payload round-trips (field values preserved; key order is not guaranteed, so assert content).
		Store.Write(TEXT("ver-1/config"), TEXT("game_config_x"), TEXT("{\"id\":\"cfg-1\",\"tag\":\"gameplay\"}"));
		FString Object;
		TestTrue(TEXT("object reads back"), Store.TryRead(TEXT("ver-1/config"), TEXT("game_config_x"), Object));
		TestTrue(TEXT("object keeps id"), Object.Contains(TEXT("\"id\":\"cfg-1\"")));
		TestTrue(TEXT("object keeps tag"), Object.Contains(TEXT("\"tag\":\"gameplay\"")));

		// An array payload round-trips too (the list-route shape).
		Store.Write(TEXT("ver-1/config"), TEXT("game_patch_all"), TEXT("[{\"id\":\"p1\"},{\"id\":\"p2\"}]"));
		FString Array;
		TestTrue(TEXT("array reads back"), Store.TryRead(TEXT("ver-1/config"), TEXT("game_patch_all"), Array));
		TestTrue(TEXT("array is an array"), Array.StartsWith(TEXT("[")));
		TestTrue(TEXT("array keeps p1"), Array.Contains(TEXT("\"id\":\"p1\"")));
		TestTrue(TEXT("array keeps p2"), Array.Contains(TEXT("\"id\":\"p2\"")));

		// A second write to the same key overwrites in place.
		Store.Write(TEXT("ver-1/config"), TEXT("game_config_x"), TEXT("{\"id\":\"cfg-2\"}"));
		FString Overwritten;
		TestTrue(TEXT("reads overwrite"), Store.TryRead(TEXT("ver-1/config"), TEXT("game_config_x"), Overwritten));
		TestTrue(TEXT("overwrite took"), Overwritten.Contains(TEXT("\"id\":\"cfg-2\"")));

		// A non-JSON payload is refused, not stored under the key.
		Store.Write(TEXT("ver-1/config"), TEXT("bad_key"), TEXT("not json"));
		FString Bad;
		TestFalse(TEXT("non-JSON payload not stored"), Store.TryRead(TEXT("ver-1/config"), TEXT("bad_key"), Bad));
	}
	DeleteTempRoot(Root);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSnapshotVersionMismatchTest, "Flock.Http.SnapshotStore.VersionMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSnapshotVersionMismatchTest::RunTest(const FString& Parameters)
{
	const FString Root = MakeTempRoot();
	{
		FFlockSnapshotStore Store(Root, MakeLogger(), TEXT("9.9.9"));
		Store.Write(TEXT("ver-1/config"), TEXT("game_config_x"), TEXT("{\"id\":\"cfg-1\"}"));

		// Locate the file and rewrite its envelope with a future version number.
		const FString ScopeDir = FPaths::Combine(Root, TEXT("ver-1"), TEXT("config"));
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *FPaths::Combine(ScopeDir, TEXT("*.json")), true, false);
		TestEqual(TEXT("one snapshot file exists"), Files.Num(), 1);
		if (Files.Num() == 1)
		{
			const FString Path = FPaths::Combine(ScopeDir, Files[0]);
			FFileHelper::SaveStringToFile(TEXT("{\"v\":999,\"sdk\":\"9.9.9\",\"data\":{\"id\":\"cfg-1\"}}"), *Path);

			FString Value;
			TestFalse(TEXT("wrong version reads as a miss"), Store.TryRead(TEXT("ver-1/config"), TEXT("game_config_x"), Value));
			// A stale-version file is invalidated (deleted) so the next fetch repopulates it.
			TestFalse(TEXT("stale file deleted"), IFileManager::Get().FileExists(*Path));
		}
	}
	DeleteTempRoot(Root);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSnapshotCorruptTest, "Flock.Http.SnapshotStore.Corrupt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSnapshotCorruptTest::RunTest(const FString& Parameters)
{
	const FString Root = MakeTempRoot();
	{
		FFlockSnapshotStore Store(Root, MakeLogger(), TEXT("9.9.9"));
		Store.Write(TEXT("ver-1/config"), TEXT("game_config_x"), TEXT("{\"id\":\"cfg-1\"}"));

		const FString ScopeDir = FPaths::Combine(Root, TEXT("ver-1"), TEXT("config"));
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *FPaths::Combine(ScopeDir, TEXT("*.json")), true, false);
		if (Files.Num() == 1)
		{
			const FString Path = FPaths::Combine(ScopeDir, Files[0]);
			FFileHelper::SaveStringToFile(TEXT("{ this is not json"), *Path);

			FString Value;
			TestFalse(TEXT("corrupt reads as a miss"), Store.TryRead(TEXT("ver-1/config"), TEXT("game_config_x"), Value));
			TestFalse(TEXT("corrupt file deleted"), IFileManager::Get().FileExists(*Path));
		}
	}
	DeleteTempRoot(Root);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSnapshotKeyCollisionTest, "Flock.Http.SnapshotStore.KeyCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSnapshotKeyCollisionTest::RunTest(const FString& Parameters)
{
	const FString Root = MakeTempRoot();
	{
		FFlockSnapshotStore Store(Root, MakeLogger(), TEXT("9.9.9"));

		// Two keys that sanitize to the same prefix ("a/b" and "a:b" both -> "a_b") must not collide:
		// the appended hash disambiguates them, so each keeps its own payload.
		Store.Write(TEXT("ver-1/config"), TEXT("a/b"), TEXT("{\"which\":\"slash\"}"));
		Store.Write(TEXT("ver-1/config"), TEXT("a:b"), TEXT("{\"which\":\"colon\"}"));

		FString Slash;
		FString Colon;
		TestTrue(TEXT("slash key reads"), Store.TryRead(TEXT("ver-1/config"), TEXT("a/b"), Slash));
		TestTrue(TEXT("colon key reads"), Store.TryRead(TEXT("ver-1/config"), TEXT("a:b"), Colon));
		TestTrue(TEXT("slash payload intact"), Slash.Contains(TEXT("slash")));
		TestTrue(TEXT("colon payload intact"), Colon.Contains(TEXT("colon")));
	}
	DeleteTempRoot(Root);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSnapshotDeleteScopeTest, "Flock.Http.SnapshotStore.DeleteScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSnapshotDeleteScopeTest::RunTest(const FString& Parameters)
{
	const FString Root = MakeTempRoot();
	{
		FFlockSnapshotStore Store(Root, MakeLogger(), TEXT("9.9.9"));
		Store.Write(TEXT("ver-1/config"), TEXT("a"), TEXT("{\"n\":1}"));
		Store.Write(TEXT("ver-1/config"), TEXT("b"), TEXT("{\"n\":2}"));
		Store.Write(TEXT("ver-1/game"), TEXT("game"), TEXT("{\"n\":3}"));

		Store.DeleteScope(TEXT("ver-1/config"));

		FString Value;
		TestFalse(TEXT("config a gone"), Store.TryRead(TEXT("ver-1/config"), TEXT("a"), Value));
		TestFalse(TEXT("config b gone"), Store.TryRead(TEXT("ver-1/config"), TEXT("b"), Value));
		TestTrue(TEXT("sibling scope untouched"), Store.TryRead(TEXT("ver-1/game"), TEXT("game"), Value));
	}
	DeleteTempRoot(Root);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSnapshotPruneTest, "Flock.Http.SnapshotStore.PruneOtherVersions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSnapshotPruneTest::RunTest(const FString& Parameters)
{
	const FString Root = MakeTempRoot();
	{
		FFlockSnapshotStore Store(Root, MakeLogger(), TEXT("9.9.9"));
		Store.Write(TEXT("ver-1/config"), TEXT("a"), TEXT("{\"n\":1}"));
		Store.Write(TEXT("ver-2/config"), TEXT("a"), TEXT("{\"n\":2}"));
		Store.Write(FFlockSnapshotStore::BootstrapScope, TEXT("resolve"), TEXT("{\"n\":3}"));

		// Keep the current version; other version trees go, bootstrap stays.
		Store.PruneOtherVersions(TEXT("ver-1"));

		FString Value;
		TestTrue(TEXT("current version kept"), Store.TryRead(TEXT("ver-1/config"), TEXT("a"), Value));
		TestFalse(TEXT("other version pruned"), Store.TryRead(TEXT("ver-2/config"), TEXT("a"), Value));
		TestTrue(TEXT("bootstrap kept"), Store.TryRead(FFlockSnapshotStore::BootstrapScope, TEXT("resolve"), Value));
	}
	DeleteTempRoot(Root);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
