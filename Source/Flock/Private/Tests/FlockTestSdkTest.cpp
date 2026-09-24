// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Misc/FlockEngineCompat.h"

#include "Analytics/FlockAnalyticsLaunches.h"
#include "Analytics/FlockConsentStore.h"
#include "Auth/FlockFileTokenStore.h"
#include "Config/FlockConfig.h"
#include "FlockInitConfig.h"
#include "FlockLogger.h"
#include "FlockSubsystem.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/FlockLaunchFolder.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Models/FlockGameModels.h"
#include "Providers/FlockAssetProvider.h"
#include "Providers/FlockGameProvider.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockTestSdk.h"

namespace FlockTestSdkTesting
{
	FFlockInitConfig MakeValidConfig()
	{
		FFlockInitConfig Config;
		Config.ApiUrl = TEXT("https://api-flock.qwacks.com");
		Config.ApiKey = TEXT("secret");
		Config.GameId = TEXT("my-game");
		Config.GameVersion = TEXT("1.2.3");
		Config.GameVersionId = TEXT("ver-abc");
		return Config;
	}

	/** The launch folders in the project's own analytics folder, one per line in name order, to compare before and after. */
	FString ProjectLaunchFolders()
	{
		TArray<FString> Folders = FFlockLaunchFolder::FindFolders(
			FPaths::Combine(FFlockAnalyticsLaunches::DefaultFolder(), FFlockAnalyticsLaunches::LaunchesFolderName));
		Folders.Sort();
		return FString::Join(Folders, TEXT("\n"));
	}
}

/**
 * A test's Flock SDK saves nothing in the project and takes nothing from it: its launch lands in its own folder, and the
 * project's launch folders, consent decision and sign-in are exactly as they were. A launch folder a test left in the
 * project was taken over by the next real launch, which sent that test's fake player to the real server.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockTestSdkKeepsItsFilesTest, "Flock.Runtime.TestSdk.KeepsItsFilesOutOfTheProject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockTestSdkKeepsItsFilesTest::RunTest(const FString& Parameters)
{
	// Analytics and the offline cache on, and a consent change that is saved, whatever the project says.
	UFlockConfig* Settings = GetMutableDefault<UFlockConfig>();
	const bool bAnalyticsWas = Settings->bAnalyticsEnabled;
	const bool bExplicitConsentWas = Settings->bAnalyticsRequireExplicitConsent;
	const bool bOfflineCacheWas = Settings->bEnableOfflineCache;
	Settings->bAnalyticsEnabled = true;
	Settings->bAnalyticsRequireExplicitConsent = false;
	Settings->bEnableOfflineCache = true;
	ON_SCOPE_EXIT
	{
		Settings->bAnalyticsEnabled = bAnalyticsWas;
		Settings->bAnalyticsRequireExplicitConsent = bExplicitConsentWas;
		Settings->bEnableOfflineCache = bOfflineCacheWas;
	};

	const FString ProjectLaunchesBefore = FlockTestSdkTesting::ProjectLaunchFolders();
	// A missing file reads as the minimum time, so a file made or deleted shows up as a change too.
	const FDateTime ProjectConsentBefore = IFileManager::Get().GetTimeStamp(*FFlockConsentStore::DefaultPath());
	const FDateTime ProjectSignInBefore = IFileManager::Get().GetTimeStamp(*FFlockFileTokenStore::DefaultPath());

	FString TestFolder;
	{
		const FFlockTestSdk Test;
		TestFolder = Test.Folder;
		Test->SetHttpAdapterForTesting(MakeShared<FFlockFakeTransport>());

		// Planted where this SDK's own offline cache and sign-in are read at start-up. Each is gone afterwards only if the
		// SDK really looked there: another version's cache is pruned, and another game's sign-in cannot be read and is removed.
		const FString OtherVersionCache = FPaths::Combine(Test.Folder, TEXT("snapshots"), TEXT("ver-other"), TEXT("config"), TEXT("entry.json"));
		const FString OtherGameSignIn = FPaths::Combine(Test.Folder, TEXT("auth.dat"));
		TestTrue(TEXT("Precondition: another version's offline cache is planted"), FFileHelper::SaveStringToFile(TEXT("{}"), *OtherVersionCache));
		TestTrue(TEXT("Precondition: another game's sign-in is planted"), FFileHelper::SaveStringToFile(TEXT("not this game's sign-in"), *OtherGameSignIn));

		Test->InitializeWithConfig(FlockTestSdkTesting::MakeValidConfig());
		TestTrue(TEXT("Precondition: initialized with analytics on"), Test->IsInitialized() && Test->GetAnalyticsProvider() != nullptr);

		const TArray<FString> OwnLaunches = FFlockLaunchFolder::FindFolders(
			FPaths::Combine(Test.Folder, TEXT("analytics"), FFlockAnalyticsLaunches::LaunchesFolderName));
		TestEqual(TEXT("This launch's analytics folder is in the test's own folder"), OwnLaunches.Num(), 1);
		TestFalse(TEXT("And held while the SDK runs"), OwnLaunches.Num() == 1 && FFlockLaunchFolder::ClaimEnded(OwnLaunches[0]).IsValid());
		TestEqualSensitive(TEXT("Not one launch folder is added to the project's, or taken from it"), FlockTestSdkTesting::ProjectLaunchFolders(), ProjectLaunchesBefore);

		// Revoking is a change, so it is saved; granting would not be, since consent is not asked for here.
		Test->SetAnalyticsConsent(false);
		TestTrue(TEXT("The consent decision is saved in the test's folder"),
			IFileManager::Get().FileExists(*FPaths::Combine(Test.Folder, TEXT("analytics"), TEXT("consent.json"))));
		TestTrue(TEXT("And the project's is left alone"),
			IFileManager::Get().GetTimeStamp(*FFlockConsentStore::DefaultPath()) == ProjectConsentBefore);

		TestFalse(TEXT("The offline cache is the test's own"), IFileManager::Get().FileExists(*OtherVersionCache));
		TestFalse(TEXT("The sign-in is the test's own"), IFileManager::Get().FileExists(*OtherGameSignIn));
		TestTrue(TEXT("And the project's sign-in is left alone"),
			IFileManager::Get().GetTimeStamp(*FFlockFileTokenStore::DefaultPath()) == ProjectSignInBefore);

		const FFlockAssetProvider* Assets = Test->GetAssetProvider();
		TestTrue(TEXT("The asset cache is in the test's folder"), Assets != nullptr
			&& FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Assets->GetCacheDirectory()), Test.Folder));
	}
	TestFalse(TEXT("The test's folder is deleted when it ends"), IFileManager::Get().DirectoryExists(*TestFolder));
	TestEqualSensitive(TEXT("And the project's launch folders are as they were"), FlockTestSdkTesting::ProjectLaunchFolders(), ProjectLaunchesBefore);
	return true;
}

/**
 * A test's Flock SDK reaches no server unless the test hands in a transport of its own: the core tests configure the
 * production address, and a request sent from one would reach it with a fake key.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockTestSdkReachesNoServerTest, "Flock.Runtime.TestSdk.ReachesNoServer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockTestSdkReachesNoServerTest::RunTest(const FString& Parameters)
{
	// No retries, so the one request is answered before this returns and leaves nothing waiting on a ticker.
	UFlockConfig* Settings = GetMutableDefault<UFlockConfig>();
	const int32 RetriesWere = Settings->RetryMaxRetries;
	Settings->RetryMaxRetries = 0;
	ON_SCOPE_EXIT
	{
		Settings->RetryMaxRetries = RetriesWere;
	};

	const FFlockTestSdk Test;
	// The failure is the point, so it is not logged as an error the automation framework would count.
	Test->SetLogger(MakeShared<FFlockNullLogger>());
	FFlockInitConfig Config = FlockTestSdkTesting::MakeValidConfig();
	// An address nothing listens on, so a build that lost the stand-in still sends nothing anywhere.
	Config.ApiUrl = TEXT("http://127.0.0.1:9");
	Test->InitializeWithConfig(Config);
	FFlockGameProvider* Games = Test->GetGameProvider();
	TestTrue(TEXT("Precondition: initialized"), Test->IsInitialized() && Games != nullptr);
	if (Games == nullptr)
	{
		return true;
	}

	bool bAnswered = false;
	bool bSucceeded = false;
	Games->GetGame([&bAnswered, &bSucceeded](TFlockResult<FFlockGameSchema> Result)
	{
		bAnswered = true;
		bSucceeded = Result.bSuccess;
	});
	TestEqual(TEXT("The request went to the stand-in"), Test.Unreachable->Requests, 1);
	TestTrue(TEXT("And was answered as unreachable"), bAnswered && !bSucceeded);
	return true;
}

/**
 * Every Flock test that builds a Flock SDK goes through FFlockTestSdk, so none can save into the project by forgetting a
 * setting. Read from the test sources, next to this file.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockTestSdkIsTheOnlyWayTest, "Flock.Runtime.TestSdk.EveryTestBuildsTheSdkThroughIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockTestSdkIsTheOnlyWayTest::RunTest(const FString& Parameters)
{
	const FString TestsFolder = FPaths::ConvertRelativePathToFull(FPaths::GetPath(FString(ANSI_TO_TCHAR(__FILE__))));
	const FString EditorTestsFolder = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(TestsFolder, TEXT(".."), TEXT(".."), TEXT(".."), TEXT("FlockEditor"), TEXT("Private"), TEXT("Tests")));
	// Put together here so that this file does not count itself.
	const FString Building = FString(TEXT("NewObject<")) + TEXT("UFlockSubsystem>");

	int32 FilesRead = 0;
	TArray<FString> FilesBuildingOne;
	for (const FString& Folder : { TestsFolder, EditorTestsFolder })
	{
		for (const TCHAR* Pattern : { TEXT("*.cpp"), TEXT("*.h") })
		{
			TArray<FString> Files;
			IFileManager::Get().FindFilesRecursive(Files, *Folder, Pattern, /*Files*/ true, /*Directories*/ false);
			for (const FString& File : Files)
			{
				FString Text;
				if (FFileHelper::LoadFileToString(Text, *File))
				{
					++FilesRead;
					if (Text.Contains(Building, ESearchCase::CaseSensitive))
					{
						FilesBuildingOne.Add(FPaths::GetCleanFilename(File));
					}
				}
			}
		}
	}
	FilesBuildingOne.Sort();

	TestTrue(TEXT("Precondition: the test sources of both modules were read"), FilesRead > 60);
	TestEqualSensitive(TEXT("Only FFlockTestSdk builds a Flock SDK"), FString::Join(FilesBuildingOne, TEXT(", ")), FString(TEXT("FlockTestSdk.h")));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
