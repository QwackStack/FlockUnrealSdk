// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "ProtokitePlaytestRecordingsFolder.h"
#include "ProtokitePlaytestVideoFile.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/ProtokitePlaytestVideoTestSupport.h"

using namespace ProtokitePlaytestVideoTesting;

namespace
{
	/** A folder of the test's own, removed when the test ends, with the recordings folder inside it. */
	struct FRecordingsTestFolder
	{
		FString Folder = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("ProtokitePlaytestTests"),
			FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		FString Recordings = FPaths::Combine(Folder, TEXT("Recordings"));

		~FRecordingsTestFolder()
		{
			IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists*/ false, /*Tree*/ true);
		}
	};

	/** An EBML size: big-endian, behind a leading 1 bit saying how many bytes it takes. */
	void AppendRecordingsTestVint(TArray<uint8>& Bytes, uint64 Value, int32 Width)
	{
		for (int32 Index = 0; Index < Width; ++Index)
		{
			uint8 Byte = static_cast<uint8>((Value >> (8 * (Width - 1 - Index))) & 0xFF);
			if (Index == 0)
			{
				Byte |= static_cast<uint8>(1 << (8 - Width));
			}
			Bytes.Add(Byte);
		}
	}

	void AppendRecordingsTestBigEndian(TArray<uint8>& Bytes, uint64 Value, int32 Width)
	{
		for (int32 Index = 0; Index < Width; ++Index)
		{
			Bytes.Add(static_cast<uint8>((Value >> (8 * (Width - 1 - Index))) & 0xFF));
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

	/** A cluster header claiming a frame whose bytes never arrived: what a recording cut off mid-frame leaves behind. */
	void AppendRecordingsTestFrameHeader(TArray<uint8>& Bytes, int32 FrameIndex, int32 FrameBytes)
	{
		Bytes.Append({ 0x1F, 0x43, 0xB6, 0x75 });
		AppendRecordingsTestVint(Bytes, static_cast<uint64>(15 + FrameBytes), 4);
		Bytes.Add(0xE7);
		Bytes.Add(0x84);
		AppendRecordingsTestBigEndian(Bytes, static_cast<uint64>(FrameIndex * 33), 4);
		Bytes.Add(0xA3);
		AppendRecordingsTestVint(Bytes, static_cast<uint64>(4 + FrameBytes), 4);
		Bytes.Append({ 0x81, 0x00, 0x00, 0x00 });
	}

	/**
	 * Video bytes laid out the way a recording writes them, produced by the writer itself so this fixture cannot drift
	 * from the format it stands in for. bWrittenToTheEnd nonzero is a file that was closed properly; zero is one whose
	 * process died part-way through, which keeps the segment's "size unknown" marker exactly as the real thing does.
	 */
	TArray<uint8> MakeRecordingsTestVideoBytes(int32 Frames, int32 FrameBytes, int32 bWrittenToTheEnd)
	{
		const FString Scratch = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
			FString::Printf(TEXT("recordings-fixture-%s.webm"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Scratch), /*Tree*/ true);
		{
			FProtokitePlaytestVideoFile File;
			FString Error;
			if (!File.Open(Scratch, FIntPoint(64, 36), Error))
			{
				return TArray<uint8>();
			}
			for (int32 Index = 0; Index < Frames; ++Index)
			{
				File.WriteFrame(MakeRecordingsTestFrame(Index, FrameBytes), Index * 33, /*bKeyFrame*/ Index == 0);
			}
			if (bWrittenToTheEnd != 0)
			{
				File.Close();
			}
			else
			{
				File.AbandonForTesting();
			}
		}
		TArray<uint8> Bytes;
		FFileHelper::LoadFileToArray(Bytes, *Scratch);
		IFileManager::Get().Delete(*Scratch, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
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

	FProtokitePlaytestRecordingSession MakeRecordingsTestSession(const FString& SessionId)
	{
		FProtokitePlaytestRecordingSession Session;
		Session.ProtokiteSessionId = SessionId;
		Session.ProtokiteApiUrl = TEXT("http://127.0.0.1:8020");
		Session.FlockGameVersionId = TEXT("pt-recordings-test-version");
		return Session;
	}

	/**
	 * A run whose launch has ended, left the way it leaves one: its folder made and locked, given the videos and session
	 * handed in, then let go of. Returns the run's folder, or empty when it could not be made.
	 */
	FString MakeEndedRecordingRun(FAutomationTestBase& Test, const FString& Recordings, EProtokitePlaytestRecordingKind Kind, const FString& RunName,
		const TArray<uint8>* FinishedVideo, const TArray<uint8>* UnfinishedVideo, const FProtokitePlaytestRecordingSession* Session)
	{
		FString Error;
		const TSharedPtr<FProtokitePlaytestRecordingRun> Run = FProtokitePlaytestRecordingRun::CreateNamedForTesting(Recordings, Kind, RunName,
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestRecordingsRunHasAFolderOfItsOwnTest,
	"Protokite.Playtest.Recordings.ARunHasAFolderOfItsOwn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestRecordingsRunHasAFolderOfItsOwnTest::RunTest(const FString& Parameters)
{
	FRecordingsTestFolder Test;
	FString Error;
	TSharedPtr<FProtokitePlaytestRecordingRun> First = FProtokitePlaytestRecordingRun::Create(Test.Recordings, EProtokitePlaytestRecordingKind::Playtest,
		/*ReservedBytes*/ 12345, Error);
	const TSharedPtr<FProtokitePlaytestRecordingRun> Second = FProtokitePlaytestRecordingRun::Create(Test.Recordings, EProtokitePlaytestRecordingKind::Playtest,
		/*ReservedBytes*/ 0, Error);
	const TSharedPtr<FProtokitePlaytestRecordingRun> TestVideoRun = FProtokitePlaytestRecordingRun::Create(Test.Recordings, EProtokitePlaytestRecordingKind::TestVideo,
		/*ReservedBytes*/ 0, Error);
	if (!TestTrue(FString::Printf(TEXT("Precondition: the runs are made (%s)"), *Error), First.IsValid() && Second.IsValid() && TestVideoRun.IsValid()))
	{
		return true;
	}

	TestFalse(TEXT("Each run has a folder of its own"), FPaths::IsSamePath(First->GetFolder(), Second->GetFolder()));
	TestTrue(TEXT("A playtest recording's run is under Playtest"),
		FPaths::IsSamePath(FPaths::GetPath(First->GetFolder()), FProtokitePlaytestRecordingsFolder::GetKindFolder(Test.Recordings, EProtokitePlaytestRecordingKind::Playtest)));
	TestTrue(TEXT("A test video's run is under TestVideos"),
		FPaths::IsSamePath(FPaths::GetPath(TestVideoRun->GetFolder()), FProtokitePlaytestRecordingsFolder::GetKindFolder(Test.Recordings, EProtokitePlaytestRecordingKind::TestVideo)));

	const FString Name = First->GetName();
	TestTrue(FString::Printf(TEXT("Named by the UTC time it was made, then eight hex digits (%s)"), *Name),
		Name.Len() == 24 && Name[8] == TEXT('-') && Name[15] == TEXT('-') && Name.Left(8).IsNumeric() && Name.Mid(9, 6).IsNumeric());
	TestEqual(TEXT("A playtest recording's file"), FPaths::GetCleanFilename(First->GetVideoFilePath()), FString::Printf(TEXT("recording-%s.webm"), *Name));
	TestEqual(TEXT("A test video's file"), FPaths::GetCleanFilename(TestVideoRun->GetVideoFilePath()),
		FString::Printf(TEXT("test-recording-%s.webm"), *TestVideoRun->GetName()));
	TestEqual(TEXT("Unfinished, it has .part added"), First->GetUnfinishedVideoFilePath(), First->GetVideoFilePath() + TEXT(".part"));
	FString Reserved;
	FFileHelper::LoadFileToString(Reserved, *FPaths::Combine(First->GetFolder(), FProtokitePlaytestRecordingsFolder::ReservedBytesFileName));
	TestEqual(TEXT("How large its video may grow is saved in its folder"), Reserved, FString(TEXT("12345")));

	const FString FirstFolder = First->GetFolder();
	TestFalse(TEXT("A run still going cannot be claimed"), FProtokitePlaytestRecordingRun::ClaimEnded(FirstFolder).IsValid());

	const FProtokitePlaytestRecordingSession Session = MakeRecordingsTestSession(TEXT("01KX0RECORDINGSESSION00001"));
	TestTrue(TEXT("Its session is saved"), First->SaveSession(Session, Error));
	const FProtokitePlaytestRecordingSession Loaded = First->LoadSession();
	TestEqual(TEXT("The session id reads back"), Loaded.ProtokiteSessionId, Session.ProtokiteSessionId);
	TestEqual(TEXT("The address reads back"), Loaded.ProtokiteApiUrl, Session.ProtokiteApiUrl);
	TestEqual(TEXT("The Game Version ID reads back"), Loaded.FlockGameVersionId, Session.FlockGameVersionId);
	FString Text;
	FFileHelper::LoadFileToString(Text, *FPaths::Combine(FirstFolder, FProtokitePlaytestRecordingsFolder::SessionFileName));
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	TestTrue(TEXT("Saved as JSON under plain names"), FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid()
		&& Object->HasField(TEXT("protokite_session_id")) && Object->HasField(TEXT("protokite_api_url")) && Object->HasField(TEXT("flock_game_version_id")));

	First.Reset();
	const TSharedPtr<FProtokitePlaytestRecordingRun> Claimed = FProtokitePlaytestRecordingRun::ClaimEnded(FirstFolder);
	if (TestTrue(TEXT("Once its launch lets go, it can be claimed"), Claimed.IsValid()))
	{
		TestTrue(TEXT("As a playtest recording's run"), Claimed->GetKind() == EProtokitePlaytestRecordingKind::Playtest);
		TestEqual(TEXT("Under its own name"), Claimed->GetName(), Name);
		TestEqual(TEXT("With its session"), Claimed->LoadSession().ProtokiteSessionId, Session.ProtokiteSessionId);
	}
	TestFalse(TEXT("By one launch at a time"), FProtokitePlaytestRecordingRun::ClaimEnded(FirstFolder).IsValid());

	const FString Elsewhere = FPaths::Combine(Test.Recordings, TEXT("Elsewhere"), TEXT("20200101-000000-aaaaaaaa"));
	TestTrue(TEXT("Precondition: a lock is saved outside Playtest and TestVideos"),
		SaveRecordingsTestFile(FPaths::Combine(Elsewhere, FProtokitePlaytestRecordingsFolder::LockFileName), TArray<uint8>()));
	TestFalse(TEXT("A folder outside Playtest and TestVideos is no run, lock or not"), FProtokitePlaytestRecordingRun::ClaimEnded(Elsewhere).IsValid());
	const FString NoLock = FPaths::Combine(FProtokitePlaytestRecordingsFolder::GetKindFolder(Test.Recordings, EProtokitePlaytestRecordingKind::Playtest), TEXT("no-lock"));
	IFileManager::Get().MakeDirectory(*NoLock, /*Tree*/ true);
	TestFalse(TEXT("Nor is a folder with no lock"), FProtokitePlaytestRecordingRun::ClaimEnded(NoLock).IsValid());
	TestFalse(TEXT("Claiming that folder did not make it a run"), IFileManager::Get().FileExists(*FPaths::Combine(NoLock, FProtokitePlaytestRecordingsFolder::LockFileName)));

	FString CreateError;
	TestFalse(TEXT("A folder that cannot be made gives no run"),
		FProtokitePlaytestRecordingRun::Create(FPaths::Combine(Test.Folder, TEXT("not|a|folder")), EProtokitePlaytestRecordingKind::Playtest, 0, CreateError).IsValid());
	TestFalse(TEXT("And says why"), CreateError.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestRecordingsDefaultFolderTest,
	"Protokite.Playtest.Recordings.DefaultFolderIsUnderSaved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestRecordingsDefaultFolderTest::RunTest(const FString& Parameters)
{
	const FString Default = FProtokitePlaytestRecordingsFolder::GetDefaultPath();
	TestTrue(FString::Printf(TEXT("Saved/ProtokitePlaytest/Recordings under the project (%s)"), *Default),
		FPaths::IsSamePath(Default, FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ProtokitePlaytest"), TEXT("Recordings"))));
	TestFalse(TEXT("As a full path"), FPaths::IsRelative(Default));
	TestEqual(TEXT("Playtest recordings under Playtest"),
		FPaths::GetCleanFilename(FProtokitePlaytestRecordingsFolder::GetKindFolder(Default, EProtokitePlaytestRecordingKind::Playtest)), FString(TEXT("Playtest")));
	TestEqual(TEXT("Test videos under TestVideos"),
		FPaths::GetCleanFilename(FProtokitePlaytestRecordingsFolder::GetKindFolder(Default, EProtokitePlaytestRecordingKind::TestVideo)), FString(TEXT("TestVideos")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestRecordingsKeepsARecordingWaitingToUploadTest,
	"Protokite.Playtest.Recordings.KeepsAPlaytestRecordingWaitingToUpload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestRecordingsKeepsARecordingWaitingToUploadTest::RunTest(const FString& Parameters)
{
	FRecordingsTestFolder Test;
	const TArray<uint8> Video = MakeRecordingsTestVideoBytes(3, 100, 3);
	const FProtokitePlaytestRecordingSession Session = MakeRecordingsTestSession(TEXT("01KX0RECORDINGSESSION00001"));
	const FString RunFolder = MakeEndedRecordingRun(*this, Test.Recordings, EProtokitePlaytestRecordingKind::Playtest, TEXT("20260915-100000-aaaaaaaa"),
		&Video, nullptr, &Session);
	// What a crash between writing a session file and moving it into place leaves.
	const FString StrayTemporaryFile = FPaths::Combine(RunFolder, TEXT("session.json.0123456789ABCDEF0123456789ABCDEF.tmp"));
	TestTrue(TEXT("Precondition: a stray temporary file is saved"), SaveRecordingsTestFile(StrayTemporaryFile, Video));

	const FProtokitePlaytestWhatEndedRunsLeft Result = FProtokitePlaytestRecordingsFolder::FinishWhatEndedRunsLeft(Test.Recordings);
	TestEqual(TEXT("Kept, waiting to be uploaded"), Result.RecordingsWaitingToUpload, 1);
	TestEqual(TEXT("Nothing deleted"), Result.RecordingsWithoutASessionDeleted, 0);
	TestEqual(TEXT("Nothing to finish"), Result.InterruptedRecordingsFinished, 0);
	TestEqual(TEXT("Nothing left for the next launch"), Result.FilesLeftForTheNextLaunch.Num(), 0);
	TestFalse(TEXT("The stray temporary file is deleted"), IFileManager::Get().FileExists(*StrayTemporaryFile));

	const TArray<FProtokitePlaytestRecordingWaitingToUpload> Waiting = FProtokitePlaytestRecordingsFolder::FindRecordingsWaitingToUpload(Test.Recordings);
	if (TestEqual(TEXT("One recording waits to be uploaded"), Waiting.Num(), 1))
	{
		TestTrue(TEXT("In its run's folder"), FPaths::IsSamePath(Waiting[0].RunFolder, RunFolder));
		TestEqual(TEXT("Named by its run"), FPaths::GetCleanFilename(Waiting[0].VideoFilePath), FString(TEXT("recording-20260915-100000-aaaaaaaa.webm")));
		TestEqual(TEXT("Unchanged"), Waiting[0].VideoBytes, static_cast<int64>(Video.Num()));
		TestEqual(TEXT("To the session it belongs to"), Waiting[0].Session.ProtokiteSessionId, Session.ProtokiteSessionId);
		TestEqual(TEXT("At the address that session started at"), Waiting[0].Session.ProtokiteApiUrl, Session.ProtokiteApiUrl);
		TestEqual(TEXT("Under the Game Version ID it started with"), Waiting[0].Session.FlockGameVersionId, Session.FlockGameVersionId);
	}
	TestEqual(TEXT("And kept again by the launch after"), FProtokitePlaytestRecordingsFolder::FinishWhatEndedRunsLeft(Test.Recordings).RecordingsWaitingToUpload, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestRecordingsDeletesARecordingNoSessionStartedForTest,
	"Protokite.Playtest.Recordings.DeletesAPlaytestRecordingNoSessionStartedFor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestRecordingsDeletesARecordingNoSessionStartedForTest::RunTest(const FString& Parameters)
{
	FRecordingsTestFolder Test;
	const TArray<uint8> Finished = MakeRecordingsTestVideoBytes(3, 100, 3);
	const TArray<uint8> Unfinished = MakeRecordingsTestVideoBytes(3, 100, 0);
	const FString FinishedRun = MakeEndedRecordingRun(*this, Test.Recordings, EProtokitePlaytestRecordingKind::Playtest, TEXT("20260101-000000-aaaaaaaa"),
		&Finished, nullptr, nullptr);
	const FString UnfinishedRun = MakeEndedRecordingRun(*this, Test.Recordings, EProtokitePlaytestRecordingKind::Playtest, TEXT("20260101-000001-bbbbbbbb"),
		nullptr, &Unfinished, nullptr);
	const FString EmptyPlaytestRun = MakeEndedRecordingRun(*this, Test.Recordings, EProtokitePlaytestRecordingKind::Playtest, TEXT("20260101-000002-cccccccc"),
		nullptr, nullptr, nullptr);
	const FString EmptyTestVideoRun = MakeEndedRecordingRun(*this, Test.Recordings, EProtokitePlaytestRecordingKind::TestVideo, TEXT("20260101-000003-dddddddd"),
		nullptr, nullptr, nullptr);

	const FProtokitePlaytestWhatEndedRunsLeft Result = FProtokitePlaytestRecordingsFolder::FinishWhatEndedRunsLeft(Test.Recordings);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestRecordingsFinishesAnInterruptedRecordingTest,
	"Protokite.Playtest.Recordings.FinishesARecordingCutOffWhenItsGameEnded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestRecordingsFinishesAnInterruptedRecordingTest::RunTest(const FString& Parameters)
{
	FRecordingsTestFolder Test;
	const FProtokitePlaytestRecordingSession Session = MakeRecordingsTestSession(TEXT("01KX0RECORDINGSESSION00001"));

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

	const FString PlaytestRun = MakeEndedRecordingRun(*this, Test.Recordings, EProtokitePlaytestRecordingKind::Playtest, TEXT("20260101-000000-aaaaaaaa"),
		nullptr, &CutOffPlaytest, &Session);
	const FString TestVideoRun = MakeEndedRecordingRun(*this, Test.Recordings, EProtokitePlaytestRecordingKind::TestVideo, TEXT("20260101-000001-bbbbbbbb"),
		nullptr, &CutOffTestVideo, nullptr);
	const FString HeaderOnlyRun = MakeEndedRecordingRun(*this, Test.Recordings, EProtokitePlaytestRecordingKind::TestVideo, TEXT("20260101-000002-cccccccc"),
		nullptr, &HeaderOnly, nullptr);
	const FString NotAVideoRun = MakeEndedRecordingRun(*this, Test.Recordings, EProtokitePlaytestRecordingKind::Playtest, TEXT("20260101-000003-dddddddd"),
		nullptr, &NotAVideo, &Session);
	TestEqual(TEXT("A cut-off recording is not listed for upload before a launch finishes it"),
		FProtokitePlaytestRecordingsFolder::FindRecordingsWaitingToUpload(Test.Recordings).Num(), 0);

	const FProtokitePlaytestWhatEndedRunsLeft Result = FProtokitePlaytestRecordingsFolder::FinishWhatEndedRunsLeft(Test.Recordings);
	TestEqual(TEXT("The two with whole frames are finished"), Result.InterruptedRecordingsFinished, 2);
	TestEqual(TEXT("The playtest one waits to be uploaded"), Result.RecordingsWaitingToUpload, 1);
	TestEqual(TEXT("Nothing is deleted for want of a session"), Result.RecordingsWithoutASessionDeleted, 0);
	TestEqual(TEXT("Nothing left for the next launch"), Result.FilesLeftForTheNextLaunch.Num(), 0);

	const FString PlaytestVideo = FPaths::Combine(PlaytestRun, TEXT("recording-20260101-000000-aaaaaaaa.webm"));
	TestFalse(TEXT("No unfinished playtest file is left"), IFileManager::Get().FileExists(*(PlaytestVideo + TEXT(".part"))));
	FVideoFileRead PlaytestFile;
	if (TestTrue(TEXT("The finished playtest recording reads back"), ReadVideoFile(PlaytestVideo, PlaytestFile)))
	{
		TestEqual(TEXT("With its five whole frames"), PlaytestFile.Frames.Num(), 5);
		TestTrue(TEXT("With its segment's size stamped in"), PlaytestFile.bSegmentSizeWritten);
		TestEqual(TEXT("And the frame cut off removed"), PlaytestFile.FileBytes,
			FProtokitePlaytestVideoFile::FileHeaderBytes + 5 * (FProtokitePlaytestVideoFile::FrameHeaderBytes + 100));
		if (PlaytestFile.Frames.Num() == 5)
		{
			TestTrue(TEXT("The last whole frame is unchanged"), PlaytestFile.Frames[4].Bytes == MakeRecordingsTestFrame(4, 100));
		}
	}
	TestEqual(TEXT("Once finished, it is listed for upload"), FProtokitePlaytestRecordingsFolder::FindRecordingsWaitingToUpload(Test.Recordings).Num(), 1);

	FVideoFileRead TestVideoFile;
	if (TestTrue(TEXT("The finished test video reads back"),
		ReadVideoFile(FPaths::Combine(TestVideoRun, TEXT("test-recording-20260101-000001-bbbbbbbb.webm")), TestVideoFile)))
	{
		TestEqual(TEXT("With its three whole frames"), TestVideoFile.Frames.Num(), 3);
		TestTrue(TEXT("With its segment's size stamped in"), TestVideoFile.bSegmentSizeWritten);
	}
	TestFalse(TEXT("A video holding no frame is removed with its run"), RecordingsTestFolderExists(HeaderOnlyRun));
	TestFalse(TEXT("So is a file that is no video"), RecordingsTestFolderExists(NotAVideoRun));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestRecordingsLeavesRunsInUseAloneTest,
	"Protokite.Playtest.Recordings.LeavesRunsStillInUseAlone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestRecordingsLeavesRunsInUseAloneTest::RunTest(const FString& Parameters)
{
	FRecordingsTestFolder Test;
	FString Error;
	const TArray<uint8> Unfinished = MakeRecordingsTestVideoBytes(3, 100, 0);
	const TArray<uint8> Finished = MakeRecordingsTestVideoBytes(3, 100, 3);
	const FProtokitePlaytestRecordingSession Session = MakeRecordingsTestSession(TEXT("01KX0RECORDINGSESSION00001"));

	// A game still recording for its playtest, with no session yet: once ended, it would be deleted.
	TSharedPtr<FProtokitePlaytestRecordingRun> RunningPlaytest = FProtokitePlaytestRecordingRun::CreateNamedForTesting(Test.Recordings,
		EProtokitePlaytestRecordingKind::Playtest, TEXT("20260101-000000-aaaaaaaa"), /*ReservedBytes*/ 0, Error);
	// A game still recording a test video: once ended, it would be finished.
	TSharedPtr<FProtokitePlaytestRecordingRun> RunningTestVideo = FProtokitePlaytestRecordingRun::CreateNamedForTesting(Test.Recordings,
		EProtokitePlaytestRecordingKind::TestVideo, TEXT("20260101-000001-bbbbbbbb"), /*ReservedBytes*/ 0, Error);
	if (!TestTrue(TEXT("Precondition: the running games' runs are made"), RunningPlaytest.IsValid() && RunningTestVideo.IsValid()))
	{
		return true;
	}
	SaveRecordingsTestFile(RunningPlaytest->GetUnfinishedVideoFilePath(), Unfinished);
	SaveRecordingsTestFile(RunningTestVideo->GetUnfinishedVideoFilePath(), Unfinished);
	const FString RunningPlaytestVideo = RunningPlaytest->GetUnfinishedVideoFilePath();
	const FString RunningTestVideoVideo = RunningTestVideo->GetUnfinishedVideoFilePath();

	// An ended run another launch has claimed, to upload it.
	const FString HeldFolder = MakeEndedRecordingRun(*this, Test.Recordings, EProtokitePlaytestRecordingKind::Playtest, TEXT("20260101-000002-cccccccc"),
		&Finished, nullptr, &Session);
	TSharedPtr<FProtokitePlaytestRecordingRun> HeldByAnotherLaunch = FProtokitePlaytestRecordingRun::ClaimEnded(HeldFolder);
	if (!TestTrue(TEXT("Precondition: another launch holds the ended run"), HeldByAnotherLaunch.IsValid()))
	{
		return true;
	}
	TestFalse(TEXT("Precondition: a running game's run cannot be claimed"), FProtokitePlaytestRecordingRun::ClaimEnded(RunningPlaytest->GetFolder()).IsValid());

	const FProtokitePlaytestWhatEndedRunsLeft WhileInUse = FProtokitePlaytestRecordingsFolder::FinishWhatEndedRunsLeft(Test.Recordings);
	TestEqual(TEXT("Nothing is deleted"), WhileInUse.RecordingsWithoutASessionDeleted, 0);
	TestEqual(TEXT("Nothing is finished"), WhileInUse.InterruptedRecordingsFinished, 0);
	TestEqual(TEXT("Nothing is counted as waiting"), WhileInUse.RecordingsWaitingToUpload, 0);
	TestEqual(TEXT("The running playtest recording is untouched"), IFileManager::Get().FileSize(*RunningPlaytestVideo), static_cast<int64>(Unfinished.Num()));
	TestEqual(TEXT("The running test video is untouched"), IFileManager::Get().FileSize(*RunningTestVideoVideo), static_cast<int64>(Unfinished.Num()));
	TestEqual(TEXT("A run another launch holds is not listed for upload"),
		FProtokitePlaytestRecordingsFolder::FindRecordingsWaitingToUpload(Test.Recordings).Num(), 0);
	const FProtokitePlaytestRecordingsRoom Room = FProtokitePlaytestRecordingsFolder::MakeRoom(Test.Recordings, /*BudgetBytes*/ 1, /*BytesWanted*/ 1024 * 1024);
	TestEqual(TEXT("Making room deletes no playtest recording in use"), Room.PlaytestRecordingsDeleted.Num(), 0);
	TestEqual(TEXT("And no test video in use"), Room.TestVideosDeleted.Num(), 0);

	// Once every launch has let go, the next one sees to them.
	RunningPlaytest.Reset();
	RunningTestVideo.Reset();
	HeldByAnotherLaunch.Reset();
	const FProtokitePlaytestWhatEndedRunsLeft Afterwards = FProtokitePlaytestRecordingsFolder::FinishWhatEndedRunsLeft(Test.Recordings);
	TestEqual(TEXT("Control: the playtest recording no session started for is deleted"), Afterwards.RecordingsWithoutASessionDeleted, 1);
	TestEqual(TEXT("Control: the test video is finished"), Afterwards.InterruptedRecordingsFinished, 1);
	TestEqual(TEXT("Control: the recording the other launch held waits to be uploaded"), Afterwards.RecordingsWaitingToUpload, 1);
	TestEqual(TEXT("Control: and is listed"), FProtokitePlaytestRecordingsFolder::FindRecordingsWaitingToUpload(Test.Recordings).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestRecordingsLeavesAFileInUseForTheNextLaunchTest,
	"Protokite.Playtest.Recordings.LeavesAFileAnotherProgramHasOpenForTheNextLaunch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestRecordingsLeavesAFileInUseForTheNextLaunchTest::RunTest(const FString& Parameters)
{
	FRecordingsTestFolder Test;
	const TArray<uint8> Finished = MakeRecordingsTestVideoBytes(3, 100, 3);
	const TArray<uint8> Unfinished = MakeRecordingsTestVideoBytes(3, 100, 0);
	const FString NoSessionRun = MakeEndedRecordingRun(*this, Test.Recordings, EProtokitePlaytestRecordingKind::Playtest, TEXT("20260101-000000-aaaaaaaa"),
		&Finished, nullptr, nullptr);
	const FString CutOffRun = MakeEndedRecordingRun(*this, Test.Recordings, EProtokitePlaytestRecordingKind::TestVideo, TEXT("20260101-000001-bbbbbbbb"),
		nullptr, &Unfinished, nullptr);
	const FString NoSessionVideo = FPaths::Combine(NoSessionRun, TEXT("recording-20260101-000000-aaaaaaaa.webm"));
	const FString CutOffVideo = FPaths::Combine(CutOffRun, TEXT("test-recording-20260101-000001-bbbbbbbb.webm.part"));
	{
		// A video player has both open.
		const TUniquePtr<FArchive> Player(IFileManager::Get().CreateFileReader(*NoSessionVideo));
		const TUniquePtr<FArchive> OtherPlayer(IFileManager::Get().CreateFileReader(*CutOffVideo));
		if (!TestTrue(TEXT("Precondition: both files are open"), Player.IsValid() && OtherPlayer.IsValid()))
		{
			return true;
		}
		const FProtokitePlaytestWhatEndedRunsLeft WhileOpen = FProtokitePlaytestRecordingsFolder::FinishWhatEndedRunsLeft(Test.Recordings);
		TestEqual(TEXT("Both are left for the next launch"), WhileOpen.FilesLeftForTheNextLaunch.Num(), 2);
		TestEqual(TEXT("Neither counts as done"), WhileOpen.RecordingsWithoutASessionDeleted + WhileOpen.InterruptedRecordingsFinished, 0);
		TestTrue(TEXT("The run that could not be deleted is still a run the next launch finds"),
			IFileManager::Get().FileExists(*FPaths::Combine(NoSessionRun, FProtokitePlaytestRecordingsFolder::LockFileName)));
		TestEqual(TEXT("The cut-off file is left as it was"), IFileManager::Get().FileSize(*CutOffVideo), static_cast<int64>(Unfinished.Num()));
	}

	const FProtokitePlaytestWhatEndedRunsLeft Afterwards = FProtokitePlaytestRecordingsFolder::FinishWhatEndedRunsLeft(Test.Recordings);
	TestEqual(TEXT("Once the player lets go, the next launch deletes the one"), Afterwards.RecordingsWithoutASessionDeleted, 1);
	TestFalse(TEXT("With its folder"), RecordingsTestFolderExists(NoSessionRun));
	TestEqual(TEXT("And finishes the other"), Afterwards.InterruptedRecordingsFinished, 1);
	TestEqual(TEXT("Nothing is left"), Afterwards.FilesLeftForTheNextLaunch.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestRecordingsNeverTouchesAnythingElseTest,
	"Protokite.Playtest.Recordings.NeverTouchesAnythingElse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestRecordingsNeverTouchesAnythingElseTest::RunTest(const FString& Parameters)
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
		FPaths::Combine(Test.Recordings, TEXT("Playtest"), TEXT("copied-by-hand"), TEXT("recording-copied.webm")),
		FPaths::Combine(Test.Recordings, TEXT("Elsewhere"), TEXT("20200101-000000-aaaaaaaa"), FProtokitePlaytestRecordingsFolder::LockFileName),
		FPaths::Combine(Test.Recordings, TEXT("Elsewhere"), TEXT("20200101-000000-aaaaaaaa"), TEXT("recording-20200101-000000-aaaaaaaa.webm")),
	};
	for (const FString& Path : Others)
	{
		TestTrue(FString::Printf(TEXT("Precondition: %s is saved"), *Path), SaveRecordingsTestFile(Path, Bytes));
	}
	const FString EndedRun = MakeEndedRecordingRun(*this, Test.Recordings, EProtokitePlaytestRecordingKind::TestVideo, TEXT("20200101-000001-bbbbbbbb"),
		&Bytes, nullptr, nullptr);

	FProtokitePlaytestRecordingsFolder::FinishWhatEndedRunsLeft(Test.Recordings);
	FProtokitePlaytestRecordingsFolder::FindRecordingsWaitingToUpload(Test.Recordings);
	const FProtokitePlaytestRecordingsRoom Room = FProtokitePlaytestRecordingsFolder::MakeRoom(Test.Recordings, /*BudgetBytes*/ 1, /*BytesWanted*/ 1024 * 1024);
	TestFalse(TEXT("Control: the ended run was deleted to make room"), RecordingsTestFolderExists(EndedRun));
	TestEqual(TEXT("Nothing that is not a run's is counted"), Room.BytesUsed, 0LL);
	for (const FString& Path : Others)
	{
		TestEqual(FString::Printf(TEXT("%s is untouched"), *Path), IFileManager::Get().FileSize(*Path), static_cast<int64>(Bytes.Num()));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestRecordingsMakeRoomDeletesTestVideosFirstTest,
	"Protokite.Playtest.Recordings.MakingRoomDeletesTestVideosFirstThenTheOldestUpload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestRecordingsMakeRoomDeletesTestVideosFirstTest::RunTest(const FString& Parameters)
{
	FRecordingsTestFolder Test;
	const TArray<uint8> Video = MakeRecordingsTestVideoBytes(1, 100 * 1024, 1);
	const FProtokitePlaytestRecordingSession Session = MakeRecordingsTestSession(TEXT("01KX0RECORDINGSESSION00001"));
	// Made newest first, so the files' times run the other way from the runs' names. The oldest run is a playtest recording.
	const FString NewestTestVideo = MakeEndedRecordingRun(*this, Test.Recordings, EProtokitePlaytestRecordingKind::TestVideo, TEXT("20260101-000003-dddddddd"),
		&Video, nullptr, nullptr);
	const FString NewerUpload = MakeEndedRecordingRun(*this, Test.Recordings, EProtokitePlaytestRecordingKind::Playtest, TEXT("20260101-000002-cccccccc"),
		&Video, nullptr, &Session);
	const FString OlderTestVideo = MakeEndedRecordingRun(*this, Test.Recordings, EProtokitePlaytestRecordingKind::TestVideo, TEXT("20260101-000001-bbbbbbbb"),
		&Video, nullptr, nullptr);
	const FString OldestUpload = MakeEndedRecordingRun(*this, Test.Recordings, EProtokitePlaytestRecordingKind::Playtest, TEXT("20260101-000000-aaaaaaaa"),
		&Video, nullptr, &Session);
	const int64 NewestTestVideoBytes = RecordingsTestBytesIn(NewestTestVideo);
	const int64 NewerUploadBytes = RecordingsTestBytesIn(NewerUpload);
	const int64 OlderTestVideoBytes = RecordingsTestBytesIn(OlderTestVideo);
	const int64 OldestUploadBytes = RecordingsTestBytesIn(OldestUpload);
	const int64 Wanted = 50 * 1024;

	FProtokitePlaytestRecordingsRoom Room = FProtokitePlaytestRecordingsFolder::MakeRoom(Test.Recordings,
		NewestTestVideoBytes + NewerUploadBytes + OlderTestVideoBytes + OldestUploadBytes + Wanted, Wanted);
	TestEqual(TEXT("With room for everything, nothing is deleted"), Room.TestVideosDeleted.Num() + Room.PlaytestRecordingsDeleted.Num(), 0);
	TestEqual(TEXT("Everything is counted"), Room.BytesUsed, NewestTestVideoBytes + NewerUploadBytes + OlderTestVideoBytes + OldestUploadBytes);
	TestEqual(TEXT("And the budget has room left for the new recording"), Room.BytesLeftInBudget, Wanted);

	// Room for both uploads and the new recording: both test videos go, oldest first, and no upload, though one is older.
	Room = FProtokitePlaytestRecordingsFolder::MakeRoom(Test.Recordings, NewerUploadBytes + OldestUploadBytes + Wanted, Wanted);
	if (TestEqual(TEXT("Both test videos are deleted"), Room.TestVideosDeleted.Num(), 2))
	{
		TestTrue(TEXT("The older one first"), FPaths::IsSamePath(FPaths::GetPath(Room.TestVideosDeleted[0]), OlderTestVideo));
		TestTrue(TEXT("Then the newest"), FPaths::IsSamePath(FPaths::GetPath(Room.TestVideosDeleted[1]), NewestTestVideo));
	}
	TestEqual(TEXT("No recording waiting to be uploaded is deleted while test videos can go"), Room.PlaytestRecordingsDeleted.Num(), 0);
	TestTrue(TEXT("The oldest upload is kept"), RecordingsTestFolderExists(OldestUpload));
	TestEqual(TEXT("And the new recording fits"), Room.BytesLeftInBudget, Wanted);

	// One byte short of keeping both uploads: only then does one go, the oldest.
	Room = FProtokitePlaytestRecordingsFolder::MakeRoom(Test.Recordings, NewerUploadBytes + OldestUploadBytes + Wanted - 1, Wanted);
	if (TestEqual(TEXT("One recording waiting to be uploaded is deleted"), Room.PlaytestRecordingsDeleted.Num(), 1))
	{
		TestTrue(TEXT("The oldest"), FPaths::IsSamePath(FPaths::GetPath(Room.PlaytestRecordingsDeleted[0]), OldestUpload));
	}
	TestTrue(TEXT("The newer upload is kept"), RecordingsTestFolderExists(NewerUpload));
	TestEqual(TEXT("What is left is counted"), Room.BytesUsed, NewerUploadBytes);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestRecordingsMakeRoomCountsRunsInUseTest,
	"Protokite.Playtest.Recordings.MakingRoomCountsRunsInUseAndNeverDeletesThem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestRecordingsMakeRoomCountsRunsInUseTest::RunTest(const FString& Parameters)
{
	FRecordingsTestFolder Test;
	FString Error;
	// The older run belongs to a game still recording: 200 KB so far, of the 400 KB it may grow to.
	const int64 Reserved = 400 * 1024;
	const TSharedPtr<FProtokitePlaytestRecordingRun> StillRunning = FProtokitePlaytestRecordingRun::CreateNamedForTesting(Test.Recordings,
		EProtokitePlaytestRecordingKind::TestVideo, TEXT("20260101-000000-aaaaaaaa"), Reserved, Error);
	if (!TestTrue(TEXT("Precondition: the running game's run is made"), StillRunning.IsValid()))
	{
		return true;
	}
	const TArray<uint8> RunningVideo = MakeRecordingsTestVideoBytes(1, 200 * 1024, 0);
	SaveRecordingsTestFile(StillRunning->GetUnfinishedVideoFilePath(), RunningVideo);
	const TArray<uint8> Video = MakeRecordingsTestVideoBytes(1, 100 * 1024, 1);
	const FString Ended = MakeEndedRecordingRun(*this, Test.Recordings, EProtokitePlaytestRecordingKind::TestVideo, TEXT("20260101-000001-bbbbbbbb"),
		&Video, nullptr, nullptr);
	const int64 Wanted = 100 * 1024;

	FProtokitePlaytestRecordingsRoom Room = FProtokitePlaytestRecordingsFolder::MakeRoom(Test.Recordings, Reserved + 50 * 1024, Wanted);
	TestEqual(TEXT("The ended run is deleted, although the running one is older"), Room.TestVideosDeleted.Num(), 1);
	TestFalse(TEXT("Its folder is gone"), RecordingsTestFolderExists(Ended));
	TestEqual(TEXT("The running game's recording is untouched"), IFileManager::Get().FileSize(*StillRunning->GetUnfinishedVideoFilePath()),
		static_cast<int64>(RunningVideo.Num()));
	TestEqual(TEXT("And counted at the size it may grow to, not the size it has"), Room.BytesUsed, Reserved);
	TestEqual(TEXT("The budget has what is left after that"), Room.BytesLeftInBudget, 50LL * 1024);

	Room = FProtokitePlaytestRecordingsFolder::MakeRoom(Test.Recordings, Reserved - 1, Wanted);
	TestEqual(TEXT("With a running game's recording filling the budget, nothing is left"), Room.BytesLeftInBudget, 0LL);
	TestTrue(TEXT("And it is still not deleted"), IFileManager::Get().FileExists(*StillRunning->GetUnfinishedVideoFilePath()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestVideoFileFinishesAnInterruptedFileTest,
	"Protokite.Playtest.Video.File.FinishesAFileCutOffPartWay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestVideoFileFinishesAnInterruptedFileTest::RunTest(const FString& Parameters)
{
	FRecordingsTestFolder Test;

	struct FCutOffCase
	{
		const TCHAR* What;
		TArray<uint8> Bytes;
		EProtokitePlaytestInterruptedVideoResult Expected;
		int32 WholeFrames;
	};
	TArray<FCutOffCase> Cases;
	Cases.Add({ TEXT("Whole frames with no count yet"), MakeRecordingsTestVideoBytes(4, 100, 0), EProtokitePlaytestInterruptedVideoResult::Finished, 4 });
	{
		TArray<uint8> Bytes = MakeRecordingsTestVideoBytes(4, 100, 0);
		AppendRecordingsTestFrameHeader(Bytes, 4, 100);
		Bytes.Append(MakeRecordingsTestFrame(4, 60));
		Cases.Add({ TEXT("A frame only partly written"), MoveTemp(Bytes), EProtokitePlaytestInterruptedVideoResult::Finished, 4 });
	}
	{
		TArray<uint8> Bytes = MakeRecordingsTestVideoBytes(4, 100, 0);
		Bytes.Append({ 100, 0, 0, 0, 16 });
		Cases.Add({ TEXT("A frame header only partly written"), MoveTemp(Bytes), EProtokitePlaytestInterruptedVideoResult::Finished, 4 });
	}
	{
		TArray<uint8> Bytes = MakeRecordingsTestVideoBytes(2, 100, 0);
		Bytes.AddZeroed(64);
		Cases.Add({ TEXT("Zeros after the last whole frame"), MoveTemp(Bytes), EProtokitePlaytestInterruptedVideoResult::Finished, 2 });
	}
	{
		// The frame's header was written, and its bytes were never: the disk kept the length and filled it with zeros.
		TArray<uint8> Bytes = MakeRecordingsTestVideoBytes(3, 100, 0);
		AppendRecordingsTestFrameHeader(Bytes, 3, 100);
		Bytes.AddZeroed(100);
		Cases.Add({ TEXT("A whole-sized frame of zeros"), MoveTemp(Bytes), EProtokitePlaytestInterruptedVideoResult::Finished, 3 });
	}
	{
		// A frame header that says zero bytes, then a whole frame whose size's first byte (130) looks like a VP9 frame's start.
		TArray<uint8> Bytes = MakeRecordingsTestVideoBytes(2, 100, 0);
		AppendRecordingsTestFrameHeader(Bytes, 2, 0);
		AppendRecordingsTestFrameHeader(Bytes, 3, 130);
		Bytes.Append(MakeRecordingsTestFrame(3, 130));
		Cases.Add({ TEXT("A frame of zero bytes"), MoveTemp(Bytes), EProtokitePlaytestInterruptedVideoResult::Finished, 2 });
	}
	Cases.Add({ TEXT("A header and no frame"), MakeRecordingsTestVideoBytes(0, 0, 0), EProtokitePlaytestInterruptedVideoResult::HeldNoFrame, 0 });
	{
		TArray<uint8> Bytes;
		for (const TCHAR Character : FString(TEXT("Only some words, and no video header before them.")))
		{
			Bytes.Add(static_cast<uint8>(Character));
		}
		Cases.Add({ TEXT("No video at all"), MoveTemp(Bytes), EProtokitePlaytestInterruptedVideoResult::HeldNoFrame, 0 });
	}
	{
		TArray<uint8> Bytes = MakeRecordingsTestVideoBytes(2, 100, 0);
		Bytes[0] = 'R';
		Bytes[1] = 'I';
		Bytes[2] = 'F';
		Bytes[3] = 'F';
		Cases.Add({ TEXT("Frames behind another kind of file's header"), MoveTemp(Bytes), EProtokitePlaytestInterruptedVideoResult::HeldNoFrame, 0 });
	}
	Cases.Add({ TEXT("An empty file"), TArray<uint8>(), EProtokitePlaytestInterruptedVideoResult::HeldNoFrame, 0 });

	for (int32 Index = 0; Index < Cases.Num(); ++Index)
	{
		const FCutOffCase& Case = Cases[Index];
		const FString Unfinished = FPaths::Combine(Test.Folder, FString::Printf(TEXT("case-%d.webm.part"), Index));
		const FString Finished = FPaths::Combine(Test.Folder, FString::Printf(TEXT("case-%d.webm"), Index));
		if (!TestTrue(FString::Printf(TEXT("%s: precondition, the file is saved"), Case.What), SaveRecordingsTestFile(Unfinished, Case.Bytes)))
		{
			continue;
		}
		int32 FramesKept = -1;
		FString Error;
		const EProtokitePlaytestInterruptedVideoResult Result = FProtokitePlaytestVideoFile::FinishInterruptedFile(Unfinished, Finished, FramesKept, Error);
		TestEqual(FString::Printf(TEXT("%s: what became of it (%s)"), Case.What, *Error), static_cast<int32>(Result), static_cast<int32>(Case.Expected));
		TestFalse(FString::Printf(TEXT("%s: no unfinished file is left"), Case.What), IFileManager::Get().FileExists(*Unfinished));
		if (Case.Expected != EProtokitePlaytestInterruptedVideoResult::Finished)
		{
			TestFalse(FString::Printf(TEXT("%s: and no finished one is made"), Case.What), IFileManager::Get().FileExists(*Finished));
			continue;
		}
		TestEqual(FString::Printf(TEXT("%s: the frames kept"), Case.What), FramesKept, Case.WholeFrames);
		FVideoFileRead File;
		if (TestTrue(FString::Printf(TEXT("%s: the finished file reads back"), Case.What), ReadVideoFile(Finished, File)))
		{
			TestEqual(FString::Printf(TEXT("%s: every whole frame"), Case.What), File.Frames.Num(), Case.WholeFrames);
			TestTrue(FString::Printf(TEXT("%s: with its segment's size stamped in"), Case.What), File.bSegmentSizeWritten);
			TestEqual(FString::Printf(TEXT("%s: and nothing after them"), Case.What), File.FileBytes,
				FProtokitePlaytestVideoFile::FileHeaderBytes + Case.WholeFrames * (FProtokitePlaytestVideoFile::FrameHeaderBytes + 100));
		}
	}

	// A file another program has open is left as it was.
	const FString Held = FPaths::Combine(Test.Folder, TEXT("held.webm.part"));
	const TArray<uint8> HeldBytes = MakeRecordingsTestVideoBytes(4, 100, 0);
	SaveRecordingsTestFile(Held, HeldBytes);
	{
		const TUniquePtr<FArchive> Player(IFileManager::Get().CreateFileReader(*Held));
		if (TestTrue(TEXT("Precondition: the file is open"), Player.IsValid()))
		{
			int32 FramesKept = -1;
			FString Error;
			const EProtokitePlaytestInterruptedVideoResult Result = FProtokitePlaytestVideoFile::FinishInterruptedFile(Held,
				FPaths::Combine(Test.Folder, TEXT("held.webm")), FramesKept, Error);
			TestEqual(TEXT("A file another program has open is not finished"), static_cast<int32>(Result),
				static_cast<int32>(EProtokitePlaytestInterruptedVideoResult::CouldNotFinish));
			TestFalse(TEXT("And the reason is given"), Error.IsEmpty());
		}
	}
	TestEqual(TEXT("It is left as it was"), IFileManager::Get().FileSize(*Held), static_cast<int64>(HeldBytes.Num()));
	return true;
}

#endif
