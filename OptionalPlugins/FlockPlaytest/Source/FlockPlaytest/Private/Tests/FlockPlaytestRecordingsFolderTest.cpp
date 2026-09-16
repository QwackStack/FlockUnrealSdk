// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "FlockPlaytestRecordingsFolder.h"
#include "FlockPlaytestVideoFile.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/FlockPlaytestVideoTestSupport.h"

using namespace FlockPlaytestVideoTesting;

namespace
{
	/** A folder of the test's own, removed when the test ends, with the recordings folder inside it. */
	struct FRecordingsTestFolder
	{
		FString Folder = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockPlaytestTests"),
			FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		FString Recordings = FPaths::Combine(Folder, TEXT("Recordings"));

		~FRecordingsTestFolder()
		{
			IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists*/ false, /*Tree*/ true);
		}
	};

	void AppendRecordingsTestLittleEndian(TArray<uint8>& Bytes, uint64 Value, int32 ByteCount)
	{
		for (int32 Index = 0; Index < ByteCount; ++Index)
		{
			Bytes.Add(static_cast<uint8>((Value >> (8 * Index)) & 0xFF));
		}
	}

	/** One frame's bytes: the two bits every VP9 frame starts with, then bytes different for each frame and never zero. */
	TArray<uint8> MakeRecordingsTestFrame(int32 FrameIndex, int32 FrameBytes)
	{
		TArray<uint8> Bytes;
		for (int32 Byte = 0; Byte < FrameBytes; ++Byte)
		{
			Bytes.Add(Byte == 0 ? static_cast<uint8>(0x82) : static_cast<uint8>(((FrameIndex * 31 + Byte) & 0xFF) | 1));
		}
		return Bytes;
	}

	void AppendRecordingsTestFrameHeader(TArray<uint8>& Bytes, int32 FrameIndex, int32 FrameBytes)
	{
		AppendRecordingsTestLittleEndian(Bytes, static_cast<uint64>(FrameBytes), 4);
		AppendRecordingsTestLittleEndian(Bytes, static_cast<uint64>(FrameIndex * 33), 8);
	}

	/** A video file laid out the way the recording writes one: its header, counting FrameCountInHeader, then Frames frames. */
	TArray<uint8> MakeRecordingsTestVideoBytes(int32 Frames, int32 FrameBytes, int32 FrameCountInHeader)
	{
		TArray<uint8> Bytes;
		Bytes.Append({ 'D', 'K', 'I', 'F' });
		AppendRecordingsTestLittleEndian(Bytes, 0, 2);
		AppendRecordingsTestLittleEndian(Bytes, 32, 2);
		Bytes.Append({ 'V', 'P', '9', '0' });
		AppendRecordingsTestLittleEndian(Bytes, 64, 2);
		AppendRecordingsTestLittleEndian(Bytes, 36, 2);
		AppendRecordingsTestLittleEndian(Bytes, 1000, 4);
		AppendRecordingsTestLittleEndian(Bytes, 1, 4);
		AppendRecordingsTestLittleEndian(Bytes, static_cast<uint64>(FrameCountInHeader), 4);
		AppendRecordingsTestLittleEndian(Bytes, 0, 4);
		for (int32 Index = 0; Index < Frames; ++Index)
		{
			AppendRecordingsTestFrameHeader(Bytes, Index, FrameBytes);
			Bytes.Append(MakeRecordingsTestFrame(Index, FrameBytes));
		}
		return Bytes;
	}

	bool SaveRecordingsTestFile(const FString& Path, const TArray<uint8>& Bytes)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
		return FFileHelper::SaveArrayToFile(Bytes, *Path);
	}

	int64 RecordingsTestBytesIn(const FString& Folder)
	{
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *FPaths::Combine(Folder, TEXT("*")), /*Files*/ true, /*Directories*/ false);
		int64 Bytes = 0;
		for (const FString& Name : Names)
		{
			Bytes += IFileManager::Get().FileSize(*FPaths::Combine(Folder, Name));
		}
		return Bytes;
	}

	FFlockPlaytestRecordingSession MakeRecordingsTestSession(const FString& SessionId)
	{
		FFlockPlaytestRecordingSession Session;
		Session.ProtokiteSessionId = SessionId;
		Session.ProtokiteApiUrl = TEXT("http://127.0.0.1:8020");
		Session.FlockGameVersionId = TEXT("pt-recordings-test-version");
		return Session;
	}

	/**
	 * A run whose launch has ended, left the way it leaves one: its folder made and locked, given the videos and session
	 * handed in, then let go of. Returns the run's folder, or empty when it could not be made.
	 */
	FString MakeEndedRecordingRun(FAutomationTestBase& Test, const FString& Recordings, EFlockPlaytestRecordingKind Kind, const FString& RunName,
		const TArray<uint8>* FinishedVideo, const TArray<uint8>* UnfinishedVideo, const FFlockPlaytestRecordingSession* Session)
	{
		FString Error;
		const TSharedPtr<FFlockPlaytestRecordingRun> Run = FFlockPlaytestRecordingRun::CreateNamedForTesting(Recordings, Kind, RunName,
			/*ReservedBytes*/ 0, Error);
		if (!Test.TestTrue(FString::Printf(TEXT("Precondition: run %s is made (%s)"), *RunName, *Error), Run.IsValid()))
		{
			return FString();
		}
		if (FinishedVideo != nullptr)
		{
			Test.TestTrue(TEXT("Precondition: its video is saved"), SaveRecordingsTestFile(Run->GetVideoFilePath(), *FinishedVideo));
		}
		if (UnfinishedVideo != nullptr)
		{
			Test.TestTrue(TEXT("Precondition: its unfinished video is saved"), SaveRecordingsTestFile(Run->GetUnfinishedVideoFilePath(), *UnfinishedVideo));
		}
		if (Session != nullptr)
		{
			Test.TestTrue(TEXT("Precondition: its session is saved"), Run->SaveSession(*Session, Error));
		}
		return Run->GetFolder();
	}

	bool RecordingsTestFolderExists(const FString& Folder)
	{
		return !Folder.IsEmpty() && IFileManager::Get().DirectoryExists(*Folder);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestRecordingsRunHasAFolderOfItsOwnTest,
	"Flock.Playtest.Recordings.ARunHasAFolderOfItsOwn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestRecordingsRunHasAFolderOfItsOwnTest::RunTest(const FString& Parameters)
{
	FRecordingsTestFolder Test;
	FString Error;
	TSharedPtr<FFlockPlaytestRecordingRun> First = FFlockPlaytestRecordingRun::Create(Test.Recordings, EFlockPlaytestRecordingKind::Playtest,
		/*ReservedBytes*/ 12345, Error);
	const TSharedPtr<FFlockPlaytestRecordingRun> Second = FFlockPlaytestRecordingRun::Create(Test.Recordings, EFlockPlaytestRecordingKind::Playtest,
		/*ReservedBytes*/ 0, Error);
	const TSharedPtr<FFlockPlaytestRecordingRun> TestVideoRun = FFlockPlaytestRecordingRun::Create(Test.Recordings, EFlockPlaytestRecordingKind::TestVideo,
		/*ReservedBytes*/ 0, Error);
	if (!TestTrue(FString::Printf(TEXT("Precondition: the runs are made (%s)"), *Error), First.IsValid() && Second.IsValid() && TestVideoRun.IsValid()))
	{
		return true;
	}

	TestFalse(TEXT("Each run has a folder of its own"), FPaths::IsSamePath(First->GetFolder(), Second->GetFolder()));
	TestTrue(TEXT("A playtest recording's run is under Playtest"),
		FPaths::IsSamePath(FPaths::GetPath(First->GetFolder()), FFlockPlaytestRecordingsFolder::GetKindFolder(Test.Recordings, EFlockPlaytestRecordingKind::Playtest)));
	TestTrue(TEXT("A test video's run is under TestVideos"),
		FPaths::IsSamePath(FPaths::GetPath(TestVideoRun->GetFolder()), FFlockPlaytestRecordingsFolder::GetKindFolder(Test.Recordings, EFlockPlaytestRecordingKind::TestVideo)));

	const FString Name = First->GetName();
	TestTrue(FString::Printf(TEXT("Named by the UTC time it was made, then eight hex digits (%s)"), *Name),
		Name.Len() == 24 && Name[8] == TEXT('-') && Name[15] == TEXT('-') && Name.Left(8).IsNumeric() && Name.Mid(9, 6).IsNumeric());
	TestEqual(TEXT("A playtest recording's file"), FPaths::GetCleanFilename(First->GetVideoFilePath()), FString::Printf(TEXT("recording-%s.ivf"), *Name));
	TestEqual(TEXT("A test video's file"), FPaths::GetCleanFilename(TestVideoRun->GetVideoFilePath()),
		FString::Printf(TEXT("test-recording-%s.ivf"), *TestVideoRun->GetName()));
	TestEqual(TEXT("Unfinished, it has .part added"), First->GetUnfinishedVideoFilePath(), First->GetVideoFilePath() + TEXT(".part"));
	FString Reserved;
	FFileHelper::LoadFileToString(Reserved, *FPaths::Combine(First->GetFolder(), FFlockPlaytestRecordingsFolder::ReservedBytesFileName));
	TestEqual(TEXT("How large its video may grow is saved in its folder"), Reserved, FString(TEXT("12345")));

	const FString FirstFolder = First->GetFolder();
	TestFalse(TEXT("A run still going cannot be claimed"), FFlockPlaytestRecordingRun::ClaimEnded(FirstFolder).IsValid());

	const FFlockPlaytestRecordingSession Session = MakeRecordingsTestSession(TEXT("01KX0RECORDINGSESSION00001"));
	TestTrue(TEXT("Its session is saved"), First->SaveSession(Session, Error));
	const FFlockPlaytestRecordingSession Loaded = First->LoadSession();
	TestEqual(TEXT("The session id reads back"), Loaded.ProtokiteSessionId, Session.ProtokiteSessionId);
	TestEqual(TEXT("The address reads back"), Loaded.ProtokiteApiUrl, Session.ProtokiteApiUrl);
	TestEqual(TEXT("The Game Version ID reads back"), Loaded.FlockGameVersionId, Session.FlockGameVersionId);
	FString Text;
	FFileHelper::LoadFileToString(Text, *FPaths::Combine(FirstFolder, FFlockPlaytestRecordingsFolder::SessionFileName));
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	TestTrue(TEXT("Saved as JSON under plain names"), FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid()
		&& Object->HasField(TEXT("protokite_session_id")) && Object->HasField(TEXT("protokite_api_url")) && Object->HasField(TEXT("flock_game_version_id")));

	First.Reset();
	const TSharedPtr<FFlockPlaytestRecordingRun> Claimed = FFlockPlaytestRecordingRun::ClaimEnded(FirstFolder);
	if (TestTrue(TEXT("Once its launch lets go, it can be claimed"), Claimed.IsValid()))
	{
		TestTrue(TEXT("As a playtest recording's run"), Claimed->GetKind() == EFlockPlaytestRecordingKind::Playtest);
		TestEqual(TEXT("Under its own name"), Claimed->GetName(), Name);
		TestEqual(TEXT("With its session"), Claimed->LoadSession().ProtokiteSessionId, Session.ProtokiteSessionId);
	}
	TestFalse(TEXT("By one launch at a time"), FFlockPlaytestRecordingRun::ClaimEnded(FirstFolder).IsValid());

	const FString Elsewhere = FPaths::Combine(Test.Recordings, TEXT("Elsewhere"), TEXT("20200101-000000-aaaaaaaa"));
	TestTrue(TEXT("Precondition: a lock is saved outside Playtest and TestVideos"),
		SaveRecordingsTestFile(FPaths::Combine(Elsewhere, FFlockPlaytestRecordingsFolder::LockFileName), TArray<uint8>()));
	TestFalse(TEXT("A folder outside Playtest and TestVideos is no run, lock or not"), FFlockPlaytestRecordingRun::ClaimEnded(Elsewhere).IsValid());
	const FString NoLock = FPaths::Combine(FFlockPlaytestRecordingsFolder::GetKindFolder(Test.Recordings, EFlockPlaytestRecordingKind::Playtest), TEXT("no-lock"));
	IFileManager::Get().MakeDirectory(*NoLock, /*Tree*/ true);
	TestFalse(TEXT("Nor is a folder with no lock"), FFlockPlaytestRecordingRun::ClaimEnded(NoLock).IsValid());
	TestFalse(TEXT("Claiming that folder did not make it a run"), IFileManager::Get().FileExists(*FPaths::Combine(NoLock, FFlockPlaytestRecordingsFolder::LockFileName)));

	FString CreateError;
	TestFalse(TEXT("A folder that cannot be made gives no run"),
		FFlockPlaytestRecordingRun::Create(FPaths::Combine(Test.Folder, TEXT("not|a|folder")), EFlockPlaytestRecordingKind::Playtest, 0, CreateError).IsValid());
	TestFalse(TEXT("And says why"), CreateError.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestRecordingsDefaultFolderTest,
	"Flock.Playtest.Recordings.DefaultFolderIsUnderSaved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestRecordingsDefaultFolderTest::RunTest(const FString& Parameters)
{
	const FString Default = FFlockPlaytestRecordingsFolder::GetDefaultPath();
	TestTrue(FString::Printf(TEXT("Saved/FlockPlaytest/Recordings under the project (%s)"), *Default),
		FPaths::IsSamePath(Default, FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FlockPlaytest"), TEXT("Recordings"))));
	TestFalse(TEXT("As a full path"), FPaths::IsRelative(Default));
	TestEqual(TEXT("Playtest recordings under Playtest"),
		FPaths::GetCleanFilename(FFlockPlaytestRecordingsFolder::GetKindFolder(Default, EFlockPlaytestRecordingKind::Playtest)), FString(TEXT("Playtest")));
	TestEqual(TEXT("Test videos under TestVideos"),
		FPaths::GetCleanFilename(FFlockPlaytestRecordingsFolder::GetKindFolder(Default, EFlockPlaytestRecordingKind::TestVideo)), FString(TEXT("TestVideos")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestRecordingsKeepsARecordingWaitingToUploadTest,
	"Flock.Playtest.Recordings.KeepsAPlaytestRecordingWaitingToUpload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestRecordingsKeepsARecordingWaitingToUploadTest::RunTest(const FString& Parameters)
{
	FRecordingsTestFolder Test;
	const TArray<uint8> Video = MakeRecordingsTestVideoBytes(3, 100, 3);
	const FFlockPlaytestRecordingSession Session = MakeRecordingsTestSession(TEXT("01KX0RECORDINGSESSION00001"));
	const FString RunFolder = MakeEndedRecordingRun(*this, Test.Recordings, EFlockPlaytestRecordingKind::Playtest, TEXT("20260915-100000-aaaaaaaa"),
		&Video, nullptr, &Session);
	// What a crash between writing a session file and moving it into place leaves.
	const FString StrayTemporaryFile = FPaths::Combine(RunFolder, TEXT("session.json.0123456789ABCDEF0123456789ABCDEF.tmp"));
	TestTrue(TEXT("Precondition: a stray temporary file is saved"), SaveRecordingsTestFile(StrayTemporaryFile, Video));

	const FFlockPlaytestWhatEndedRunsLeft Result = FFlockPlaytestRecordingsFolder::FinishWhatEndedRunsLeft(Test.Recordings);
	TestEqual(TEXT("Kept, waiting to be uploaded"), Result.RecordingsWaitingToUpload, 1);
	TestEqual(TEXT("Nothing deleted"), Result.RecordingsWithoutASessionDeleted, 0);
	TestEqual(TEXT("Nothing to finish"), Result.InterruptedRecordingsFinished, 0);
	TestEqual(TEXT("Nothing left for the next launch"), Result.FilesLeftForTheNextLaunch.Num(), 0);
	TestFalse(TEXT("The stray temporary file is deleted"), IFileManager::Get().FileExists(*StrayTemporaryFile));

	const TArray<FFlockPlaytestRecordingWaitingToUpload> Waiting = FFlockPlaytestRecordingsFolder::FindRecordingsWaitingToUpload(Test.Recordings);
	if (TestEqual(TEXT("One recording waits to be uploaded"), Waiting.Num(), 1))
	{
		TestTrue(TEXT("In its run's folder"), FPaths::IsSamePath(Waiting[0].RunFolder, RunFolder));
		TestEqual(TEXT("Named by its run"), FPaths::GetCleanFilename(Waiting[0].VideoFilePath), FString(TEXT("recording-20260915-100000-aaaaaaaa.ivf")));
		TestEqual(TEXT("Unchanged"), Waiting[0].VideoBytes, static_cast<int64>(Video.Num()));
		TestEqual(TEXT("To the session it belongs to"), Waiting[0].Session.ProtokiteSessionId, Session.ProtokiteSessionId);
		TestEqual(TEXT("At the address that session started at"), Waiting[0].Session.ProtokiteApiUrl, Session.ProtokiteApiUrl);
		TestEqual(TEXT("Under the Game Version ID it started with"), Waiting[0].Session.FlockGameVersionId, Session.FlockGameVersionId);
	}
	TestEqual(TEXT("And kept again by the launch after"), FFlockPlaytestRecordingsFolder::FinishWhatEndedRunsLeft(Test.Recordings).RecordingsWaitingToUpload, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestRecordingsDeletesARecordingNoSessionStartedForTest,
	"Flock.Playtest.Recordings.DeletesAPlaytestRecordingNoSessionStartedFor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestRecordingsDeletesARecordingNoSessionStartedForTest::RunTest(const FString& Parameters)
{
	FRecordingsTestFolder Test;
	const TArray<uint8> Finished = MakeRecordingsTestVideoBytes(3, 100, 3);
	const TArray<uint8> Unfinished = MakeRecordingsTestVideoBytes(3, 100, 0);
	const FString FinishedRun = MakeEndedRecordingRun(*this, Test.Recordings, EFlockPlaytestRecordingKind::Playtest, TEXT("20260101-000000-aaaaaaaa"),
		&Finished, nullptr, nullptr);
	const FString UnfinishedRun = MakeEndedRecordingRun(*this, Test.Recordings, EFlockPlaytestRecordingKind::Playtest, TEXT("20260101-000001-bbbbbbbb"),
		nullptr, &Unfinished, nullptr);
	const FString EmptyPlaytestRun = MakeEndedRecordingRun(*this, Test.Recordings, EFlockPlaytestRecordingKind::Playtest, TEXT("20260101-000002-cccccccc"),
		nullptr, nullptr, nullptr);
	const FString EmptyTestVideoRun = MakeEndedRecordingRun(*this, Test.Recordings, EFlockPlaytestRecordingKind::TestVideo, TEXT("20260101-000003-dddddddd"),
		nullptr, nullptr, nullptr);

	const FFlockPlaytestWhatEndedRunsLeft Result = FFlockPlaytestRecordingsFolder::FinishWhatEndedRunsLeft(Test.Recordings);
	TestEqual(TEXT("Both playtest recordings no session started for are deleted"), Result.RecordingsWithoutASessionDeleted, 2);
	TestEqual(TEXT("The unfinished one is not finished first"), Result.InterruptedRecordingsFinished, 0);
	TestEqual(TEXT("None waits to be uploaded"), Result.RecordingsWaitingToUpload, 0);
	TestEqual(TEXT("Nothing left for the next launch"), Result.FilesLeftForTheNextLaunch.Num(), 0);
	TestFalse(TEXT("The finished one's folder is gone"), RecordingsTestFolderExists(FinishedRun));
	TestFalse(TEXT("The unfinished one's folder is gone"), RecordingsTestFolderExists(UnfinishedRun));
	TestFalse(TEXT("A playtest run that recorded nothing is removed"), RecordingsTestFolderExists(EmptyPlaytestRun));
	TestFalse(TEXT("So is a test video run that recorded nothing"), RecordingsTestFolderExists(EmptyTestVideoRun));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestRecordingsFinishesAnInterruptedRecordingTest,
	"Flock.Playtest.Recordings.FinishesARecordingCutOffWhenItsGameEnded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestRecordingsFinishesAnInterruptedRecordingTest::RunTest(const FString& Parameters)
{
	FRecordingsTestFolder Test;
	const FFlockPlaytestRecordingSession Session = MakeRecordingsTestSession(TEXT("01KX0RECORDINGSESSION00001"));

	// Five whole frames, then a sixth whose header promises 100 bytes, of which 40 were written.
	TArray<uint8> CutOffPlaytest = MakeRecordingsTestVideoBytes(5, 100, 0);
	AppendRecordingsTestFrameHeader(CutOffPlaytest, 5, 100);
	CutOffPlaytest.Append(MakeRecordingsTestFrame(5, 40));
	// Three whole frames, then seven bytes of the next frame's header.
	TArray<uint8> CutOffTestVideo = MakeRecordingsTestVideoBytes(3, 100, 0);
	CutOffTestVideo.Append({ 100, 0, 0, 0, 99, 0, 0 });
	const TArray<uint8> HeaderOnly = MakeRecordingsTestVideoBytes(0, 0, 0);
	TArray<uint8> NotAVideo;
	for (const TCHAR Character : FString(TEXT("Not a video file at all, only some words that were saved here.")))
	{
		NotAVideo.Add(static_cast<uint8>(Character));
	}

	const FString PlaytestRun = MakeEndedRecordingRun(*this, Test.Recordings, EFlockPlaytestRecordingKind::Playtest, TEXT("20260101-000000-aaaaaaaa"),
		nullptr, &CutOffPlaytest, &Session);
	const FString TestVideoRun = MakeEndedRecordingRun(*this, Test.Recordings, EFlockPlaytestRecordingKind::TestVideo, TEXT("20260101-000001-bbbbbbbb"),
		nullptr, &CutOffTestVideo, nullptr);
	const FString HeaderOnlyRun = MakeEndedRecordingRun(*this, Test.Recordings, EFlockPlaytestRecordingKind::TestVideo, TEXT("20260101-000002-cccccccc"),
		nullptr, &HeaderOnly, nullptr);
	const FString NotAVideoRun = MakeEndedRecordingRun(*this, Test.Recordings, EFlockPlaytestRecordingKind::Playtest, TEXT("20260101-000003-dddddddd"),
		nullptr, &NotAVideo, &Session);
	TestEqual(TEXT("A cut-off recording is not listed for upload before a launch finishes it"),
		FFlockPlaytestRecordingsFolder::FindRecordingsWaitingToUpload(Test.Recordings).Num(), 0);

	const FFlockPlaytestWhatEndedRunsLeft Result = FFlockPlaytestRecordingsFolder::FinishWhatEndedRunsLeft(Test.Recordings);
	TestEqual(TEXT("The two with whole frames are finished"), Result.InterruptedRecordingsFinished, 2);
	TestEqual(TEXT("The playtest one waits to be uploaded"), Result.RecordingsWaitingToUpload, 1);
	TestEqual(TEXT("Nothing is deleted for want of a session"), Result.RecordingsWithoutASessionDeleted, 0);
	TestEqual(TEXT("Nothing left for the next launch"), Result.FilesLeftForTheNextLaunch.Num(), 0);

	const FString PlaytestVideo = FPaths::Combine(PlaytestRun, TEXT("recording-20260101-000000-aaaaaaaa.ivf"));
	TestFalse(TEXT("No unfinished playtest file is left"), IFileManager::Get().FileExists(*(PlaytestVideo + TEXT(".part"))));
	FVideoFileRead PlaytestFile;
	if (TestTrue(TEXT("The finished playtest recording reads back"), ReadVideoFile(PlaytestVideo, PlaytestFile)))
	{
		TestEqual(TEXT("With its five whole frames"), PlaytestFile.Frames.Num(), 5);
		TestEqual(TEXT("Counted in its header"), PlaytestFile.FrameCountInHeader, 5);
		TestEqual(TEXT("And the frame cut off removed"), PlaytestFile.FileBytes, 32LL + 5 * (12 + 100));
		if (PlaytestFile.Frames.Num() == 5)
		{
			TestTrue(TEXT("The last whole frame is unchanged"), PlaytestFile.Frames[4].Bytes == MakeRecordingsTestFrame(4, 100));
		}
	}
	TestEqual(TEXT("Once finished, it is listed for upload"), FFlockPlaytestRecordingsFolder::FindRecordingsWaitingToUpload(Test.Recordings).Num(), 1);

	FVideoFileRead TestVideoFile;
	if (TestTrue(TEXT("The finished test video reads back"),
		ReadVideoFile(FPaths::Combine(TestVideoRun, TEXT("test-recording-20260101-000001-bbbbbbbb.ivf")), TestVideoFile)))
	{
		TestEqual(TEXT("With its three whole frames"), TestVideoFile.Frames.Num(), 3);
		TestEqual(TEXT("Counted in its header"), TestVideoFile.FrameCountInHeader, 3);
	}
	TestFalse(TEXT("A video holding no frame is removed with its run"), RecordingsTestFolderExists(HeaderOnlyRun));
	TestFalse(TEXT("So is a file that is no video"), RecordingsTestFolderExists(NotAVideoRun));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestRecordingsLeavesRunsInUseAloneTest,
	"Flock.Playtest.Recordings.LeavesRunsStillInUseAlone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestRecordingsLeavesRunsInUseAloneTest::RunTest(const FString& Parameters)
{
	FRecordingsTestFolder Test;
	FString Error;
	const TArray<uint8> Unfinished = MakeRecordingsTestVideoBytes(3, 100, 0);
	const TArray<uint8> Finished = MakeRecordingsTestVideoBytes(3, 100, 3);
	const FFlockPlaytestRecordingSession Session = MakeRecordingsTestSession(TEXT("01KX0RECORDINGSESSION00001"));

	// A game still recording for its playtest, with no session yet: once ended, it would be deleted.
	TSharedPtr<FFlockPlaytestRecordingRun> RunningPlaytest = FFlockPlaytestRecordingRun::CreateNamedForTesting(Test.Recordings,
		EFlockPlaytestRecordingKind::Playtest, TEXT("20260101-000000-aaaaaaaa"), /*ReservedBytes*/ 0, Error);
	// A game still recording a test video: once ended, it would be finished.
	TSharedPtr<FFlockPlaytestRecordingRun> RunningTestVideo = FFlockPlaytestRecordingRun::CreateNamedForTesting(Test.Recordings,
		EFlockPlaytestRecordingKind::TestVideo, TEXT("20260101-000001-bbbbbbbb"), /*ReservedBytes*/ 0, Error);
	if (!TestTrue(TEXT("Precondition: the running games' runs are made"), RunningPlaytest.IsValid() && RunningTestVideo.IsValid()))
	{
		return true;
	}
	SaveRecordingsTestFile(RunningPlaytest->GetUnfinishedVideoFilePath(), Unfinished);
	SaveRecordingsTestFile(RunningTestVideo->GetUnfinishedVideoFilePath(), Unfinished);
	const FString RunningPlaytestVideo = RunningPlaytest->GetUnfinishedVideoFilePath();
	const FString RunningTestVideoVideo = RunningTestVideo->GetUnfinishedVideoFilePath();

	// An ended run another launch has claimed, to upload it.
	const FString HeldFolder = MakeEndedRecordingRun(*this, Test.Recordings, EFlockPlaytestRecordingKind::Playtest, TEXT("20260101-000002-cccccccc"),
		&Finished, nullptr, &Session);
	TSharedPtr<FFlockPlaytestRecordingRun> HeldByAnotherLaunch = FFlockPlaytestRecordingRun::ClaimEnded(HeldFolder);
	if (!TestTrue(TEXT("Precondition: another launch holds the ended run"), HeldByAnotherLaunch.IsValid()))
	{
		return true;
	}
	TestFalse(TEXT("Precondition: a running game's run cannot be claimed"), FFlockPlaytestRecordingRun::ClaimEnded(RunningPlaytest->GetFolder()).IsValid());

	const FFlockPlaytestWhatEndedRunsLeft WhileInUse = FFlockPlaytestRecordingsFolder::FinishWhatEndedRunsLeft(Test.Recordings);
	TestEqual(TEXT("Nothing is deleted"), WhileInUse.RecordingsWithoutASessionDeleted, 0);
	TestEqual(TEXT("Nothing is finished"), WhileInUse.InterruptedRecordingsFinished, 0);
	TestEqual(TEXT("Nothing is counted as waiting"), WhileInUse.RecordingsWaitingToUpload, 0);
	TestEqual(TEXT("The running playtest recording is untouched"), IFileManager::Get().FileSize(*RunningPlaytestVideo), static_cast<int64>(Unfinished.Num()));
	TestEqual(TEXT("The running test video is untouched"), IFileManager::Get().FileSize(*RunningTestVideoVideo), static_cast<int64>(Unfinished.Num()));
	TestEqual(TEXT("A run another launch holds is not listed for upload"),
		FFlockPlaytestRecordingsFolder::FindRecordingsWaitingToUpload(Test.Recordings).Num(), 0);
	const FFlockPlaytestRecordingsRoom Room = FFlockPlaytestRecordingsFolder::MakeRoom(Test.Recordings, /*BudgetBytes*/ 1, /*BytesWanted*/ 1024 * 1024);
	TestEqual(TEXT("Making room deletes no playtest recording in use"), Room.PlaytestRecordingsDeleted.Num(), 0);
	TestEqual(TEXT("And no test video in use"), Room.TestVideosDeleted.Num(), 0);

	// Once every launch has let go, the next one sees to them.
	RunningPlaytest.Reset();
	RunningTestVideo.Reset();
	HeldByAnotherLaunch.Reset();
	const FFlockPlaytestWhatEndedRunsLeft Afterwards = FFlockPlaytestRecordingsFolder::FinishWhatEndedRunsLeft(Test.Recordings);
	TestEqual(TEXT("Control: the playtest recording no session started for is deleted"), Afterwards.RecordingsWithoutASessionDeleted, 1);
	TestEqual(TEXT("Control: the test video is finished"), Afterwards.InterruptedRecordingsFinished, 1);
	TestEqual(TEXT("Control: the recording the other launch held waits to be uploaded"), Afterwards.RecordingsWaitingToUpload, 1);
	TestEqual(TEXT("Control: and is listed"), FFlockPlaytestRecordingsFolder::FindRecordingsWaitingToUpload(Test.Recordings).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestRecordingsLeavesAFileInUseForTheNextLaunchTest,
	"Flock.Playtest.Recordings.LeavesAFileAnotherProgramHasOpenForTheNextLaunch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestRecordingsLeavesAFileInUseForTheNextLaunchTest::RunTest(const FString& Parameters)
{
	FRecordingsTestFolder Test;
	const TArray<uint8> Finished = MakeRecordingsTestVideoBytes(3, 100, 3);
	const TArray<uint8> Unfinished = MakeRecordingsTestVideoBytes(3, 100, 0);
	const FString NoSessionRun = MakeEndedRecordingRun(*this, Test.Recordings, EFlockPlaytestRecordingKind::Playtest, TEXT("20260101-000000-aaaaaaaa"),
		&Finished, nullptr, nullptr);
	const FString CutOffRun = MakeEndedRecordingRun(*this, Test.Recordings, EFlockPlaytestRecordingKind::TestVideo, TEXT("20260101-000001-bbbbbbbb"),
		nullptr, &Unfinished, nullptr);
	const FString NoSessionVideo = FPaths::Combine(NoSessionRun, TEXT("recording-20260101-000000-aaaaaaaa.ivf"));
	const FString CutOffVideo = FPaths::Combine(CutOffRun, TEXT("test-recording-20260101-000001-bbbbbbbb.ivf.part"));
	{
		// A video player has both open.
		const TUniquePtr<FArchive> Player(IFileManager::Get().CreateFileReader(*NoSessionVideo));
		const TUniquePtr<FArchive> OtherPlayer(IFileManager::Get().CreateFileReader(*CutOffVideo));
		if (!TestTrue(TEXT("Precondition: both files are open"), Player.IsValid() && OtherPlayer.IsValid()))
		{
			return true;
		}
		const FFlockPlaytestWhatEndedRunsLeft WhileOpen = FFlockPlaytestRecordingsFolder::FinishWhatEndedRunsLeft(Test.Recordings);
		TestEqual(TEXT("Both are left for the next launch"), WhileOpen.FilesLeftForTheNextLaunch.Num(), 2);
		TestEqual(TEXT("Neither counts as done"), WhileOpen.RecordingsWithoutASessionDeleted + WhileOpen.InterruptedRecordingsFinished, 0);
		TestTrue(TEXT("The run that could not be deleted is still a run the next launch finds"),
			IFileManager::Get().FileExists(*FPaths::Combine(NoSessionRun, FFlockPlaytestRecordingsFolder::LockFileName)));
		TestEqual(TEXT("The cut-off file is left as it was"), IFileManager::Get().FileSize(*CutOffVideo), static_cast<int64>(Unfinished.Num()));
	}

	const FFlockPlaytestWhatEndedRunsLeft Afterwards = FFlockPlaytestRecordingsFolder::FinishWhatEndedRunsLeft(Test.Recordings);
	TestEqual(TEXT("Once the player lets go, the next launch deletes the one"), Afterwards.RecordingsWithoutASessionDeleted, 1);
	TestFalse(TEXT("With its folder"), RecordingsTestFolderExists(NoSessionRun));
	TestEqual(TEXT("And finishes the other"), Afterwards.InterruptedRecordingsFinished, 1);
	TestEqual(TEXT("Nothing is left"), Afterwards.FilesLeftForTheNextLaunch.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestRecordingsNeverTouchesAnythingElseTest,
	"Flock.Playtest.Recordings.NeverTouchesAnythingElse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestRecordingsNeverTouchesAnythingElseTest::RunTest(const FString& Parameters)
{
	FRecordingsTestFolder Test;
	const TArray<uint8> Bytes = MakeRecordingsTestVideoBytes(2, 100, 2);
	const TArray<FString> Others = {
		// Beside the recordings folder: a device id, a feedback form's answers waiting to be sent, and the Flock SDK's event queue.
		FPaths::Combine(Test.Folder, TEXT("device_id.txt")),
		FPaths::Combine(Test.Folder, TEXT("Feedback"), TEXT("answers-waiting-to-be-sent.json")),
		FPaths::Combine(Test.Folder, TEXT("Flock"), TEXT("analytics"), TEXT("analytics_events"), TEXT("0001.json")),
		// Inside it, but no run's: a note, a video copied in by hand, and a run-shaped folder outside Playtest and TestVideos.
		FPaths::Combine(Test.Recordings, TEXT("notes.txt")),
		FPaths::Combine(Test.Recordings, TEXT("Playtest"), TEXT("copied-by-hand"), TEXT("recording-copied.ivf")),
		FPaths::Combine(Test.Recordings, TEXT("Elsewhere"), TEXT("20200101-000000-aaaaaaaa"), FFlockPlaytestRecordingsFolder::LockFileName),
		FPaths::Combine(Test.Recordings, TEXT("Elsewhere"), TEXT("20200101-000000-aaaaaaaa"), TEXT("recording-20200101-000000-aaaaaaaa.ivf")),
	};
	for (const FString& Path : Others)
	{
		TestTrue(FString::Printf(TEXT("Precondition: %s is saved"), *Path), SaveRecordingsTestFile(Path, Bytes));
	}
	const FString EndedRun = MakeEndedRecordingRun(*this, Test.Recordings, EFlockPlaytestRecordingKind::TestVideo, TEXT("20200101-000001-bbbbbbbb"),
		&Bytes, nullptr, nullptr);

	FFlockPlaytestRecordingsFolder::FinishWhatEndedRunsLeft(Test.Recordings);
	FFlockPlaytestRecordingsFolder::FindRecordingsWaitingToUpload(Test.Recordings);
	const FFlockPlaytestRecordingsRoom Room = FFlockPlaytestRecordingsFolder::MakeRoom(Test.Recordings, /*BudgetBytes*/ 1, /*BytesWanted*/ 1024 * 1024);
	TestFalse(TEXT("Control: the ended run was deleted to make room"), RecordingsTestFolderExists(EndedRun));
	TestEqual(TEXT("Nothing that is not a run's is counted"), Room.BytesUsed, 0LL);
	for (const FString& Path : Others)
	{
		TestEqual(FString::Printf(TEXT("%s is untouched"), *Path), IFileManager::Get().FileSize(*Path), static_cast<int64>(Bytes.Num()));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestRecordingsMakeRoomDeletesTestVideosFirstTest,
	"Flock.Playtest.Recordings.MakingRoomDeletesTestVideosFirstThenTheOldestUpload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestRecordingsMakeRoomDeletesTestVideosFirstTest::RunTest(const FString& Parameters)
{
	FRecordingsTestFolder Test;
	const TArray<uint8> Video = MakeRecordingsTestVideoBytes(1, 100 * 1024, 1);
	const FFlockPlaytestRecordingSession Session = MakeRecordingsTestSession(TEXT("01KX0RECORDINGSESSION00001"));
	// Made newest first, so the files' times run the other way from the runs' names. The oldest run is a playtest recording.
	const FString NewestTestVideo = MakeEndedRecordingRun(*this, Test.Recordings, EFlockPlaytestRecordingKind::TestVideo, TEXT("20260101-000003-dddddddd"),
		&Video, nullptr, nullptr);
	const FString NewerUpload = MakeEndedRecordingRun(*this, Test.Recordings, EFlockPlaytestRecordingKind::Playtest, TEXT("20260101-000002-cccccccc"),
		&Video, nullptr, &Session);
	const FString OlderTestVideo = MakeEndedRecordingRun(*this, Test.Recordings, EFlockPlaytestRecordingKind::TestVideo, TEXT("20260101-000001-bbbbbbbb"),
		&Video, nullptr, nullptr);
	const FString OldestUpload = MakeEndedRecordingRun(*this, Test.Recordings, EFlockPlaytestRecordingKind::Playtest, TEXT("20260101-000000-aaaaaaaa"),
		&Video, nullptr, &Session);
	const int64 NewestTestVideoBytes = RecordingsTestBytesIn(NewestTestVideo);
	const int64 NewerUploadBytes = RecordingsTestBytesIn(NewerUpload);
	const int64 OlderTestVideoBytes = RecordingsTestBytesIn(OlderTestVideo);
	const int64 OldestUploadBytes = RecordingsTestBytesIn(OldestUpload);
	const int64 Wanted = 50 * 1024;

	FFlockPlaytestRecordingsRoom Room = FFlockPlaytestRecordingsFolder::MakeRoom(Test.Recordings,
		NewestTestVideoBytes + NewerUploadBytes + OlderTestVideoBytes + OldestUploadBytes + Wanted, Wanted);
	TestEqual(TEXT("With room for everything, nothing is deleted"), Room.TestVideosDeleted.Num() + Room.PlaytestRecordingsDeleted.Num(), 0);
	TestEqual(TEXT("Everything is counted"), Room.BytesUsed, NewestTestVideoBytes + NewerUploadBytes + OlderTestVideoBytes + OldestUploadBytes);
	TestEqual(TEXT("And the budget has room left for the new recording"), Room.BytesLeftInBudget, Wanted);

	// Room for both uploads and the new recording: both test videos go, oldest first, and no upload, though one is older.
	Room = FFlockPlaytestRecordingsFolder::MakeRoom(Test.Recordings, NewerUploadBytes + OldestUploadBytes + Wanted, Wanted);
	if (TestEqual(TEXT("Both test videos are deleted"), Room.TestVideosDeleted.Num(), 2))
	{
		TestTrue(TEXT("The older one first"), FPaths::IsSamePath(FPaths::GetPath(Room.TestVideosDeleted[0]), OlderTestVideo));
		TestTrue(TEXT("Then the newest"), FPaths::IsSamePath(FPaths::GetPath(Room.TestVideosDeleted[1]), NewestTestVideo));
	}
	TestEqual(TEXT("No recording waiting to be uploaded is deleted while test videos can go"), Room.PlaytestRecordingsDeleted.Num(), 0);
	TestTrue(TEXT("The oldest upload is kept"), RecordingsTestFolderExists(OldestUpload));
	TestEqual(TEXT("And the new recording fits"), Room.BytesLeftInBudget, Wanted);

	// One byte short of keeping both uploads: only then does one go, the oldest.
	Room = FFlockPlaytestRecordingsFolder::MakeRoom(Test.Recordings, NewerUploadBytes + OldestUploadBytes + Wanted - 1, Wanted);
	if (TestEqual(TEXT("One recording waiting to be uploaded is deleted"), Room.PlaytestRecordingsDeleted.Num(), 1))
	{
		TestTrue(TEXT("The oldest"), FPaths::IsSamePath(FPaths::GetPath(Room.PlaytestRecordingsDeleted[0]), OldestUpload));
	}
	TestTrue(TEXT("The newer upload is kept"), RecordingsTestFolderExists(NewerUpload));
	TestEqual(TEXT("What is left is counted"), Room.BytesUsed, NewerUploadBytes);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestRecordingsMakeRoomCountsRunsInUseTest,
	"Flock.Playtest.Recordings.MakingRoomCountsRunsInUseAndNeverDeletesThem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestRecordingsMakeRoomCountsRunsInUseTest::RunTest(const FString& Parameters)
{
	FRecordingsTestFolder Test;
	FString Error;
	// The older run belongs to a game still recording: 200 KB so far, of the 400 KB it may grow to.
	const int64 Reserved = 400 * 1024;
	const TSharedPtr<FFlockPlaytestRecordingRun> StillRunning = FFlockPlaytestRecordingRun::CreateNamedForTesting(Test.Recordings,
		EFlockPlaytestRecordingKind::TestVideo, TEXT("20260101-000000-aaaaaaaa"), Reserved, Error);
	if (!TestTrue(TEXT("Precondition: the running game's run is made"), StillRunning.IsValid()))
	{
		return true;
	}
	const TArray<uint8> RunningVideo = MakeRecordingsTestVideoBytes(1, 200 * 1024, 0);
	SaveRecordingsTestFile(StillRunning->GetUnfinishedVideoFilePath(), RunningVideo);
	const TArray<uint8> Video = MakeRecordingsTestVideoBytes(1, 100 * 1024, 1);
	const FString Ended = MakeEndedRecordingRun(*this, Test.Recordings, EFlockPlaytestRecordingKind::TestVideo, TEXT("20260101-000001-bbbbbbbb"),
		&Video, nullptr, nullptr);
	const int64 Wanted = 100 * 1024;

	FFlockPlaytestRecordingsRoom Room = FFlockPlaytestRecordingsFolder::MakeRoom(Test.Recordings, Reserved + 50 * 1024, Wanted);
	TestEqual(TEXT("The ended run is deleted, although the running one is older"), Room.TestVideosDeleted.Num(), 1);
	TestFalse(TEXT("Its folder is gone"), RecordingsTestFolderExists(Ended));
	TestEqual(TEXT("The running game's recording is untouched"), IFileManager::Get().FileSize(*StillRunning->GetUnfinishedVideoFilePath()),
		static_cast<int64>(RunningVideo.Num()));
	TestEqual(TEXT("And counted at the size it may grow to, not the size it has"), Room.BytesUsed, Reserved);
	TestEqual(TEXT("The budget has what is left after that"), Room.BytesLeftInBudget, 50LL * 1024);

	Room = FFlockPlaytestRecordingsFolder::MakeRoom(Test.Recordings, Reserved - 1, Wanted);
	TestEqual(TEXT("With a running game's recording filling the budget, nothing is left"), Room.BytesLeftInBudget, 0LL);
	TestTrue(TEXT("And it is still not deleted"), IFileManager::Get().FileExists(*StillRunning->GetUnfinishedVideoFilePath()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestVideoFileFinishesAnInterruptedFileTest,
	"Flock.Playtest.Video.File.FinishesAFileCutOffPartWay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestVideoFileFinishesAnInterruptedFileTest::RunTest(const FString& Parameters)
{
	FRecordingsTestFolder Test;

	struct FCutOffCase
	{
		const TCHAR* What;
		TArray<uint8> Bytes;
		EFlockPlaytestInterruptedVideoResult Expected;
		int32 WholeFrames;
	};
	TArray<FCutOffCase> Cases;
	Cases.Add({ TEXT("Whole frames with no count yet"), MakeRecordingsTestVideoBytes(4, 100, 0), EFlockPlaytestInterruptedVideoResult::Finished, 4 });
	{
		TArray<uint8> Bytes = MakeRecordingsTestVideoBytes(4, 100, 0);
		AppendRecordingsTestFrameHeader(Bytes, 4, 100);
		Bytes.Append(MakeRecordingsTestFrame(4, 60));
		Cases.Add({ TEXT("A frame only partly written"), MoveTemp(Bytes), EFlockPlaytestInterruptedVideoResult::Finished, 4 });
	}
	{
		TArray<uint8> Bytes = MakeRecordingsTestVideoBytes(4, 100, 0);
		Bytes.Append({ 100, 0, 0, 0, 16 });
		Cases.Add({ TEXT("A frame header only partly written"), MoveTemp(Bytes), EFlockPlaytestInterruptedVideoResult::Finished, 4 });
	}
	{
		TArray<uint8> Bytes = MakeRecordingsTestVideoBytes(2, 100, 0);
		Bytes.AddZeroed(64);
		Cases.Add({ TEXT("Zeros after the last whole frame"), MoveTemp(Bytes), EFlockPlaytestInterruptedVideoResult::Finished, 2 });
	}
	{
		// The frame's header was written, and its bytes were never: the disk kept the length and filled it with zeros.
		TArray<uint8> Bytes = MakeRecordingsTestVideoBytes(3, 100, 0);
		AppendRecordingsTestFrameHeader(Bytes, 3, 100);
		Bytes.AddZeroed(100);
		Cases.Add({ TEXT("A whole-sized frame of zeros"), MoveTemp(Bytes), EFlockPlaytestInterruptedVideoResult::Finished, 3 });
	}
	{
		// A frame header that says zero bytes, then a whole frame whose size's first byte (130) looks like a VP9 frame's start.
		TArray<uint8> Bytes = MakeRecordingsTestVideoBytes(2, 100, 0);
		AppendRecordingsTestFrameHeader(Bytes, 2, 0);
		AppendRecordingsTestFrameHeader(Bytes, 3, 130);
		Bytes.Append(MakeRecordingsTestFrame(3, 130));
		Cases.Add({ TEXT("A frame of zero bytes"), MoveTemp(Bytes), EFlockPlaytestInterruptedVideoResult::Finished, 2 });
	}
	Cases.Add({ TEXT("A header and no frame"), MakeRecordingsTestVideoBytes(0, 0, 0), EFlockPlaytestInterruptedVideoResult::HeldNoFrame, 0 });
	{
		TArray<uint8> Bytes;
		for (const TCHAR Character : FString(TEXT("Only some words, and no video header before them.")))
		{
			Bytes.Add(static_cast<uint8>(Character));
		}
		Cases.Add({ TEXT("No video at all"), MoveTemp(Bytes), EFlockPlaytestInterruptedVideoResult::HeldNoFrame, 0 });
	}
	{
		TArray<uint8> Bytes = MakeRecordingsTestVideoBytes(2, 100, 0);
		Bytes[0] = 'R';
		Bytes[1] = 'I';
		Bytes[2] = 'F';
		Bytes[3] = 'F';
		Cases.Add({ TEXT("Frames behind another kind of file's header"), MoveTemp(Bytes), EFlockPlaytestInterruptedVideoResult::HeldNoFrame, 0 });
	}
	Cases.Add({ TEXT("An empty file"), TArray<uint8>(), EFlockPlaytestInterruptedVideoResult::HeldNoFrame, 0 });

	for (int32 Index = 0; Index < Cases.Num(); ++Index)
	{
		const FCutOffCase& Case = Cases[Index];
		const FString Unfinished = FPaths::Combine(Test.Folder, FString::Printf(TEXT("case-%d.ivf.part"), Index));
		const FString Finished = FPaths::Combine(Test.Folder, FString::Printf(TEXT("case-%d.ivf"), Index));
		if (!TestTrue(FString::Printf(TEXT("%s: precondition, the file is saved"), Case.What), SaveRecordingsTestFile(Unfinished, Case.Bytes)))
		{
			continue;
		}
		int32 FramesKept = -1;
		FString Error;
		const EFlockPlaytestInterruptedVideoResult Result = FFlockPlaytestVideoFile::FinishInterruptedFile(Unfinished, Finished, FramesKept, Error);
		TestEqual(FString::Printf(TEXT("%s: what became of it (%s)"), Case.What, *Error), static_cast<int32>(Result), static_cast<int32>(Case.Expected));
		TestFalse(FString::Printf(TEXT("%s: no unfinished file is left"), Case.What), IFileManager::Get().FileExists(*Unfinished));
		if (Case.Expected != EFlockPlaytestInterruptedVideoResult::Finished)
		{
			TestFalse(FString::Printf(TEXT("%s: and no finished one is made"), Case.What), IFileManager::Get().FileExists(*Finished));
			continue;
		}
		TestEqual(FString::Printf(TEXT("%s: the frames kept"), Case.What), FramesKept, Case.WholeFrames);
		FVideoFileRead File;
		if (TestTrue(FString::Printf(TEXT("%s: the finished file reads back"), Case.What), ReadVideoFile(Finished, File)))
		{
			TestEqual(FString::Printf(TEXT("%s: every whole frame"), Case.What), File.Frames.Num(), Case.WholeFrames);
			TestEqual(FString::Printf(TEXT("%s: counted in the header"), Case.What), File.FrameCountInHeader, Case.WholeFrames);
			TestEqual(FString::Printf(TEXT("%s: and nothing after them"), Case.What), File.FileBytes, 32LL + Case.WholeFrames * (12 + 100));
		}
	}

	// A file another program has open is left as it was.
	const FString Held = FPaths::Combine(Test.Folder, TEXT("held.ivf.part"));
	const TArray<uint8> HeldBytes = MakeRecordingsTestVideoBytes(4, 100, 0);
	SaveRecordingsTestFile(Held, HeldBytes);
	{
		const TUniquePtr<FArchive> Player(IFileManager::Get().CreateFileReader(*Held));
		if (TestTrue(TEXT("Precondition: the file is open"), Player.IsValid()))
		{
			int32 FramesKept = -1;
			FString Error;
			const EFlockPlaytestInterruptedVideoResult Result = FFlockPlaytestVideoFile::FinishInterruptedFile(Held,
				FPaths::Combine(Test.Folder, TEXT("held.ivf")), FramesKept, Error);
			TestEqual(TEXT("A file another program has open is not finished"), static_cast<int32>(Result),
				static_cast<int32>(EFlockPlaytestInterruptedVideoResult::CouldNotFinish));
			TestFalse(TEXT("And the reason is given"), Error.IsEmpty());
		}
	}
	TestEqual(TEXT("It is left as it was"), IFileManager::Get().FileSize(*Held), static_cast<int64>(HeldBytes.Num()));
	return true;
}

#endif
