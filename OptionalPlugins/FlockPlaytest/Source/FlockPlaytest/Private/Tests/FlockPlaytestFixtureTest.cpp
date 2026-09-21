// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Analytics/FlockAnalyticsLaunches.h"
#include "FlockPlaytestFormSpool.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/FlockLaunchFolder.h"
#include "Misc/Paths.h"
#include "Tests/FlockPlaytestSubsystemTestSupport.h"
#include "Tests/FlockPlaytestTestSupport.h"

using namespace FlockPlaytestSubsystemTesting;
using namespace FlockPlaytestFixtures;

namespace FlockPlaytestFixtureTesting
{
	/** The launch folders in the project's own Flock analytics folder, one per line in name order. */
	FString ProjectLaunchFolders()
	{
		TArray<FString> Folders = FFlockLaunchFolder::FindFolders(
			FPaths::Combine(FFlockAnalyticsLaunches::DefaultFolder(), FFlockAnalyticsLaunches::LaunchesFolderName));
		Folders.Sort();
		return FString::Join(Folders, TEXT("\n"));
	}

	/** Every file under Folder, one per line in name order. */
	FString FilesUnder(const FString& Folder)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Folder, TEXT("*"), /*Files*/ true, /*Directories*/ false);
		Files.Sort();
		return FString::Join(Files, TEXT("\n"));
	}
}

/**
 * The fixture's Flock SDK and playtest keep their files in the fixture's own folder: its Flock launch lands there, and so
 * does a feedback form it could not send. A run killed mid-test left a signed-in fixture's launch in the project, and the
 * next real launch sent that fake player's session to the real server; a form left in the project would be sent to
 * Protokite the same way.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestFixtureKeepsItsFilesTest, "Flock.Playtest.Fixture.KeepsItsFilesOutOfTheProject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestFixtureKeepsItsFilesTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Settings(true, UsableUrl);
	// Events are kept until sent, as a game keeps them: that queue is one of the files that must stay in the fixture.
	FScopedFlockAnalyticsSettings Analytics(/*bAnalyticsEnabled*/ true, /*bCaptureExceptions*/ true, /*bKeepEventsUntilSent*/ true);

	const FString ProjectLaunchesBefore = FlockPlaytestFixtureTesting::ProjectLaunchFolders();
	const FString ProjectFormsBefore = FlockPlaytestFixtureTesting::FilesUnder(FFlockPlaytestFormSpool::GetDefaultFolder());

	FString FixtureFolder;
	{
		FPlaytestFixture Fixture;
		FixtureFolder = Fixture.Folder;
		// Signed in with a Flock session running, as the fixture was in the run that leaked its player.
		Fixture.SignInToFlockOnStart();
		// A server having a moment: the form is kept for a later launch.
		Fixture.Transport->Answer(TEXT("/game/sdk/feedback-form"), FFlockPlaytestFakeTransport::Status(500, TEXT("{}")));
		Fixture.StartFlock();
		TestTrue(TEXT("Precondition: a Flock session is running"), Fixture.Flock->HasActiveAnalyticsSession());

		const TArray<FString> OwnLaunches = FFlockLaunchFolder::FindFolders(
			FPaths::Combine(Fixture.Folder, TEXT("Flock"), TEXT("analytics"), FFlockAnalyticsLaunches::LaunchesFolderName));
		TestEqual(TEXT("The Flock launch is in the fixture's own folder"), OwnLaunches.Num(), 1);
		TestFalse(TEXT("And held while the fixture runs"), OwnLaunches.Num() == 1 && FFlockLaunchFolder::ClaimEnded(OwnLaunches[0]).IsValid());
		TestEqualSensitive(TEXT("Not one launch folder is added to the project's, or taken from it"),
			FlockPlaytestFixtureTesting::ProjectLaunchFolders(), ProjectLaunchesBefore);

		FFlockPlaytestFormAnswers Answers;
		Answers.SetRating(TEXT("rating"), 4);
		Answers.SetChosenOption(TEXT("category"), TEXT("Bug"));
		Answers.SetText(TEXT("steps"), TEXT("Opened the map"));
		TestTrue(TEXT("Precondition: the form is sent or kept"), Fixture.Playtest->SendFilledInForm(Answers));
		TestEqual(TEXT("A form that could not be sent is kept in the fixture's own folder"),
			FFlockPlaytestFormSpool(FPaths::Combine(Fixture.Folder, TEXT("FeedbackForms"))).CountWaiting(), 1);
		TestEqualSensitive(TEXT("Not in the project's"),
			FlockPlaytestFixtureTesting::FilesUnder(FFlockPlaytestFormSpool::GetDefaultFolder()), ProjectFormsBefore);
	}
	TestFalse(TEXT("The fixture's folder is deleted when it ends"), IFileManager::Get().DirectoryExists(*FixtureFolder));
	TestEqualSensitive(TEXT("And the project's launch folders are as they were"),
		FlockPlaytestFixtureTesting::ProjectLaunchFolders(), ProjectLaunchesBefore);
	return true;
}

/**
 * Every playtest test that builds a Flock SDK does it through FPlaytestFixture, so none can save into the project by
 * forgetting a setting. Read from the test sources, next to this file.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestFixtureIsTheOnlyWayTest, "Flock.Playtest.Fixture.EveryTestBuildsTheSdkThroughIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestFixtureIsTheOnlyWayTest::RunTest(const FString& Parameters)
{
	const FString TestsFolder = FPaths::ConvertRelativePathToFull(FPaths::GetPath(FString(ANSI_TO_TCHAR(__FILE__))));
	// Put together here so that this file does not count itself.
	const FString Building = FString(TEXT("NewObject<")) + TEXT("UFlockSubsystem>");

	int32 FilesRead = 0;
	TArray<FString> FilesBuildingOne;
	for (const TCHAR* Pattern : { TEXT("*.cpp"), TEXT("*.h") })
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *TestsFolder, Pattern, /*Files*/ true, /*Directories*/ false);
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
	FilesBuildingOne.Sort();

	TestTrue(TEXT("Precondition: the playtest test sources were read"), FilesRead > 20);
	TestEqualSensitive(TEXT("Only FPlaytestFixture builds a Flock SDK"), FString::Join(FilesBuildingOne, TEXT(", ")),
		FString(TEXT("FlockPlaytestSubsystemTestSupport.h")));
	return true;
}

/**
 * A test that builds a fixture says what the playtest settings are, rather than reading the project's own.
 *
 * **Measured 2026-09-21** by installing the release zips into a project that had never seen them: five tests passed
 * here and failed there, because they wanted a ready playtest and took Enable Playtesting from the harness project's
 * DefaultGame.ini. The shipped default is off, so a studio running this suite in their own project saw failures that
 * were not defects. Every file that builds a fixture now declares its own settings, and this keeps it that way.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestFixtureSettingsAreTheTestsOwnTest,
	"Flock.Playtest.Fixture.EveryTestSaysWhatItsSettingsAre",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestFixtureSettingsAreTheTestsOwnTest::RunTest(const FString& Parameters)
{
	const FString TestsFolder = FPaths::ConvertRelativePathToFull(FPaths::GetPath(FString(ANSI_TO_TCHAR(__FILE__))));
	// Put together here so this file's own mention does not count.
	const FString BuildingAFixture = FString(TEXT("FPlaytestFixture ")) + TEXT("Fixture");
	const FString SayingTheSettings = FString(TEXT("FScopedPlaytest")) + TEXT("Settings");

	int32 FilesRead = 0;
	TArray<FString> FilesTakingTheProjectsSettings;
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *TestsFolder, TEXT("*.cpp"), /*Files*/ true, /*Directories*/ false);
	for (const FString& File : Files)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File))
		{
			continue;
		}
		++FilesRead;
		if (Text.Contains(BuildingAFixture, ESearchCase::CaseSensitive)
			&& !Text.Contains(SayingTheSettings, ESearchCase::CaseSensitive))
		{
			FilesTakingTheProjectsSettings.Add(FPaths::GetCleanFilename(File));
		}
	}
	FilesTakingTheProjectsSettings.Sort();

	TestTrue(TEXT("Precondition: the playtest test sources were read"), FilesRead > 20);
	TestEqualSensitive(TEXT("No test file leaves its playtest settings to the project it runs in"),
		FString::Join(FilesTakingTheProjectsSettings, TEXT(", ")), TEXT(""));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
