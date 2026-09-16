// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/FlockLaunchFolder.h"
#include "Misc/Paths.h"

namespace
{
	/** A parent folder of its own for one test, removed when the test ends. */
	struct FLaunchFolderTestRoot
	{
		FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
			FString::Printf(TEXT("launch_folders_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));

		~FLaunchFolderTestRoot()
		{
			IFileManager::Get().DeleteDirectory(*Path, /*RequireExists*/ false, /*Tree*/ true);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLaunchFolderClaimedOnlyOnceItsOwnerLetsGoTest, "Flock.Misc.LaunchFolder.ClaimedOnlyOnceItsOwnerLetsGo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLaunchFolderClaimedOnlyOnceItsOwnerLetsGoTest::RunTest(const FString& Parameters)
{
	const FLaunchFolderTestRoot Root;
	TSharedPtr<FFlockLaunchFolder> Owner = FFlockLaunchFolder::Create(Root.Path);
	TestTrue(TEXT("Precondition: a launch makes a folder of its own"), Owner.IsValid());
	if (!Owner.IsValid())
	{
		return true;
	}
	const FString Folder = Owner->GetPath();
	TestTrue(TEXT("Under the parent folder"), Folder.StartsWith(Root.Path));
	TestTrue(TEXT("With a lock file"), IFileManager::Get().FileExists(*FPaths::Combine(Folder, FFlockLaunchFolder::LockFileName)));

	TestFalse(TEXT("A folder whose owner still runs is never claimed, from the owner's own process either"),
		FFlockLaunchFolder::ClaimEnded(Folder).IsValid());

	// The owner ends however it ends; the operating system lets go of its lock.
	Owner.Reset();
	TSharedPtr<FFlockLaunchFolder> Claim = FFlockLaunchFolder::ClaimEnded(Folder);
	TestTrue(TEXT("Once the owner lets go, the folder is claimed"), Claim.IsValid());
	TestFalse(TEXT("By one launch at a time"), FFlockLaunchFolder::ClaimEnded(Folder).IsValid());
	Claim.Reset();
	TestTrue(TEXT("A claim that lets go leaves the folder for a later launch"), FFlockLaunchFolder::ClaimEnded(Folder).IsValid());

	const FString WithoutLock = FPaths::Combine(Root.Path, TEXT("not-a-launch"));
	IFileManager::Get().MakeDirectory(*WithoutLock, /*Tree*/ true);
	TestFalse(TEXT("A folder with no lock is never claimed"), FFlockLaunchFolder::ClaimEnded(WithoutLock).IsValid());
	TestEqual(TEXT("Both folders are found"), FFlockLaunchFolder::FindFolders(Root.Path).Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLaunchFolderDeletesEverythingItsLockLastTest, "Flock.Misc.LaunchFolder.DeletesEverythingItsLockLast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockLaunchFolderDeletesEverythingItsLockLastTest::RunTest(const FString& Parameters)
{
	const FLaunchFolderTestRoot Root;
	TSharedPtr<FFlockLaunchFolder> Owner = FFlockLaunchFolder::Create(Root.Path);
	TestTrue(TEXT("Precondition: a launch makes a folder of its own"), Owner.IsValid());
	if (!Owner.IsValid())
	{
		return true;
	}
	const FString Folder = Owner->GetPath();
	const FString LockPath = FPaths::Combine(Folder, FFlockLaunchFolder::LockFileName);
	const FString QueuedEntry = FPaths::Combine(Folder, TEXT("log_events"), TEXT("0001.json"));
	TestTrue(TEXT("Precondition: a record is saved"), FFileHelper::SaveStringToFile(TEXT("{}"), *FPaths::Combine(Folder, TEXT("session_state.json"))));
	TestTrue(TEXT("Precondition: an entry is queued"), FFileHelper::SaveStringToFile(TEXT("{}"), *QueuedEntry));
	Owner.Reset();

	{
		// Another program holds the queued entry open, so it cannot be deleted.
		const TUniquePtr<IFileHandle> Held(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*QueuedEntry, /*bAppend*/ true, /*bAllowRead*/ false));
		TestTrue(TEXT("Precondition: the entry is held open"), Held.IsValid());
		const TSharedPtr<FFlockLaunchFolder> Claim = FFlockLaunchFolder::ClaimEnded(Folder);
		TestTrue(TEXT("Precondition: the folder is claimed"), Claim.IsValid());
		if (Claim.IsValid())
		{
			TestFalse(TEXT("A folder with a file that cannot be deleted is not reported gone"), Claim->DeleteEverything());
		}
		TestTrue(TEXT("Its lock stays, so a later launch finds the folder again"), IFileManager::Get().FileExists(*LockPath));
		TestTrue(TEXT("And can claim it"), FFlockLaunchFolder::ClaimEnded(Folder).IsValid());
	}

	const TSharedPtr<FFlockLaunchFolder> Claim = FFlockLaunchFolder::ClaimEnded(Folder);
	TestTrue(TEXT("Precondition: the folder is claimed again"), Claim.IsValid());
	if (Claim.IsValid())
	{
		TestTrue(TEXT("Once nothing holds a file, everything goes"), Claim->DeleteEverything());
	}
	TestFalse(TEXT("The folder is gone"), IFileManager::Get().DirectoryExists(*Folder));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
