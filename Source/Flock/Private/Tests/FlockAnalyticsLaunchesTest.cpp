// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Analytics/FlockAnalyticsLaunches.h"
#include "Analytics/FlockFileEventCache.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "FlockSubsystem.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/FlockEngineCompat.h"
#include "Misc/FlockLaunchFolder.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/Support/FlockTestSafeIndex.h"

namespace
{
	/** The game the launches tests start launches for, and another one. */
	const TCHAR* const LaunchesTestGame = TEXT("0123456789abcdef");
	const TCHAR* const LaunchesOtherGame = TEXT("fedcba9876543210");

	/** An analytics folder of its own for one test, removed when the test ends. */
	struct FAnalyticsLaunchesTestRoot
	{
		FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
			FString::Printf(TEXT("analytics_launches_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));

		~FAnalyticsLaunchesTestRoot()
		{
			IFileManager::Get().DeleteDirectory(*Path, /*RequireExists*/ false, /*Tree*/ true);
		}
	};

	/** Queues Payload in the log events queue of the launch folder at LaunchFolder. */
	void QueueAnalyticsLaunchesTestEntry(const FString& LaunchFolder, const FString& Payload)
	{
		FFlockFileEventCache Queue(FFlockAnalyticsLaunches::LogEventsQueueName, 100, LaunchFolder);
		Queue.Enqueue(Payload);
	}

	/** The payloads queued in the log events queue of the launch folder at LaunchFolder, oldest first. */
	TArray<FString> QueuedAnalyticsLaunchesTestPayloads(const FString& LaunchFolder)
	{
		FFlockFileEventCache Queue(FFlockAnalyticsLaunches::LogEventsQueueName, 100, LaunchFolder);
		TArray<FString> Handles;
		TArray<FString> Payloads;
		Queue.PeekBatch(100, Handles, Payloads);
		return Payloads;
	}
}

/**
 * Two games started from one project folder each keep their own analytics files. A launch started while another still runs
 * takes none of that launch's files; one started after it ended takes all of them, once.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsLaunchesARunningLaunchKeepsItsFilesTest, "Flock.Analytics.Launches.ARunningLaunchKeepsItsFiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsLaunchesARunningLaunchKeepsItsFilesTest::RunTest(const FString& Parameters)
{
	const FAnalyticsLaunchesTestRoot Root;
	TSharedPtr<FFlockAnalyticsLaunches> First = FFlockAnalyticsLaunches::Start(Root.Path, LaunchesTestGame);
	TestTrue(TEXT("Precondition: the first launch holds its folder"), First->IsHoldingItsFolder());
	const FString FirstFolder = First->GetFolder();
	const FString FirstMarker = First->GetTerminationMarkerPath();
	const FString FirstRecord = First->GetSessionStatePath();
	TestTrue(TEXT("Precondition: the first launch leaves a marker"), FFileHelper::SaveStringToFile(TEXT("{\"session_id\":\"first\"}"), *FirstMarker));
	TestTrue(TEXT("Precondition: and a live-session record"), FFileHelper::SaveStringToFile(TEXT("{\"session_number\":1}"), *FirstRecord));
	QueueAnalyticsLaunchesTestEntry(FirstFolder, TEXT("{\"n\":1}"));

	const TSharedRef<FFlockAnalyticsLaunches> Second = FFlockAnalyticsLaunches::Start(Root.Path, LaunchesTestGame);
	TestTrue(TEXT("Precondition: the second launch holds a folder of its own"), Second->IsHoldingItsFolder()
		&& !Second->GetFolder().Equals(FirstFolder, ESearchCase::IgnoreCase));
	TestEqual(TEXT("A launch started while the first still runs takes nothing over"), Second->GetEndedLaunchRecords().Num(), 0);
	TestTrue(TEXT("The first launch's marker is left alone"), IFileManager::Get().FileExists(*FirstMarker));
	TestTrue(TEXT("So is its live-session record"), IFileManager::Get().FileExists(*FirstRecord));
	TestEqual(TEXT("And its queued entry"), QueuedAnalyticsLaunchesTestPayloads(FirstFolder).Num(), 1);
	TestEqual(TEXT("Which the second launch does not queue"), QueuedAnalyticsLaunchesTestPayloads(Second->GetFolder()).Num(), 0);

	// The first launch ends however it ends, and its lock goes with it.
	First.Reset();

	const TSharedRef<FFlockAnalyticsLaunches> Third = FFlockAnalyticsLaunches::Start(Root.Path, LaunchesTestGame);
	TestEqual(TEXT("A launch started after the first ended takes it over, and not the second, which still runs"),
		Third->GetEndedLaunchRecords().Num(), 1);
	if (Third->GetEndedLaunchRecords().Num() == 1)
	{
		TestEqual(TEXT("With its marker"), Third->GetEndedLaunchRecords()[0].TerminationMarkerPath, FirstMarker);
		TestEqual(TEXT("And its live-session record"), Third->GetEndedLaunchRecords()[0].SessionStatePath, FirstRecord);
	}
	const TArray<FString> Taken = QueuedAnalyticsLaunchesTestPayloads(Third->GetFolder());
	TestEqual(TEXT("Its queued entry moves into the third launch's queue"), Taken.Num(), 1);
	TestEqual(TEXT("Intact"), FlockTestAt(Taken, 0), FString(TEXT("{\"n\":1}")));
	TestEqual(TEXT("And leaves the first launch's queue"), QueuedAnalyticsLaunchesTestPayloads(FirstFolder).Num(), 0);

	Third->DeleteEndedLaunch(0);
	Third->LetGoOfEndedLaunches();
	TestFalse(TEXT("Once reported, the first launch's folder is deleted"), IFileManager::Get().DirectoryExists(*FirstFolder));
	TestTrue(TEXT("The second launch's folder is still there"), IFileManager::Get().DirectoryExists(*Second->GetFolder()));
	return true;
}

/** Every ended launch's queued entries move into the launch that takes them over, in the order they were queued. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsLaunchesTakesOverQueuedEntriesTest, "Flock.Analytics.Launches.TakesOverQueuedEntriesOfEveryEndedLaunch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsLaunchesTakesOverQueuedEntriesTest::RunTest(const FString& Parameters)
{
	const FAnalyticsLaunchesTestRoot Root;
	const FString LaunchesFolder = FFlockAnalyticsLaunches::LaunchesFolderOf(Root.Path, LaunchesTestGame);

	TSharedPtr<FFlockLaunchFolder> EarlierLaunch = FFlockLaunchFolder::Create(LaunchesFolder);
	TSharedPtr<FFlockLaunchFolder> LaterLaunch = FFlockLaunchFolder::Create(LaunchesFolder);
	TestTrue(TEXT("Precondition: two launches make folders"), EarlierLaunch.IsValid() && LaterLaunch.IsValid());
	if (!EarlierLaunch.IsValid() || !LaterLaunch.IsValid())
	{
		return true;
	}
	// Entries are named for when they were queued. The later launch queued one after the earlier launch's entry, and one named
	// exactly like it: two launches can reach the same millisecond and the same count.
	const FString EarlierQueue = FPaths::Combine(EarlierLaunch->GetPath(), FFlockAnalyticsLaunches::LogEventsQueueName);
	const FString LaterQueue = FPaths::Combine(LaterLaunch->GetPath(), FFlockAnalyticsLaunches::LogEventsQueueName);
	TestTrue(TEXT("Precondition: the earlier launch queued an entry"),
		FFileHelper::SaveStringToFile(TEXT("{\"n\":1}"), *FPaths::Combine(EarlierQueue, TEXT("1789000000001_00000001.json"))));
	TestTrue(TEXT("Precondition: the later launch queued one after it"),
		FFileHelper::SaveStringToFile(TEXT("{\"n\":2}"), *FPaths::Combine(LaterQueue, TEXT("1789000000002_00000001.json"))));
	TestTrue(TEXT("Precondition: and one named like the earlier launch's"),
		FFileHelper::SaveStringToFile(TEXT("{\"n\":3}"), *FPaths::Combine(LaterQueue, TEXT("1789000000001_00000001.json"))));
	EarlierLaunch.Reset();
	LaterLaunch.Reset();

	const TSharedRef<FFlockAnalyticsLaunches> Taking = FFlockAnalyticsLaunches::Start(Root.Path, LaunchesTestGame);
	TestEqual(TEXT("Both ended launches are taken over"), Taking->GetEndedLaunchRecords().Num(), 2);
	const TArray<FString> Payloads = QueuedAnalyticsLaunchesTestPayloads(Taking->GetFolder());
	TestEqual(TEXT("Every entry moves into this launch's queue, the one named alike included"), Payloads.Num(), 3);
	TestTrue(TEXT("The one named alike is kept"), Payloads.Contains(TEXT("{\"n\":3}")));
	TestTrue(TEXT("An entry queued earlier is still sent first"),
		Payloads.IndexOfByKey(TEXT("{\"n\":1}")) != INDEX_NONE && Payloads.IndexOfByKey(TEXT("{\"n\":1}")) < Payloads.IndexOfByKey(TEXT("{\"n\":2}")));
	return true;
}

/** Taking over a full queue is a start-up cost, so it is measured on the most entries a launch can leave. */
/**
 * A queued entry that cannot be moved out of an ended launch stays where it is: the launch's reported records go, and its folder
 * keeps the entry and its lock file, so a later launch takes the entry over rather than it being deleted with the folder.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsLaunchesAnEntryThatCannotBeMovedStaysTest, "Flock.Analytics.Launches.AnEntryThatCannotBeMovedStaysForALaterLaunch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsLaunchesAnEntryThatCannotBeMovedStaysTest::RunTest(const FString& Parameters)
{
	const FAnalyticsLaunchesTestRoot Root;
	const FString LaunchesFolder = FFlockAnalyticsLaunches::LaunchesFolderOf(Root.Path, LaunchesTestGame);
	TSharedPtr<FFlockLaunchFolder> EarlierLaunch = FFlockLaunchFolder::Create(LaunchesFolder);
	TSharedPtr<FFlockLaunchFolder> LaterLaunch = FFlockLaunchFolder::Create(LaunchesFolder);
	TestTrue(TEXT("Precondition: two launches make folders"), EarlierLaunch.IsValid() && LaterLaunch.IsValid());
	if (!EarlierLaunch.IsValid() || !LaterLaunch.IsValid())
	{
		return true;
	}
	// Folders made in the same second sort by their random ending, so both are renamed to names that sort in a known order.
	const FString EarlierFolder = FPaths::Combine(LaunchesFolder, TEXT("20000101-000000-aaaaaaaa"));
	const FString LaterFolder = FPaths::Combine(LaunchesFolder, TEXT("20000101-000001-bbbbbbbb"));
	const FString EarlierMade = EarlierLaunch->GetPath();
	const FString LaterMade = LaterLaunch->GetPath();
	EarlierLaunch.Reset();
	LaterLaunch.Reset();
	TestTrue(TEXT("Precondition: the earlier folder is named to sort first"), IFileManager::Get().Move(*EarlierFolder, *EarlierMade));
	TestTrue(TEXT("Precondition: the later folder is named to sort second"), IFileManager::Get().Move(*LaterFolder, *LaterMade));
	const FString LaterQueue = FPaths::Combine(LaterFolder, FFlockAnalyticsLaunches::LogEventsQueueName);
	const FString LaterRecord = FPaths::Combine(LaterFolder, FFlockAnalyticsLaunches::SessionStateFileName);
	// The earlier launch's queue becomes the taking launch's in one rename, so the later launch's entries are moved one by one.
	FFileHelper::SaveStringToFile(TEXT("{\"n\":1}"), *FPaths::Combine(EarlierFolder, FFlockAnalyticsLaunches::LogEventsQueueName,
		TEXT("1789000000001_00000001.json")));
	FFileHelper::SaveStringToFile(TEXT("{\"n\":2}"), *FPaths::Combine(LaterQueue, TEXT("1789000000002_00000001.json")));
	FFileHelper::SaveStringToFile(TEXT("{\"n\":3}"), *FPaths::Combine(LaterQueue, TEXT("1789000000003_00000001.json")));
	TestTrue(TEXT("Precondition: the later launch left a live-session record"), FFileHelper::SaveStringToFile(TEXT("{\"session_number\":1}"), *LaterRecord));

	FFlockAnalyticsLaunches::SetEntryMoveFailsForTesting([](const FString& EntryFileName)
	{
		return EntryFileName.Equals(TEXT("1789000000003_00000001.json"), ESearchCase::CaseSensitive);
	});
	const TSharedRef<FFlockAnalyticsLaunches> Taking = FFlockAnalyticsLaunches::Start(Root.Path, LaunchesTestGame);
	FFlockAnalyticsLaunches::SetEntryMoveFailsForTesting(nullptr);
	TestEqual(TEXT("Both ended launches are taken over"), Taking->GetEndedLaunchRecords().Num(), 2);
	TestEqual(TEXT("The entries that moved are this launch's"), QueuedAnalyticsLaunchesTestPayloads(Taking->GetFolder()).Num(), 2);
	TestTrue(TEXT("The later launch is marked as leaving an entry behind"),
		FlockTestAt(Taking->GetEndedLaunchRecords(), 1).bQueuedEntriesLeftBehind);

	Taking->DeleteEndedLaunch(0);
	Taking->DeleteEndedLaunch(1);
	Taking->LetGoOfEndedLaunches();
	TestTrue(TEXT("The entry that could not move is still there"),
		IFileManager::Get().FileExists(*FPaths::Combine(LaterQueue, TEXT("1789000000003_00000001.json"))));
	TestFalse(TEXT("The reported record is not, so it is never reported twice"), IFileManager::Get().FileExists(*LaterRecord));

	const TSharedRef<FFlockAnalyticsLaunches> Next = FFlockAnalyticsLaunches::Start(Root.Path, LaunchesTestGame);
	// The earlier launch was deleted and the taking launch still runs, so only the folder kept for the entry is taken over.
	TestEqual(TEXT("A later launch takes the folder over again"), Next->GetEndedLaunchRecords().Num(), 1);
	TestTrue(TEXT("And the entry with it"), QueuedAnalyticsLaunchesTestPayloads(Next->GetFolder()).Contains(TEXT("{\"n\":3}")));
	return true;
}

/** A launch that cannot hold a folder keeps nothing on disk, rather than a folder with no lock no later launch would ever clear. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsLaunchesALaunchWithNoFolderKeepsNothingTest, "Flock.Analytics.Launches.ALaunchWithNoFolderKeepsNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsLaunchesALaunchWithNoFolderKeepsNothingTest::RunTest(const FString& Parameters)
{
	const FAnalyticsLaunchesTestRoot Root;
	// A file where the launches folder goes: no launch folder or lock can be made under it.
	TestTrue(TEXT("Precondition: the launches folder's place is taken"),
		FFileHelper::SaveStringToFile(TEXT("not a folder"), *FFlockAnalyticsLaunches::LaunchesFolderOf(Root.Path, LaunchesTestGame)));

	const TSharedRef<FFlockAnalyticsLaunches> Launch = FFlockAnalyticsLaunches::Start(Root.Path, LaunchesTestGame);
	TestFalse(TEXT("Precondition: the launch holds no folder"), Launch->IsHoldingItsFolder());
	TestTrue(TEXT("It names no folder"), Launch->GetFolder().IsEmpty());
	TestTrue(TEXT("No session record path"), Launch->GetSessionStatePath().IsEmpty());
	TestTrue(TEXT("No crash marker path"), Launch->GetTerminationMarkerPath().IsEmpty());

	const FString QueueName = FString::Printf(TEXT("flock_test_queue_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FFlockFileEventCache Queue(QueueName, 100, Launch->GetFolder());
	TestTrue(TEXT("Its queue keeps nothing, so the entry goes straight out"), Queue.Enqueue(TEXT("{\"n\":1}")).IsEmpty());
	TestFalse(TEXT("And makes no folder where the game runs"), IFileManager::Get().DirectoryExists(*QueueName));
	TestFalse(TEXT("Nor beside the engine"), IFileManager::Get().DirectoryExists(*FPaths::Combine(FPlatformProcess::BaseDir(), QueueName)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsLaunchesTakesOverAFullQueueQuicklyTest, "Flock.Analytics.Launches.TakesOverAFullQueueQuickly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsLaunchesTakesOverAFullQueueQuicklyTest::RunTest(const FString& Parameters)
{
	/** The cap a project can set on each queue is in the hundreds; this is more than a launch is expected to leave. */
	const int32 EntriesQueued = 300;
	const FAnalyticsLaunchesTestRoot Root;
	TSharedPtr<FFlockLaunchFolder> EndedLaunch = FFlockLaunchFolder::Create(
		FFlockAnalyticsLaunches::LaunchesFolderOf(Root.Path, LaunchesTestGame));
	TestTrue(TEXT("Precondition: a launch makes a folder"), EndedLaunch.IsValid());
	if (!EndedLaunch.IsValid())
	{
		return true;
	}
	const FString Queue = FPaths::Combine(EndedLaunch->GetPath(), FFlockAnalyticsLaunches::LogEventsQueueName);
	for (int32 Entry = 0; Entry < EntriesQueued; ++Entry)
	{
		FFileHelper::SaveStringToFile(FString::Printf(TEXT("{\"n\":%d}"), Entry),
			*FPaths::Combine(Queue, FString::Printf(TEXT("178900000%04d_00000001.json"), Entry)));
	}
	EndedLaunch.Reset();

	const double StartedAt = FPlatformTime::Seconds();
	const TSharedRef<FFlockAnalyticsLaunches> Taking = FFlockAnalyticsLaunches::Start(Root.Path, LaunchesTestGame);
	const double Seconds = FPlatformTime::Seconds() - StartedAt;

	AddInfo(FString::Printf(TEXT("Taking over %d queued entries took %.3f s"), EntriesQueued, Seconds));
	TestEqual(TEXT("The ended launch is taken over"), Taking->GetEndedLaunchRecords().Num(), 1);
	// Counted on disk rather than read back through a cache, whose own limit is smaller than this queue.
	TArray<FString> TakenOver;
	IFileManager::Get().FindFiles(TakenOver, *FPaths::Combine(Taking->GetFolder(), FFlockAnalyticsLaunches::LogEventsQueueName, TEXT("*.json")),
		/*Files*/ true, /*Directories*/ false);
	TestEqual(TEXT("With every entry"), TakenOver.Num(), EntriesQueued);
	TestTrue(FString::Printf(TEXT("Without holding up the launch (took %.3f s)"), Seconds), Seconds < 2.5);
	return true;
}

/** A build before 1.16.0 kept one set of analytics files straight in the analytics folder; they are taken over once. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsLaunchesTakesOverEarlierBuildFilesTest, "Flock.Analytics.Launches.TakesOverFilesAnEarlierBuildLeft",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsLaunchesTakesOverEarlierBuildFilesTest::RunTest(const FString& Parameters)
{
	const FAnalyticsLaunchesTestRoot Root;
	const FString Record = FPaths::Combine(Root.Path, FFlockAnalyticsLaunches::SessionStateFileName);
	const FString Marker = FPaths::Combine(Root.Path, FFlockAnalyticsLaunches::TerminationMarkerFileName);
	const FString Consent = FPaths::Combine(Root.Path, TEXT("consent.json"));
	TestTrue(TEXT("Precondition: an earlier build's live-session record"), FFileHelper::SaveStringToFile(TEXT("{\"session_number\":4}"), *Record));
	TestTrue(TEXT("Precondition: its marker"), FFileHelper::SaveStringToFile(TEXT("{\"session_id\":\"earlier\"}"), *Marker));
	TestTrue(TEXT("Precondition: its queued entry"), FFileHelper::SaveStringToFile(TEXT("{\"n\":1}"),
		*FPaths::Combine(Root.Path, FFlockAnalyticsLaunches::LogEventsQueueName, TEXT("0000000000001_00000001.json"))));
	TestTrue(TEXT("Precondition: the install's consent decision"), FFileHelper::SaveStringToFile(TEXT("{\"granted\":false}"), *Consent));

	const TSharedRef<FFlockAnalyticsLaunches> First = FFlockAnalyticsLaunches::Start(Root.Path, LaunchesTestGame);
	TestEqual(TEXT("The earlier build's files are taken over as one ended launch"), First->GetEndedLaunchRecords().Num(), 1);
	if (First->GetEndedLaunchRecords().Num() == 1)
	{
		TestEqual(TEXT("With its live-session record"), First->GetEndedLaunchRecords()[0].SessionStatePath, Record);
		TestEqual(TEXT("And its marker"), First->GetEndedLaunchRecords()[0].TerminationMarkerPath, Marker);
	}
	TestEqual(TEXT("Its queued entry moves into this launch's queue"), QueuedAnalyticsLaunchesTestPayloads(First->GetFolder()).Num(), 1);

	const TSharedRef<FFlockAnalyticsLaunches> Second = FFlockAnalyticsLaunches::Start(Root.Path, LaunchesTestGame);
	TestEqual(TEXT("A launch started meanwhile does not take them over too"), Second->GetEndedLaunchRecords().Num(), 0);

	First->DeleteEndedLaunch(0);
	First->LetGoOfEndedLaunches();
	TestFalse(TEXT("Once reported, the record is deleted"), IFileManager::Get().FileExists(*Record));
	TestFalse(TEXT("So is the marker"), IFileManager::Get().FileExists(*Marker));
	TestFalse(TEXT("And the old queue folder"), IFileManager::Get().DirectoryExists(*FPaths::Combine(Root.Path, FFlockAnalyticsLaunches::LogEventsQueueName)));
	TestFalse(TEXT("And the lock that guarded them"),
		IFileManager::Get().FileExists(*FPaths::Combine(Root.Path, FFlockAnalyticsLaunches::EarlierBuildFilesLockName)));
	TestTrue(TEXT("The consent decision belongs to the install and stays"), IFileManager::Get().FileExists(*Consent));

	const TSharedRef<FFlockAnalyticsLaunches> Third = FFlockAnalyticsLaunches::Start(Root.Path, LaunchesTestGame);
	TestEqual(TEXT("Nothing is taken over twice"), Third->GetEndedLaunchRecords().Num(), 0);
	return true;
}

/** The running game's Flock holds a launch folder of its own under the project's analytics folder, from the real start-up path. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsLaunchesRunningGameHoldsItsFolderTest, "Flock.Analytics.Launches.TheRunningGameHoldsItsLaunchFolder",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsLaunchesRunningGameHoldsItsFolderTest::RunTest(const FString& Parameters)
{
	UFlockSubsystem* Flock = nullptr;
	if (GEngine != nullptr)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.OwningGameInstance != nullptr)
			{
				Flock = Context.OwningGameInstance->GetSubsystem<UFlockSubsystem>();
				if (Flock != nullptr)
				{
					break;
				}
			}
		}
	}
	TestTrue(TEXT("Precondition: the running game has a Flock subsystem"), Flock != nullptr);
	if (Flock == nullptr)
	{
		return true;
	}
	// A project with no Flock settings of its own -- a studio's, before they fill them in -- starts no SDK, so there is
	// no launch folder to check and nothing is wrong. Said out loud rather than failed: a failure here reads as the SDK
	// being broken in a project that has simply not been set up yet.
	if (!Flock->IsInitialized() || Flock->GetAnalyticsProvider() == nullptr)
	{
		AddInfo(TEXT("Skipped: the running game's Flock SDK is not initialized with analytics on, so it holds no launch folder. Fill in the Flock settings (API URL, API Key, Game Name, Game Version) to check this."));
		return true;
	}

	int32 HeldFolders = 0;
	int32 HeldFoldersHoldingTheQueues = 0;
	for (const FString& Folder : FFlockAnalyticsLaunches::FindLaunchFoldersOfEveryGame(FFlockAnalyticsLaunches::DefaultFolder()))
	{
		if (!IFileManager::Get().FileExists(*FPaths::Combine(Folder, FFlockLaunchFolder::LockFileName))
			|| FFlockLaunchFolder::ClaimEnded(Folder).IsValid())
		{
			continue;
		}
		++HeldFolders;
		if (IFileManager::Get().DirectoryExists(*FPaths::Combine(Folder, FFlockAnalyticsLaunches::LogEventsQueueName))
			&& IFileManager::Get().DirectoryExists(*FPaths::Combine(Folder, FFlockAnalyticsLaunches::SessionEndsQueueName))
			&& IFileManager::Get().DirectoryExists(*FPaths::Combine(Folder, FFlockAnalyticsLaunches::AnalyticsEventsQueueName)))
		{
			++HeldFoldersHoldingTheQueues;
		}
	}
	TestTrue(TEXT("The running game holds a launch folder of its own"), HeldFolders >= 1);
	// The queues belong to that folder, not to the analytics folder every launch shares.
	TestTrue(TEXT("With its three event queues inside it"), HeldFoldersHoldingTheQueues >= 1);
	TestFalse(TEXT("And no queue straight in the analytics folder"), IFileManager::Get().DirectoryExists(
		*FPaths::Combine(FFlockAnalyticsLaunches::DefaultFolder(), FFlockAnalyticsLaunches::LogEventsQueueName)));

	// Control: a folder nobody holds is claimed, so the count above is not of locks that nothing can open.
	TSharedPtr<FFlockLaunchFolder> Released = FFlockLaunchFolder::Create(FFlockAnalyticsLaunches::EveryGamesLaunchesFolder(FFlockAnalyticsLaunches::DefaultFolder()));
	TestTrue(TEXT("Precondition: a control folder is made"), Released.IsValid());
	if (Released.IsValid())
	{
		const FString ReleasedFolder = Released->GetPath();
		Released.Reset();
		const TSharedPtr<FFlockLaunchFolder> Claim = FFlockLaunchFolder::ClaimEnded(ReleasedFolder);
		TestTrue(TEXT("Control: a folder nobody holds is claimed"), Claim.IsValid());
		if (Claim.IsValid())
		{
			Claim->DeleteEverything();
		}
	}
	return true;
}

// ── A launch folder that cannot be removed once its lock has gone gets the lock back, so a later launch still takes it over ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLaunchFolderKeepsItsLockTest, "Flock.Analytics.Launches.AFolderThatCannotBeRemovedKeepsItsLock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLaunchFolderKeepsItsLockTest::RunTest(const FString& Parameters)
{
	const FString Parent = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
		TEXT("LaunchFolderLock"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*Parent, /*RequireExists*/ false, /*Tree*/ true);
	};
	TSharedPtr<FFlockLaunchFolder> Launch = FFlockLaunchFolder::Create(Parent);
	if (!TestTrue(TEXT("Precondition: a launch folder is made"), Launch.IsValid()))
	{
		return false;
	}
	const FString Folder = Launch->GetPath();
	const FString LockPath = FPaths::Combine(Folder, FFlockLaunchFolder::LockFileName);
	const FString Stray = FPaths::Combine(Folder, TEXT("arrived-meanwhile.txt"));
	FFlockLaunchFolder::SetBeforeNextFolderRemovalForTesting([Stray]()
	{
		FFileHelper::SaveStringToFile(TEXT("x"), *Stray);
	});

	TestFalse(TEXT("The folder is not reported gone"), Launch->DeleteEverything());
	Launch.Reset();
	TestTrue(TEXT("It is still there"), IFileManager::Get().DirectoryExists(*Folder));
	TestTrue(TEXT("With its lock back"), IFileManager::Get().FileExists(*LockPath));

	const TSharedPtr<FFlockLaunchFolder> Later = FFlockLaunchFolder::ClaimEnded(Folder);
	if (TestTrue(TEXT("A later launch takes it over"), Later.IsValid()))
	{
		TestTrue(TEXT("And deletes it"), Later->DeleteEverything());
		TestFalse(TEXT("The folder is gone"), IFileManager::Get().DirectoryExists(*Folder));
	}
	return true;
}

/** What an earlier build kept is older than any launch of the game's own, so it is handed over first. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsLaunchesOldestFirstTest, "Flock.Analytics.Launches.TheLaunchesAnEarlierBuildKeptAreHandedOverBeforeTheGamesOwn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsLaunchesOldestFirstTest::RunTest(const FString& Parameters)
{
	const FAnalyticsLaunchesTestRoot Root;
	FString EarlierFolder;
	{
		const TSharedPtr<FFlockLaunchFolder> Earlier = FFlockLaunchFolder::Create(FFlockAnalyticsLaunches::EveryGamesLaunchesFolder(Root.Path));
		const TSharedPtr<FFlockLaunchFolder> Own = FFlockLaunchFolder::Create(FFlockAnalyticsLaunches::LaunchesFolderOf(Root.Path, LaunchesTestGame));
		if (!TestTrue(TEXT("Precondition: both launch folders are made"), Earlier.IsValid() && Own.IsValid()))
		{
			return false;
		}
		EarlierFolder = Earlier->GetPath();
	}
	const TSharedRef<FFlockAnalyticsLaunches> Taking = FFlockAnalyticsLaunches::Start(Root.Path, LaunchesTestGame);
	if (!TestEqual(TEXT("Precondition: both are taken over"), Taking->GetEndedLaunchRecords().Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("The earlier build's launch comes first"), Taking->GetEndedLaunchRecords()[0].SessionStatePath.StartsWith(EarlierFolder));
	return true;
}

/** A project whose game changed: the old game's unsent entries never reach the new game, and wait for their own. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsLaunchesAnotherGamesTest, "Flock.Analytics.Launches.AnotherGamesEndedLaunchIsNotTakenOverButItsOwnGameTakesItOver",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsLaunchesAnotherGamesTest::RunTest(const FString& Parameters)
{
	const FAnalyticsLaunchesTestRoot Root;
	{
		const TSharedPtr<FFlockLaunchFolder> Theirs = FFlockLaunchFolder::Create(FFlockAnalyticsLaunches::LaunchesFolderOf(Root.Path, LaunchesOtherGame));
		if (!TestTrue(TEXT("Precondition: another game's launch folder is made"), Theirs.IsValid()))
		{
			return false;
		}
		QueueAnalyticsLaunchesTestEntry(Theirs->GetPath(), TEXT("theirs"));
	}

	const TSharedRef<FFlockAnalyticsLaunches> Ours = FFlockAnalyticsLaunches::Start(Root.Path, LaunchesTestGame);
	TestEqual(TEXT("This game takes over none of another game's launches"), Ours->GetEndedLaunchRecords().Num(), 0);
	TestEqual(TEXT("Nor any of its entries"), QueuedAnalyticsLaunchesTestPayloads(Ours->GetFolder()).Num(), 0);

	const TSharedRef<FFlockAnalyticsLaunches> TheirGame = FFlockAnalyticsLaunches::Start(Root.Path, LaunchesOtherGame);
	TestEqual(TEXT("Its own game takes it over"), TheirGame->GetEndedLaunchRecords().Num(), 1);
	TestEqualSensitive(TEXT("With its entry"), FString::Join(QueuedAnalyticsLaunchesTestPayloads(TheirGame->GetFolder()), TEXT(",")), FString(TEXT("theirs")));
	return true;
}

/** Builds before 1.33.0 kept every game's launches in one folder: the first game to start takes them over, once. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsLaunchesEveryGamesTest, "Flock.Analytics.Launches.TheLaunchesAnEarlierBuildKeptForEveryGameAreTakenOverByTheFirstGameOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsLaunchesEveryGamesTest::RunTest(const FString& Parameters)
{
	const FAnalyticsLaunchesTestRoot Root;
	{
		const TSharedPtr<FFlockLaunchFolder> Earlier = FFlockLaunchFolder::Create(FFlockAnalyticsLaunches::EveryGamesLaunchesFolder(Root.Path));
		if (!TestTrue(TEXT("Precondition: an earlier build's launch folder is made"), Earlier.IsValid()))
		{
			return false;
		}
		QueueAnalyticsLaunchesTestEntry(Earlier->GetPath(), TEXT("earlier"));
	}

	const TSharedRef<FFlockAnalyticsLaunches> First = FFlockAnalyticsLaunches::Start(Root.Path, LaunchesTestGame);
	TestEqual(TEXT("The first game to start takes it over"), First->GetEndedLaunchRecords().Num(), 1);
	TestEqualSensitive(TEXT("With its entry"), FString::Join(QueuedAnalyticsLaunchesTestPayloads(First->GetFolder()), TEXT(",")), FString(TEXT("earlier")));
	First->DeleteEndedLaunch(0);
	First->LetGoOfEndedLaunches();
	TestEqual(TEXT("Nothing is left where the earlier build kept them"),
		FFlockLaunchFolder::FindFolders(FFlockAnalyticsLaunches::EveryGamesLaunchesFolder(Root.Path)).Num(), 0);

	const TSharedRef<FFlockAnalyticsLaunches> Second = FFlockAnalyticsLaunches::Start(Root.Path, LaunchesOtherGame);
	TestEqual(TEXT("Taken over once: a second game finds nothing left"), Second->GetEndedLaunchRecords().Num(), 0);
	TestEqual(TEXT("And no entry"), QueuedAnalyticsLaunchesTestPayloads(Second->GetFolder()).Num(), 0);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
