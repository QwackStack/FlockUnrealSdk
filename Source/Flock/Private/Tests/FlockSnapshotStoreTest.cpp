// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Http/FlockSnapshotStore.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/FlockTemporaryFiles.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/Support/FlockTemporaryFilesTestSupport.h"
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
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

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
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

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
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

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
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

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
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

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
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSnapshotStateSurvivesPruneTest, "Flock.Http.SnapshotStore.StateSurvivesVersionBump",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockSnapshotStateSurvivesPruneTest::RunTest(const FString& Parameters)
{
	const FString Root = MakeTempRoot();
	{
		FFlockSnapshotStore Store(Root, MakeLogger(), TEXT("9.9.9"));

		// The defect: the offline write queue used to live at "<version>/command/<player>", so shipping a
		// build with a new GameVersionId deleted the player's unsent writes — no error, no log.
		const FString StateQueue = FString::Printf(TEXT("%s/command/p-1"), FFlockSnapshotStore::StateScope);
		Store.Write(StateQueue, TEXT("pending_writes"), TEXT("{\"op\":1}"));
		Store.Write(TEXT("ver-old/config"), TEXT("a"), TEXT("{\"n\":1}"));

		// Asserted before the prune so a write that never landed cannot masquerade as a prune that ate
		// it — the two have different fixes and the failure text should say which happened.
		FString Landed;
		TestTrue(TEXT("the state write landed"),
			Store.TryRead(StateQueue, TEXT("pending_writes"), Landed));

		Store.PruneOtherVersions(TEXT("ver-new"));

		FString Value;
		TestTrue(TEXT("queued writes survive a version change"),
			Store.TryRead(StateQueue, TEXT("pending_writes"), Value));
		TestFalse(TEXT("cached answers from another version are still pruned"),
			Store.TryRead(TEXT("ver-old/config"), TEXT("a"), Value));

		// And every subsequent one, not just the first.
		Store.PruneOtherVersions(TEXT("ver-newer-still"));
		TestTrue(TEXT("state survives repeated prunes"),
			Store.TryRead(StateQueue, TEXT("pending_writes"), Value));

		// A version id is a ULID, so it can never sanitize to the reserved name. Without that the pruner
		// would either eat state or spare a stale version, and neither would be visible.
		Store.PruneOtherVersions(TEXT("01KZ3ZY8RQHTXRK9VS8H89P296"));
		TestTrue(TEXT("a realistic ULID version does not collide with the state scope"),
			Store.TryRead(StateQueue, TEXT("pending_writes"), Value));
	}
	DeleteTempRoot(Root);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSnapshotLegacyStateMigrationTest, "Flock.Http.SnapshotStore.MigrateLegacyState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockSnapshotLegacyStateMigrationTest::RunTest(const FString& Parameters)
{
	const FString Root = MakeTempRoot();
	{
		FFlockSnapshotStore Store(Root, MakeLogger(), TEXT("9.9.9"));
		const FString StateQueue = FString::Printf(TEXT("%s/command/p-1"), FFlockSnapshotStore::StateScope);

		// Exactly what a pre-1.9.0 install has on disk.
		Store.Write(TEXT("ver-old/command/p-1"), TEXT("pending_writes"), TEXT("{\"op\":1}"));
		Store.Write(TEXT("ver-old/config"), TEXT("a"), TEXT("{\"n\":1}"));

		TestEqual(TEXT("one queue file moved"), Store.MigrateLegacyState({ TEXT("command") }), 1);
		Store.PruneOtherVersions(TEXT("ver-new"));

		FString Value;
		TestTrue(TEXT("the queued write survived the upgrade"),
			Store.TryRead(StateQueue, TEXT("pending_writes"), Value));
		TestFalse(TEXT("and is no longer in the version tree"),
			Store.TryRead(TEXT("ver-old/command/p-1"), TEXT("pending_writes"), Value));
		TestFalse(TEXT("cache beside it was left for the pruner"),
			Store.TryRead(TEXT("ver-old/config"), TEXT("a"), Value));

		// It runs on every subsystem init, so it has to be idempotent rather than merely correct once.
		TestEqual(TEXT("a second run moves nothing"), Store.MigrateLegacyState({ TEXT("command") }), 0);

		// Two versions can each hold a queue for one player — one from before an upgrade, one from a build
		// that was rolled back. The newer layout wins; the point is that neither is left for the pruner.
		Store.Write(TEXT("ver-older/command/p-1"), TEXT("pending_writes"), TEXT("{\"op\":2}"));
		Store.MigrateLegacyState({ TEXT("command") });
		TestTrue(TEXT("the already-migrated copy is kept"),
			Store.TryRead(StateQueue, TEXT("pending_writes"), Value));
		TestTrue(TEXT("and it is the newer one"), Value.Contains(TEXT("\"op\":1")));
		TestFalse(TEXT("the legacy copy is not left behind"),
			Store.TryRead(TEXT("ver-older/command/p-1"), TEXT("pending_writes"), Value));
	}
	DeleteTempRoot(Root);
	return true;
}

/**
 * A crash between writing a snapshot and moving it into place leaves its temporary file behind, in whichever scope it was
 * written. An old one is deleted when a store is built over the folder; a fresh one may be another launch's write.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSnapshotSweepsLeftOverTemporaryFilesTest, "Flock.Http.SnapshotStore.SweepsLeftOverTemporaryFiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockSnapshotSweepsLeftOverTemporaryFilesTest::RunTest(const FString& Parameters)
{
	const FString Root = MakeTempRoot();
	const FString LeftOver = FFlockTemporaryFiles::MakePath(FPaths::Combine(Root, TEXT("ver-1"), TEXT("config"), TEXT("a_00000000.json")));
	const FString LeftOverFixedName = FPaths::Combine(Root, TEXT("_state"), TEXT("command"), TEXT("p-1"), TEXT("pending_writes_00000000.json.tmp"));
	const FString BeingWritten = FFlockTemporaryFiles::MakePath(FPaths::Combine(Root, TEXT("ver-1"), TEXT("config"), TEXT("b_00000000.json")));
	for (const FString& Path : { LeftOver, LeftOverFixedName, BeingWritten })
	{
		TestTrue(TEXT("Precondition: a temporary file is saved"), FFileHelper::SaveStringToFile(TEXT("{\"v\":"), *Path));
	}
	FlockMoveTestFileTimeBack(LeftOver);
	FlockMoveTestFileTimeBack(LeftOverFixedName);

	{
		const FFlockSnapshotStore Store(Root, MakeLogger(), TEXT("9.9.9"));
		TestFalse(TEXT("A left-over temporary file in a nested scope is deleted"), IFileManager::Get().FileExists(*LeftOver));
		TestFalse(TEXT("So is one in the fixed-name form earlier builds wrote"), IFileManager::Get().FileExists(*LeftOverFixedName));
		TestTrue(TEXT("A fresh one is left for the launch that may be writing it"), IFileManager::Get().FileExists(*BeingWritten));
	}
	DeleteTempRoot(Root);
	return true;
}

/** Another launch of the game building its store between this launch's write and its move leaves the write alone. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSnapshotKeepsASnapshotAnotherLaunchIsWritingTest, "Flock.Http.SnapshotStore.KeepsASnapshotAnotherLaunchIsWriting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockSnapshotKeepsASnapshotAnotherLaunchIsWritingTest::RunTest(const FString& Parameters)
{
	const FString Root = MakeTempRoot();
	{
		const TSharedRef<FFlockRecordingLogger> Logger = MakeShared<FFlockRecordingLogger>();
		FFlockSnapshotStore Store(Root, Logger, TEXT("9.9.9"));
		TUniquePtr<FFlockSnapshotStore> AnotherLaunch;
		ON_SCOPE_EXIT { FFlockTemporaryFiles::SetBeforeNextMoveForTesting(nullptr); };
		FFlockTemporaryFiles::SetBeforeNextMoveForTesting([&AnotherLaunch, &Root](const FString&)
		{
			AnotherLaunch = MakeUnique<FFlockSnapshotStore>(Root, MakeLogger(), TEXT("9.9.9"));
		});

		int32 Complaints = 0;
		{
			FFlockFileManagerComplaintCounter Counter;
			Store.Write(TEXT("ver-1/config"), TEXT("game_config_x"), TEXT("{\"id\":\"cfg-1\"}"));
			Complaints = Counter.Count();
		}

		TestTrue(TEXT("Precondition: another launch built its store between the write and the move"), AnotherLaunch.IsValid());
		FString Payload;
		TestTrue(TEXT("The snapshot is written"), Store.TryRead(TEXT("ver-1/config"), TEXT("game_config_x"), Payload));
		TestTrue(TEXT("With its payload"), Payload.Contains(TEXT("\"id\":\"cfg-1\"")));
		TestEqual(TEXT("Nothing is warned about"), Logger->Warnings.Num(), 0);
		TestEqual(TEXT("The file manager logs no warning or error"), Complaints, 0);
	}
	DeleteTempRoot(Root);
	return true;
}

/** Two launches writing the same snapshot at once each write a file of their own, and both writes land. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSnapshotTwoLaunchesOneKeyTest, "Flock.Http.SnapshotStore.TwoLaunchesWritingOneKeyBothLand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockSnapshotTwoLaunchesOneKeyTest::RunTest(const FString& Parameters)
{
	const FString Root = MakeTempRoot();
	{
		const TSharedRef<FFlockRecordingLogger> ThisLogger = MakeShared<FFlockRecordingLogger>();
		const TSharedRef<FFlockRecordingLogger> AnotherLogger = MakeShared<FFlockRecordingLogger>();
		FFlockSnapshotStore ThisLaunch(Root, ThisLogger, TEXT("9.9.9"));
		FFlockSnapshotStore AnotherLaunch(Root, AnotherLogger, TEXT("9.9.9"));
		ON_SCOPE_EXIT { FFlockTemporaryFiles::SetBeforeNextMoveForTesting(nullptr); };
		// The other launch writes the same key after this launch has written its file and before this launch moves it.
		FFlockTemporaryFiles::SetBeforeNextMoveForTesting([&AnotherLaunch](const FString&)
		{
			AnotherLaunch.Write(TEXT("ver-1/config"), TEXT("game_config_x"), TEXT("{\"id\":\"from-another-launch\"}"));
		});

		int32 Complaints = 0;
		{
			FFlockFileManagerComplaintCounter Counter;
			ThisLaunch.Write(TEXT("ver-1/config"), TEXT("game_config_x"), TEXT("{\"id\":\"from-this-launch\"}"));
			Complaints = Counter.Count();
		}

		FString Payload;
		TestTrue(TEXT("The snapshot reads back"), ThisLaunch.TryRead(TEXT("ver-1/config"), TEXT("game_config_x"), Payload));
		TestTrue(TEXT("Holding the write that moved into place last"), Payload.Contains(TEXT("from-this-launch")));
		TestEqual(TEXT("This launch warns about nothing"), ThisLogger->Warnings.Num(), 0);
		TestEqual(TEXT("Nor does the other launch"), AnotherLogger->Warnings.Num(), 0);
		TestEqual(TEXT("The file manager logs no warning or error"), Complaints, 0);
		TArray<FString> Temps;
		IFileManager::Get().FindFilesRecursive(Temps, *Root, TEXT("*.tmp"), /*Files*/ true, /*Directories*/ false);
		TestEqual(TEXT("No temporary file is left"), Temps.Num(), 0);
	}
	DeleteTempRoot(Root);
	return true;
}

/** A write that cannot be moved into place gives up at once, says so, and leaves no temporary file behind. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSnapshotWriteGivesUpAtOnceTest, "Flock.Http.SnapshotStore.AWriteThatCannotMoveGivesUpAtOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockSnapshotWriteGivesUpAtOnceTest::RunTest(const FString& Parameters)
{
	const FString Root = MakeTempRoot();
	{
		const TSharedRef<FFlockRecordingLogger> Logger = MakeShared<FFlockRecordingLogger>();
		FFlockSnapshotStore Store(Root, Logger, TEXT("9.9.9"));
		Store.Write(TEXT("ver-1/config"), TEXT("game_config_x"), TEXT("{\"id\":\"cfg-1\"}"));
		TArray<FString> Written;
		IFileManager::Get().FindFilesRecursive(Written, *Root, TEXT("*.json"), /*Files*/ true, /*Directories*/ false);
		TestEqual(TEXT("Precondition: one snapshot is written"), Written.Num(), 1);
		if (Written.Num() == 1)
		{
			// A folder takes the snapshot's place, so a new one can be written beside it but never moved over it.
			IFileManager::Get().Delete(*Written[0]);
			IFileManager::Get().MakeDirectory(*Written[0], /*Tree*/ true);

			int32 Complaints = 0;
			const double StartedAt = FPlatformTime::Seconds();
			{
				FFlockFileManagerComplaintCounter Counter;
				Store.Write(TEXT("ver-1/config"), TEXT("game_config_x"), TEXT("{\"id\":\"cfg-2\"}"));
				Complaints = Counter.Count();
			}
			const double Seconds = FPlatformTime::Seconds() - StartedAt;

			TestTrue(FString::Printf(TEXT("It gives up at once instead of retrying on the game thread (took %.2f s)"), Seconds), Seconds < FlockTestSecondsWithoutARetry);
			TestEqual(TEXT("The file manager logs no warning or error"), Complaints, 0);
			TestTrue(TEXT("It warns that the snapshot was not saved"),
				FFlockRecordingLogger::AnyContains(Logger->Warnings, TEXT("could not save the file")));
			TArray<FString> Temps;
			IFileManager::Get().FindFilesRecursive(Temps, *Root, TEXT("*.tmp"), /*Files*/ true, /*Directories*/ false);
			TestEqual(TEXT("The temporary file written beside it is deleted"), Temps.Num(), 0);
		}
	}
	DeleteTempRoot(Root);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
