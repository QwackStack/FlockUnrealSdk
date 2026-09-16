// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/FlockLaunchFolder.h"

#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

FFlockLaunchFolder::FFlockLaunchFolder(const FString& InPath, TUniquePtr<IFileHandle> InLock)
	: Path(InPath)
	, Lock(MoveTemp(InLock))
{
}

FFlockLaunchFolder::~FFlockLaunchFolder() = default;

TSharedPtr<FFlockLaunchFolder> FFlockLaunchFolder::Create(const FString& ParentFolder)
{
	const FString Name = FString::Printf(TEXT("%s-%s"), *FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S")),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower());
	const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::Combine(ParentFolder, Name));
	IFileManager::Get().MakeDirectory(*Folder, /*Tree*/ true);

	// Shared with nobody: while this handle is open no other launch can open the lock, so none takes the folder for ended.
	TUniquePtr<IFileHandle> FolderLock(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(
		*FPaths::Combine(Folder, LockFileName), /*bAppend*/ false, /*bAllowRead*/ false));
	if (!FolderLock.IsValid())
	{
		return nullptr;
	}
	return MakeShareable(new FFlockLaunchFolder(Folder, MoveTemp(FolderLock)));
}

TSharedPtr<FFlockLaunchFolder> FFlockLaunchFolder::ClaimEnded(const FString& Folder)
{
	const FString FullFolder = FPaths::ConvertRelativePathToFull(Folder);
	const FString LockPath = FPaths::Combine(FullFolder, LockFileName);
	// A folder with no lock was not made by a launch, or its launch already deleted everything but the folder.
	if (!IFileManager::Get().FileExists(*LockPath))
	{
		return nullptr;
	}
	// Opening the lock succeeds only once the launch holding it has let go. Appending leaves the file as it is.
	TUniquePtr<IFileHandle> FolderLock(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*LockPath, /*bAppend*/ true, /*bAllowRead*/ false));
	if (!FolderLock.IsValid())
	{
		return nullptr;
	}
	return MakeShareable(new FFlockLaunchFolder(FullFolder, MoveTemp(FolderLock)));
}

TArray<FString> FFlockLaunchFolder::FindFolders(const FString& ParentFolder)
{
	TArray<FString> Names;
	IFileManager::Get().FindFiles(Names, *FPaths::Combine(ParentFolder, TEXT("*")), /*Files*/ false, /*Directories*/ true);
	Names.Sort();
	TArray<FString> Folders;
	for (const FString& Name : Names)
	{
		Folders.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(ParentFolder, Name)));
	}
	return Folders;
}

bool FFlockLaunchFolder::DeleteEverything()
{
	IFileManager& FileManager = IFileManager::Get();

	TArray<FString> Files;
	FileManager.FindFiles(Files, *FPaths::Combine(Path, TEXT("*")), /*Files*/ true, /*Directories*/ false);
	for (const FString& File : Files)
	{
		if (!File.Equals(LockFileName, ESearchCase::IgnoreCase))
		{
			FileManager.Delete(*FPaths::Combine(Path, File), /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
		}
	}
	TArray<FString> Subfolders;
	FileManager.FindFiles(Subfolders, *FPaths::Combine(Path, TEXT("*")), /*Files*/ false, /*Directories*/ true);
	for (const FString& Subfolder : Subfolders)
	{
		FileManager.DeleteDirectory(*FPaths::Combine(Path, Subfolder), /*RequireExists*/ false, /*Tree*/ true);
	}

	// Whatever is still there keeps the lock file, so the folder is found again and nothing in it is forgotten.
	TArray<FString> Left;
	FileManager.FindFiles(Left, *FPaths::Combine(Path, TEXT("*")), /*Files*/ true, /*Directories*/ true);
	const bool bOnlyTheLockIsLeft = Left.Num() == 0 || (Left.Num() == 1 && Left[0].Equals(LockFileName, ESearchCase::IgnoreCase));
	Lock.Reset();
	if (!bOnlyTheLockIsLeft)
	{
		return false;
	}
	FileManager.Delete(*FPaths::Combine(Path, LockFileName), /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
	return FileManager.DeleteDirectory(*Path, /*RequireExists*/ false, /*Tree*/ false);
}
