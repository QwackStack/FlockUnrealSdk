// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/FlockTemporaryFiles.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/Support/FlockTemporaryFilesTestSupport.h"

namespace
{
	/** A folder of its own for one test, removed when the test ends. */
	struct FTemporaryFilesTestFolder
	{
		FString Path = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
			FString::Printf(TEXT("temporary_files_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

		~FTemporaryFilesTestFolder()
		{
			IFileManager::Get().DeleteDirectory(*Path, /*bRequireExists*/ false, /*bTree*/ true);
		}

		/** Every .tmp file anywhere inside the folder. */
		TArray<FString> TemporaryFiles() const
		{
			TArray<FString> Found;
			IFileManager::Get().FindFilesRecursive(Found, *Path, TEXT("*.tmp"), /*bFiles*/ true, /*bDirectories*/ false);
			return Found;
		}
	};

	/** Saves a half-written entry at Path, its time moved MinutesOld back. */
	bool SaveTemporaryFilesTestFile(const FString& Path, double MinutesOld)
	{
		if (!FFileHelper::SaveStringToFile(TEXT("{\"half\":"), *Path))
		{
			return false;
		}
		if (MinutesOld > 0.0)
		{
			FlockMoveTestFileTimeBack(Path, MinutesOld);
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockTemporaryFilesEachWriteHasItsOwnNameTest, "Flock.TemporaryFiles.EachWriteHasItsOwnName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockTemporaryFilesEachWriteHasItsOwnNameTest::RunTest(const FString& Parameters)
{
	const FString Destination = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Flock"), TEXT("entry.json"));
	const FString First = FFlockTemporaryFiles::MakePath(Destination);
	const FString Second = FFlockTemporaryFiles::MakePath(Destination);

	TestFalse(TEXT("Two writes of one file use two temporary files"), First.Equals(Second, ESearchCase::IgnoreCase));
	TestTrue(TEXT("A temporary file sits beside its destination"), First.StartsWith(Destination + TEXT("."), ESearchCase::CaseSensitive));
	TestTrue(TEXT("And ends in .tmp"), First.EndsWith(TEXT(".tmp"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("With 32 hex digits between"), First.Len(), Destination.Len() + 1 + 32 + 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockTemporaryFilesDeletesOnlyLeftOverFilesTest, "Flock.TemporaryFiles.DeletesOnlyLeftOverFiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockTemporaryFilesDeletesOnlyLeftOverFilesTest::RunTest(const FString& Parameters)
{
	const FTemporaryFilesTestFolder Folder;
	const FString LeftOver = FPaths::Combine(Folder.Path, TEXT("a.json.0123456789abcdef0123456789abcdef.tmp"));
	const FString FixedName = FPaths::Combine(Folder.Path, TEXT("b.json.tmp"));
	const FString AboutToBeMoved = FPaths::Combine(Folder.Path, TEXT("c.json.fedcba9876543210fedcba9876543210.tmp"));
	const FString HalfAMinuteOld = FPaths::Combine(Folder.Path, TEXT("d.json.00000000000000000000000000000000.tmp"));
	const FString NotTemporary = FPaths::Combine(Folder.Path, TEXT("e.json"));
	const FString InASubfolder = FPaths::Combine(Folder.Path, TEXT("Sub"), TEXT("f.json.0123456789abcdef0123456789abcdef.tmp"));

	TestTrue(TEXT("Precondition: a left-over file is saved"), SaveTemporaryFilesTestFile(LeftOver, 10.0));
	TestTrue(TEXT("Precondition: a left-over file in the fixed-name form is saved"), SaveTemporaryFilesTestFile(FixedName, 10.0));
	TestTrue(TEXT("Precondition: a fresh file is saved"), SaveTemporaryFilesTestFile(AboutToBeMoved, 0.0));
	TestTrue(TEXT("Precondition: a file half a minute old is saved"), SaveTemporaryFilesTestFile(HalfAMinuteOld, 0.5));
	TestTrue(TEXT("Precondition: an old file that is not temporary is saved"), SaveTemporaryFilesTestFile(NotTemporary, 10.0));
	TestTrue(TEXT("Precondition: a left-over file in a subfolder is saved"), SaveTemporaryFilesTestFile(InASubfolder, 10.0));

	TestEqual(TEXT("Two left-over files are deleted from the folder itself"),
		FFlockTemporaryFiles::DeleteLeftOverFiles(Folder.Path, TEXT("*.tmp"), /*bIncludeSubfolders*/ false), 2);
	TestFalse(TEXT("A temporary file ten minutes old is deleted"), IFileManager::Get().FileExists(*LeftOver));
	TestFalse(TEXT("So is one in the fixed-name form earlier builds wrote"), IFileManager::Get().FileExists(*FixedName));
	TestTrue(TEXT("A fresh one is left for the process that may be about to move it"), IFileManager::Get().FileExists(*AboutToBeMoved));
	TestTrue(TEXT("So is one half a minute old"), IFileManager::Get().FileExists(*HalfAMinuteOld));
	TestTrue(TEXT("A file that does not match the pattern is left alone"), IFileManager::Get().FileExists(*NotTemporary));
	TestTrue(TEXT("A subfolder is left alone unless asked for"), IFileManager::Get().FileExists(*InASubfolder));

	TestEqual(TEXT("Subfolders are included when asked for"),
		FFlockTemporaryFiles::DeleteLeftOverFiles(Folder.Path, TEXT("*.tmp"), /*bIncludeSubfolders*/ true), 1);
	TestFalse(TEXT("The subfolder's left-over file is deleted"), IFileManager::Get().FileExists(*InASubfolder));
	TestTrue(TEXT("And the fresh one is still kept"), IFileManager::Get().FileExists(*AboutToBeMoved));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockTemporaryFilesSavesThenMovesTest, "Flock.TemporaryFiles.SavesThenMoves",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockTemporaryFilesSavesThenMovesTest::RunTest(const FString& Parameters)
{
	const FTemporaryFilesTestFolder Folder;
	const FString Destination = FPaths::Combine(Folder.Path, TEXT("entry.json"));

	int32 TimesRun = 0;
	FString TemporaryPathSeen;
	bool bTextWasWrittenBeforeTheMove = false;
	ON_SCOPE_EXIT { FFlockTemporaryFiles::SetBeforeNextMoveForTesting(nullptr); };
	FFlockTemporaryFiles::SetBeforeNextMoveForTesting([&TimesRun, &TemporaryPathSeen, &bTextWasWrittenBeforeTheMove](const FString& TemporaryPath)
	{
		++TimesRun;
		TemporaryPathSeen = TemporaryPath;
		bTextWasWrittenBeforeTheMove = IFileManager::Get().FileExists(*TemporaryPath);
	});

	TestTrue(TEXT("The first save lands"), FFlockTemporaryFiles::SaveThenMove(TEXT("{\"n\":1}"), Destination));
	TestTrue(TEXT("The second save lands"), FFlockTemporaryFiles::SaveThenMove(TEXT("{\"n\":2}"), Destination));

	FString Contents;
	FFileHelper::LoadFileToString(Contents, *Destination);
	TestEqual(TEXT("The destination holds the last save"), Contents, FString(TEXT("{\"n\":2}")));
	TestEqual(TEXT("The test hook runs once"), TimesRun, 1);
	TestTrue(TEXT("It is handed the temporary file beside the destination"),
		TemporaryPathSeen.StartsWith(Destination + TEXT("."), ESearchCase::CaseSensitive));
	TestTrue(TEXT("Once the text is written and before it is moved"), bTextWasWrittenBeforeTheMove);
	TestEqual(TEXT("No temporary file is left"), Folder.TemporaryFiles().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockTemporaryFilesFailedMoveGivesUpAtOnceTest, "Flock.TemporaryFiles.AFailedMoveGivesUpAtOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockTemporaryFilesFailedMoveGivesUpAtOnceTest::RunTest(const FString& Parameters)
{
	const FTemporaryFilesTestFolder Folder;
	const FString Destination = FPaths::Combine(Folder.Path, TEXT("entry.json"));
	// A folder stands where the file goes, so a save can be written beside it but never moved into place.
	IFileManager::Get().MakeDirectory(*Destination, /*bTree*/ true);

	bool bSaved = true;
	int32 Complaints = 0;
	const double StartedAt = FPlatformTime::Seconds();
	{
		FFlockFileManagerComplaintCounter Counter;
		bSaved = FFlockTemporaryFiles::SaveThenMove(TEXT("{}"), Destination);
		Complaints = Counter.Count();
	}
	const double Seconds = FPlatformTime::Seconds() - StartedAt;

	TestFalse(TEXT("A save that cannot be moved into place says so"), bSaved);
	TestTrue(FString::Printf(TEXT("It gives up at once instead of retrying on the calling thread (took %.2f s)"), Seconds), Seconds < FlockTestSecondsWithoutARetry);
	TestEqual(TEXT("The file manager logs no warning or error"), Complaints, 0);
	TestEqual(TEXT("The temporary file written beside it is deleted"), Folder.TemporaryFiles().Num(), 0);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
