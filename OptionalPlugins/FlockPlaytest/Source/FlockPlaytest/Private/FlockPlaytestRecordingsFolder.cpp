// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockPlaytestRecordingsFolder.h"

#include "Dom/JsonObject.h"
#include "FlockPlaytestVideoFile.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	const TCHAR* const RecordingSessionIdKey = TEXT("protokite_session_id");
	const TCHAR* const RecordingSessionApiUrlKey = TEXT("protokite_api_url");
	const TCHAR* const RecordingSessionGameVersionIdKey = TEXT("flock_game_version_id");

	const TCHAR* RecordingKindFolderName(EFlockPlaytestRecordingKind Kind)
	{
		return Kind == EFlockPlaytestRecordingKind::Playtest ? FFlockPlaytestRecordingsFolder::PlaytestFolderName
			: FFlockPlaytestRecordingsFolder::TestVideoFolderName;
	}

	const TCHAR* RecordingVideoFilePrefix(EFlockPlaytestRecordingKind Kind)
	{
		return Kind == EFlockPlaytestRecordingKind::Playtest ? TEXT("recording") : TEXT("test-recording");
	}

	/** The files directly in Folder, as names. */
	TArray<FString> RecordingFileNamesIn(const FString& Folder)
	{
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *FPaths::Combine(Folder, TEXT("*")), /*Files*/ true, /*Directories*/ false);
		return Names;
	}

	int64 RecordingBytesInFolder(const FString& Folder)
	{
		int64 Bytes = 0;
		for (const FString& Name : RecordingFileNamesIn(Folder))
		{
			Bytes += FMath::Max<int64>(0, IFileManager::Get().FileSize(*FPaths::Combine(Folder, Name)));
		}
		return Bytes;
	}

	/** How large a run said its video may grow; 0 when it said nothing readable. */
	int64 ReadRecordingReservedBytes(const FString& RunFolder)
	{
		const FString Path = FPaths::Combine(RunFolder, FFlockPlaytestRecordingsFolder::ReservedBytesFileName);
		FString Text;
		int64 Bytes = 0;
		if (IFileManager::Get().FileExists(*Path) && FFileHelper::LoadFileToString(Text, *Path) && LexTryParseString(Bytes, *Text.TrimStartAndEnd()))
		{
			return FMath::Max<int64>(0, Bytes);
		}
		return 0;
	}

	/** Every folder directly under the Playtest and TestVideos folders, oldest first by name, whichever kind it is. */
	TArray<FString> AllRecordingRunFolders(const FString& RecordingsFolder)
	{
		TArray<FString> Folders;
		for (const EFlockPlaytestRecordingKind Kind : { EFlockPlaytestRecordingKind::Playtest, EFlockPlaytestRecordingKind::TestVideo })
		{
			const FString KindFolder = FFlockPlaytestRecordingsFolder::GetKindFolder(RecordingsFolder, Kind);
			TArray<FString> Names;
			IFileManager::Get().FindFiles(Names, *FPaths::Combine(KindFolder, TEXT("*")), /*Files*/ false, /*Directories*/ true);
			for (const FString& Name : Names)
			{
				Folders.Add(FPaths::Combine(KindFolder, Name));
			}
		}
		Folders.Sort([](const FString& A, const FString& B)
		{
			const int32 ByName = FPaths::GetCleanFilename(A).Compare(FPaths::GetCleanFilename(B), ESearchCase::CaseSensitive);
			return ByName != 0 ? ByName < 0 : A.Compare(B, ESearchCase::CaseSensitive) < 0;
		});
		return Folders;
	}
}

FFlockPlaytestRecordingRun::FFlockPlaytestRecordingRun(const FString& InFolder, const FString& InName, EFlockPlaytestRecordingKind InKind,
	TUniquePtr<IFileHandle> InLock)
	: Folder(InFolder)
	, Name(InName)
	, Kind(InKind)
	, Lock(MoveTemp(InLock))
{
}

FFlockPlaytestRecordingRun::~FFlockPlaytestRecordingRun() = default;

TSharedPtr<FFlockPlaytestRecordingRun> FFlockPlaytestRecordingRun::Create(const FString& RecordingsFolder, EFlockPlaytestRecordingKind Kind,
	int64 ReservedBytes, FString& OutError)
{
	const FString RunName = FString::Printf(TEXT("%s-%s"), *FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S")),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower());
	return CreateWithName(RecordingsFolder, Kind, RunName, ReservedBytes, OutError);
}

TSharedPtr<FFlockPlaytestRecordingRun> FFlockPlaytestRecordingRun::CreateNamedForTesting(const FString& RecordingsFolder,
	EFlockPlaytestRecordingKind Kind, const FString& RunName, int64 ReservedBytes, FString& OutError)
{
	return CreateWithName(RecordingsFolder, Kind, RunName, ReservedBytes, OutError);
}

TSharedPtr<FFlockPlaytestRecordingRun> FFlockPlaytestRecordingRun::CreateWithName(const FString& RecordingsFolder, EFlockPlaytestRecordingKind Kind,
	const FString& RunName, int64 ReservedBytes, FString& OutError)
{
	const FString RunFolder = FPaths::ConvertRelativePathToFull(FPaths::Combine(FFlockPlaytestRecordingsFolder::GetKindFolder(RecordingsFolder, Kind), RunName));
	IFileManager::Get().MakeDirectory(*RunFolder, /*Tree*/ true);

	// Shared with nobody: while this handle is open no other launch can open the lock, so none treats the run as ended.
	TUniquePtr<IFileHandle> RunLock(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(
		*FPaths::Combine(RunFolder, FFlockPlaytestRecordingsFolder::LockFileName), /*bAppend*/ false, /*bAllowRead*/ false));
	if (!RunLock.IsValid())
	{
		OutError = FString::Printf(TEXT("the recording folder %s could not be made"), *RunFolder);
		return nullptr;
	}
	const FString ReservedBytesPath = FPaths::Combine(RunFolder, FFlockPlaytestRecordingsFolder::ReservedBytesFileName);
	if (!FFileHelper::SaveStringToFile(LexToString(ReservedBytes), *ReservedBytesPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("%s could not be written"), *ReservedBytesPath);
		return nullptr;
	}
	return MakeShareable(new FFlockPlaytestRecordingRun(RunFolder, RunName, Kind, MoveTemp(RunLock)));
}

TSharedPtr<FFlockPlaytestRecordingRun> FFlockPlaytestRecordingRun::ClaimEnded(const FString& RunFolder)
{
	const FString FullFolder = FPaths::ConvertRelativePathToFull(RunFolder);
	const FString KindFolderName = FPaths::GetCleanFilename(FPaths::GetPath(FullFolder));
	EFlockPlaytestRecordingKind Kind;
	// Folder names are compared the way the file system compares them.
	if (KindFolderName.Equals(FFlockPlaytestRecordingsFolder::PlaytestFolderName, ESearchCase::IgnoreCase))
	{
		Kind = EFlockPlaytestRecordingKind::Playtest;
	}
	else if (KindFolderName.Equals(FFlockPlaytestRecordingsFolder::TestVideoFolderName, ESearchCase::IgnoreCase))
	{
		Kind = EFlockPlaytestRecordingKind::TestVideo;
	}
	else
	{
		return nullptr;
	}

	// A folder with no lock was not made by a run, so it is left alone.
	const FString LockPath = FPaths::Combine(FullFolder, FFlockPlaytestRecordingsFolder::LockFileName);
	if (!IFileManager::Get().FileExists(*LockPath))
	{
		return nullptr;
	}
	// Opening the lock succeeds only once the run, or the launch, holding it has let go. Appending leaves the file as it is.
	TUniquePtr<IFileHandle> RunLock(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*LockPath, /*bAppend*/ true, /*bAllowRead*/ false));
	if (!RunLock.IsValid())
	{
		return nullptr;
	}
	return MakeShareable(new FFlockPlaytestRecordingRun(FullFolder, FPaths::GetCleanFilename(FullFolder), Kind, MoveTemp(RunLock)));
}

FString FFlockPlaytestRecordingRun::GetVideoFilePath() const
{
	return FPaths::Combine(Folder, FString::Printf(TEXT("%s-%s.webm"), RecordingVideoFilePrefix(Kind), *Name));
}

FString FFlockPlaytestRecordingRun::GetUnfinishedVideoFilePath() const
{
	return GetVideoFilePath() + TEXT(".part");
}

bool FFlockPlaytestRecordingRun::SaveSession(const FFlockPlaytestRecordingSession& Session, FString& OutError) const
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(RecordingSessionIdKey, Session.ProtokiteSessionId);
	Object->SetStringField(RecordingSessionApiUrlKey, Session.ProtokiteApiUrl);
	Object->SetStringField(RecordingSessionGameVersionIdKey, Session.FlockGameVersionId);
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Object, Writer);

	// Written beside the session file and moved over it, so a session file is never found half written. A crash between
	// the two leaves the temporary file, which the next launch deletes once the run has ended.
	const FString SessionPath = FPaths::Combine(Folder, FFlockPlaytestRecordingsFolder::SessionFileName);
	const FString TemporaryPath = FString::Printf(TEXT("%s.%s.tmp"), *SessionPath, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!FFileHelper::SaveStringToFile(Text, *TemporaryPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("%s could not be written"), *TemporaryPath);
		return false;
	}
	if (!IFileManager::Get().Move(*SessionPath, *TemporaryPath, /*Replace*/ true, /*EvenIfReadOnly*/ true, /*Attributes*/ false,
		/*bDoNotRetryOrError*/ true))
	{
		IFileManager::Get().Delete(*TemporaryPath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
		OutError = FString::Printf(TEXT("%s could not be saved"), *SessionPath);
		return false;
	}
	return true;
}

FFlockPlaytestRecordingSession FFlockPlaytestRecordingRun::LoadSession() const
{
	FFlockPlaytestRecordingSession Session;
	const FString SessionPath = FPaths::Combine(Folder, FFlockPlaytestRecordingsFolder::SessionFileName);
	FString Text;
	if (!IFileManager::Get().FileExists(*SessionPath) || !FFileHelper::LoadFileToString(Text, *SessionPath))
	{
		return Session;
	}
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return Session;
	}
	Object->TryGetStringField(RecordingSessionIdKey, Session.ProtokiteSessionId);
	Object->TryGetStringField(RecordingSessionApiUrlKey, Session.ProtokiteApiUrl);
	Object->TryGetStringField(RecordingSessionGameVersionIdKey, Session.FlockGameVersionId);
	return Session;
}

bool FFlockPlaytestRecordingRun::DeleteEverything(TArray<FString>& OutFilesLeft)
{
	IFileManager& FileManager = IFileManager::Get();
	bool bEveryFileDeleted = true;
	for (const FString& FileName : RecordingFileNamesIn(Folder))
	{
		if (FileName.Equals(FFlockPlaytestRecordingsFolder::LockFileName, ESearchCase::IgnoreCase))
		{
			continue;
		}
		const FString Path = FPaths::Combine(Folder, FileName);
		if (!FileManager.Delete(*Path, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true))
		{
			OutFilesLeft.Add(Path);
			bEveryFileDeleted = false;
		}
	}
	if (!bEveryFileDeleted)
	{
		return false;
	}
	// The lock goes last, so a run whose files could not all be deleted is still a run the next launch finds.
	Lock.Reset();
	FileManager.Delete(*FPaths::Combine(Folder, FFlockPlaytestRecordingsFolder::LockFileName), /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
	FileManager.DeleteDirectory(*Folder, /*RequireExists*/ false, /*Tree*/ false);
	return true;
}

FString FFlockPlaytestRecordingsFolder::GetDefaultPath()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FlockPlaytest"), TEXT("Recordings")));
}

FString FFlockPlaytestRecordingsFolder::GetKindFolder(const FString& RecordingsFolder, EFlockPlaytestRecordingKind Kind)
{
	return FPaths::Combine(RecordingsFolder, RecordingKindFolderName(Kind));
}

FFlockPlaytestWhatEndedRunsLeft FFlockPlaytestRecordingsFolder::FinishWhatEndedRunsLeft(const FString& RecordingsFolder)
{
	FFlockPlaytestWhatEndedRunsLeft Result;
	IFileManager& FileManager = IFileManager::Get();
	for (const FString& RunFolder : AllRecordingRunFolders(RecordingsFolder))
	{
		const TSharedPtr<FFlockPlaytestRecordingRun> Run = FFlockPlaytestRecordingRun::ClaimEnded(RunFolder);
		if (!Run.IsValid())
		{
			continue;
		}
		const FString VideoPath = Run->GetVideoFilePath();
		const FString UnfinishedPath = Run->GetUnfinishedVideoFilePath();
		const bool bForThePlaytest = Run->GetKind() == EFlockPlaytestRecordingKind::Playtest;

		// No session was started for it, so it can never be uploaded, and is not worth finishing.
		if (bForThePlaytest && Run->LoadSession().IsEmpty())
		{
			const bool bHadVideo = FileManager.FileExists(*VideoPath) || FileManager.FileExists(*UnfinishedPath);
			if (Run->DeleteEverything(Result.FilesLeftForTheNextLaunch) && bHadVideo)
			{
				++Result.RecordingsWithoutASessionDeleted;
			}
			continue;
		}

		// The run has ended, so nothing is still writing a temporary file in it: one is what a crash left.
		for (const FString& FileName : RecordingFileNamesIn(Run->GetFolder()))
		{
			if (FileName.EndsWith(TEXT(".tmp"), ESearchCase::IgnoreCase))
			{
				FileManager.Delete(*FPaths::Combine(Run->GetFolder(), FileName), /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
			}
		}

		if (FileManager.FileExists(*UnfinishedPath))
		{
			if (FileManager.FileExists(*VideoPath))
			{
				// A run makes one video, so an unfinished file beside the finished one is what renaming it left behind.
				if (!FileManager.Delete(*UnfinishedPath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true))
				{
					Result.FilesLeftForTheNextLaunch.Add(UnfinishedPath);
					continue;
				}
			}
			else
			{
				int32 FramesKept = 0;
				FString Error;
				const EFlockPlaytestInterruptedVideoResult Finished = FFlockPlaytestVideoFile::FinishInterruptedFile(UnfinishedPath, VideoPath, FramesKept, Error);
				if (Finished == EFlockPlaytestInterruptedVideoResult::CouldNotFinish)
				{
					Result.FilesLeftForTheNextLaunch.Add(UnfinishedPath);
					continue;
				}
				if (Finished == EFlockPlaytestInterruptedVideoResult::Finished)
				{
					++Result.InterruptedRecordingsFinished;
				}
			}
		}

		if (!FileManager.FileExists(*VideoPath))
		{
			// Nothing whole was recorded, so the run leaves nothing worth keeping.
			Run->DeleteEverything(Result.FilesLeftForTheNextLaunch);
			continue;
		}
		if (bForThePlaytest)
		{
			++Result.RecordingsWaitingToUpload;
		}
	}
	return Result;
}

TArray<FFlockPlaytestRecordingWaitingToUpload> FFlockPlaytestRecordingsFolder::FindRecordingsWaitingToUpload(const FString& RecordingsFolder)
{
	TArray<FFlockPlaytestRecordingWaitingToUpload> Waiting;
	IFileManager& FileManager = IFileManager::Get();
	for (const FString& RunFolder : AllRecordingRunFolders(RecordingsFolder))
	{
		const TSharedPtr<FFlockPlaytestRecordingRun> Run = FFlockPlaytestRecordingRun::ClaimEnded(RunFolder);
		if (!Run.IsValid() || Run->GetKind() != EFlockPlaytestRecordingKind::Playtest)
		{
			continue;
		}
		FFlockPlaytestRecordingWaitingToUpload Recording;
		Recording.Session = Run->LoadSession();
		Recording.VideoFilePath = Run->GetVideoFilePath();
		// Only a finished video is uploaded: an unfinished one is finished by a launch first.
		if (Recording.Session.IsEmpty() || !FileManager.FileExists(*Recording.VideoFilePath))
		{
			continue;
		}
		Recording.RunFolder = Run->GetFolder();
		Recording.VideoBytes = FileManager.FileSize(*Recording.VideoFilePath);
		Waiting.Add(MoveTemp(Recording));
	}
	return Waiting;
}

FFlockPlaytestRecordingsRoom FFlockPlaytestRecordingsFolder::MakeRoom(const FString& RecordingsFolder, int64 BudgetBytes, int64 BytesWanted)
{
	FFlockPlaytestRecordingsRoom Room;
	struct FEndedRecordingRun
	{
		FString Folder;
		int64 Bytes = 0;
	};
	TArray<FEndedRecordingRun> EndedTestVideos;
	TArray<FEndedRecordingRun> EndedPlaytestRecordings;
	IFileManager& FileManager = IFileManager::Get();
	for (const FString& RunFolder : AllRecordingRunFolders(RecordingsFolder))
	{
		// A folder a run did not make is neither counted nor deleted.
		if (!FileManager.FileExists(*FPaths::Combine(RunFolder, LockFileName)))
		{
			continue;
		}
		const int64 Bytes = RecordingBytesInFolder(RunFolder);
		// Held only for this moment, to tell a run that has ended from one still going. It is taken again to be deleted.
		const TSharedPtr<FFlockPlaytestRecordingRun> Run = FFlockPlaytestRecordingRun::ClaimEnded(RunFolder);
		if (!Run.IsValid())
		{
			// A game still recording is counted at the size its video may still grow to.
			Room.BytesUsed += FMath::Max(Bytes, ReadRecordingReservedBytes(RunFolder));
			continue;
		}
		Room.BytesUsed += Bytes;
		(Run->GetKind() == EFlockPlaytestRecordingKind::Playtest ? EndedPlaytestRecordings : EndedTestVideos).Add({ Run->GetFolder(), Bytes });
	}

	// Test videos go first; a recording waiting to be uploaded only when that is not enough. Oldest first within each: the
	// folders are in name order, and a run's name starts with the time it was made.
	for (const TArray<FEndedRecordingRun>* EndedRuns : { &EndedTestVideos, &EndedPlaytestRecordings })
	{
		for (const FEndedRecordingRun& Ended : *EndedRuns)
		{
			if (Room.BytesUsed + BytesWanted <= BudgetBytes)
			{
				break;
			}
			const TSharedPtr<FFlockPlaytestRecordingRun> Run = FFlockPlaytestRecordingRun::ClaimEnded(Ended.Folder);
			if (!Run.IsValid())
			{
				// Another launch has taken hold of it since.
				continue;
			}
			const FString VideoPath = Run->GetVideoFilePath();
			const bool bHadVideo = FileManager.FileExists(*VideoPath) || FileManager.FileExists(*Run->GetUnfinishedVideoFilePath());
			TArray<FString> FilesLeft;
			const bool bDeleted = Run->DeleteEverything(FilesLeft);
			Room.BytesUsed -= Ended.Bytes - (bDeleted ? 0 : RecordingBytesInFolder(Ended.Folder));
			if (bDeleted && bHadVideo)
			{
				(Run->GetKind() == EFlockPlaytestRecordingKind::Playtest ? Room.PlaytestRecordingsDeleted : Room.TestVideosDeleted).Add(VideoPath);
			}
		}
	}
	Room.BytesLeftInBudget = FMath::Max<int64>(0, BudgetBytes - Room.BytesUsed);
	return Room;
}
