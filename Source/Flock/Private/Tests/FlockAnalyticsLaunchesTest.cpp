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
#include "Misc/FlockLaunchFolder.h"
#include "Misc/Paths.h"
#include "Tests/Support/FlockTestSafeIndex.h"

namespace
{
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
	TSharedPtr<FFlockAnalyticsLaunches> First = FFlockAnalyticsLaunches::Start(Root.Path);
	TestTrue(TEXT("Precondition: the first launch holds its folder"), First->IsHoldingItsFolder());
	const FString FirstFolder = First->GetFolder();
	const FString FirstMarker = First->GetTerminationMarkerPath();
	const FString FirstRecord = First->GetSessionStatePath();
	TestTrue(TEXT("Precondition: the first launch leaves a marker"), FFileHelper::SaveStringToFile(TEXT("{\"session_id\":\"first\"}"), *FirstMarker));
	TestTrue(TEXT("Precondition: and a live-session record"), FFileHelper::SaveStringToFile(TEXT("{\"session_number\":1}"), *FirstRecord));
	QueueAnalyticsLaunchesTestEntry(FirstFolder, TEXT("{\"n\":1}"));

	const TSharedRef<FFlockAnalyticsLaunches> Second = FFlockAnalyticsLaunches::Start(Root.Path);
	TestTrue(TEXT("Precondition: the second launch holds a folder of its own"), Second->IsHoldingItsFolder()
		&& !Second->GetFolder().Equals(FirstFolder, ESearchCase::IgnoreCase));
	TestEqual(TEXT("A launch started while the first still runs takes nothing over"), Second->GetEndedLaunchRecords().Num(), 0);
	TestTrue(TEXT("The first launch's marker is left alone"), IFileManager::Get().FileExists(*FirstMarker));
	TestTrue(TEXT("So is its live-session record"), IFileManager::Get().FileExists(*FirstRecord));
	TestEqual(TEXT("And its queued entry"), QueuedAnalyticsLaunchesTestPayloads(FirstFolder).Num(), 1);
	TestEqual(TEXT("Which the second launch does not queue"), QueuedAnalyticsLaunchesTestPayloads(Second->GetFolder()).Num(), 0);

	// The first launch ends however it ends, and its lock goes with it.
	First.Reset();

	const TSharedRef<FFlockAnalyticsLaunches> Third = FFlockAnalyticsLaunches::Start(Root.Path);
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
	const FString LaunchesFolder = FPaths::Combine(Root.Path, FFlockAnalyticsLaunches::LaunchesFolderName);

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

	const TSharedRef<FFlockAnalyticsLaunches> Taking = FFlockAnalyticsLaunches::Start(Root.Path);
	TestEqual(TEXT("Both ended launches are taken over"), Taking->GetEndedLaunchRecords().Num(), 2);
	const TArray<FString> Payloads = QueuedAnalyticsLaunchesTestPayloads(Taking->GetFolder());
	TestEqual(TEXT("Every entry moves into this launch's queue, the one named alike included"), Payloads.Num(), 3);
	TestTrue(TEXT("The one named alike is kept"), Payloads.Contains(TEXT("{\"n\":3}")));
	TestTrue(TEXT("An entry queued earlier is still sent first"),
		Payloads.IndexOfByKey(TEXT("{\"n\":1}")) != INDEX_NONE && Payloads.IndexOfByKey(TEXT("{\"n\":1}")) < Payloads.IndexOfByKey(TEXT("{\"n\":2}")));
	return true;
}

/** Taking over a full queue is a start-up cost, so it is measured on the most entries a launch can leave. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsLaunchesTakesOverAFullQueueQuicklyTest, "Flock.Analytics.Launches.TakesOverAFullQueueQuickly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsLaunchesTakesOverAFullQueueQuicklyTest::RunTest(const FString& Parameters)
{
	/** The cap a project can set on each queue is in the hundreds; this is more than a launch is expected to leave. */
	const int32 EntriesQueued = 300;
	const FAnalyticsLaunchesTestRoot Root;
	TSharedPtr<FFlockLaunchFolder> EndedLaunch = FFlockLaunchFolder::Create(
		FPaths::Combine(Root.Path, FFlockAnalyticsLaunches::LaunchesFolderName));
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
	const TSharedRef<FFlockAnalyticsLaunches> Taking = FFlockAnalyticsLaunches::Start(Root.Path);
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

	const TSharedRef<FFlockAnalyticsLaunches> First = FFlockAnalyticsLaunches::Start(Root.Path);
	TestEqual(TEXT("The earlier build's files are taken over as one ended launch"), First->GetEndedLaunchRecords().Num(), 1);
	if (First->GetEndedLaunchRecords().Num() == 1)
	{
		TestEqual(TEXT("With its live-session record"), First->GetEndedLaunchRecords()[0].SessionStatePath, Record);
		TestEqual(TEXT("And its marker"), First->GetEndedLaunchRecords()[0].TerminationMarkerPath, Marker);
	}
	TestEqual(TEXT("Its queued entry moves into this launch's queue"), QueuedAnalyticsLaunchesTestPayloads(First->GetFolder()).Num(), 1);

	const TSharedRef<FFlockAnalyticsLaunches> Second = FFlockAnalyticsLaunches::Start(Root.Path);
	TestEqual(TEXT("A launch started meanwhile does not take them over too"), Second->GetEndedLaunchRecords().Num(), 0);

	First->DeleteEndedLaunch(0);
	First->LetGoOfEndedLaunches();
	TestFalse(TEXT("Once reported, the record is deleted"), IFileManager::Get().FileExists(*Record));
	TestFalse(TEXT("So is the marker"), IFileManager::Get().FileExists(*Marker));
	TestFalse(TEXT("And the old queue folder"), IFileManager::Get().DirectoryExists(*FPaths::Combine(Root.Path, FFlockAnalyticsLaunches::LogEventsQueueName)));
	TestFalse(TEXT("And the lock that guarded them"),
		IFileManager::Get().FileExists(*FPaths::Combine(Root.Path, FFlockAnalyticsLaunches::EarlierBuildFilesLockName)));
	TestTrue(TEXT("The consent decision belongs to the install and stays"), IFileManager::Get().FileExists(*Consent));

	const TSharedRef<FFlockAnalyticsLaunches> Third = FFlockAnalyticsLaunches::Start(Root.Path);
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

	const FString LaunchesFolder = FPaths::Combine(FFlockAnalyticsLaunches::DefaultFolder(), FFlockAnalyticsLaunches::LaunchesFolderName);
	int32 HeldFolders = 0;
	int32 HeldFoldersHoldingTheQueues = 0;
	for (const FString& Folder : FFlockLaunchFolder::FindFolders(LaunchesFolder))
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
	TSharedPtr<FFlockLaunchFolder> Released = FFlockLaunchFolder::Create(LaunchesFolder);
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

#endif // WITH_AUTOMATION_TESTS
