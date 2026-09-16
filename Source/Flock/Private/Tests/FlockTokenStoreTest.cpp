// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockFileTokenStore.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/FlockTemporaryFiles.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/Support/FlockTemporaryFilesTestSupport.h"

namespace
{
	FString TestFilePath(const FString& Name)
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FlockTests"), Name);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockFileTokenStoreRoundtripTest, "Flock.Auth.TokenStore.Roundtrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockFileTokenStoreRoundtripTest::RunTest(const FString& Parameters)
{
	const FString Path = TestFilePath(TEXT("roundtrip.dat"));
	IFileManager::Get().Delete(*Path);
	FFlockFileTokenStore Store(Path, TEXT("test-game"));

	FFlockStoredTokens Tokens;
	Tokens.AccessToken = TEXT("access-1");
	Tokens.RefreshToken = TEXT("refresh-1");
	Tokens.AuthMethod = EFlockAuthMethod::Email;
	Store.Save(Tokens);

	TestTrue(TEXT("file exists"), IFileManager::Get().FileExists(*Path));

	// On-disk bytes must not contain the plaintext tokens.
	TArray<uint8> Raw;
	FFileHelper::LoadFileToArray(Raw, *Path);
	const FString RawAsString(Raw.Num(), reinterpret_cast<const ANSICHAR*>(Raw.GetData()));
	TestFalse(TEXT("access token not plaintext"), RawAsString.Contains(TEXT("access-1")));

	FFlockStoredTokens Loaded;
	TestTrue(TEXT("loads"), Store.Load(Loaded));
	TestEqual(TEXT("access"), Loaded.AccessToken, FString(TEXT("access-1")));
	TestEqual(TEXT("refresh"), Loaded.RefreshToken, FString(TEXT("refresh-1")));
	TestTrue(TEXT("method set"), Loaded.AuthMethod.IsSet());
	TestEqual(TEXT("method"), static_cast<int32>(Loaded.AuthMethod.GetValue()), static_cast<int32>(EFlockAuthMethod::Email));

	Store.Clear();
	TestFalse(TEXT("file gone"), IFileManager::Get().FileExists(*Path));
	TestFalse(TEXT("load after clear"), Store.Load(Loaded));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockFileTokenStoreEdgeTest, "Flock.Auth.TokenStore.Edges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockFileTokenStoreEdgeTest::RunTest(const FString& Parameters)
{
	// Missing file -> no session.
	{
		const FString Path = TestFilePath(TEXT("missing.dat"));
		IFileManager::Get().Delete(*Path);
		FFlockFileTokenStore Store(Path, TEXT("test-game"));
		FFlockStoredTokens Loaded;
		TestFalse(TEXT("missing -> false"), Store.Load(Loaded));
	}
	// Corrupt file -> no session, and the file is removed.
	{
		const FString Path = TestFilePath(TEXT("corrupt.dat"));
		FFileHelper::SaveStringToFile(TEXT("garbage-not-encrypted"), *Path);
		FFlockFileTokenStore Store(Path, TEXT("test-game"));
		FFlockStoredTokens Loaded;
		TestFalse(TEXT("corrupt -> false"), Store.Load(Loaded));
		TestFalse(TEXT("corrupt file deleted"), IFileManager::Get().FileExists(*Path));
	}
	// No auth method saved -> loads with method unset.
	{
		const FString Path = TestFilePath(TEXT("nomethod.dat"));
		IFileManager::Get().Delete(*Path);
		FFlockFileTokenStore Store(Path, TEXT("test-game"));
		FFlockStoredTokens Tokens;
		Tokens.AccessToken = TEXT("a");
		Tokens.RefreshToken = TEXT("r");
		Store.Save(Tokens);
		FFlockStoredTokens Loaded;
		TestTrue(TEXT("loads"), Store.Load(Loaded));
		TestFalse(TEXT("method unset"), Loaded.AuthMethod.IsSet());
	}
	// Empty access token save behaves as Clear.
	{
		const FString Path = TestFilePath(TEXT("emptysave.dat"));
		FFlockFileTokenStore Store(Path, TEXT("test-game"));
		FFlockStoredTokens Tokens;
		Tokens.AccessToken = TEXT("a");
		Store.Save(Tokens);
		Tokens.AccessToken = TEXT("");
		Store.Save(Tokens);
		FFlockStoredTokens Loaded;
		TestFalse(TEXT("cleared"), Store.Load(Loaded));
		TestFalse(TEXT("file removed"), IFileManager::Get().FileExists(*Path));
	}
	// A store keyed differently cannot read the file (key binds to context).
	{
		const FString Path = TestFilePath(TEXT("keyed.dat"));
		IFileManager::Get().Delete(*Path);
		FFlockFileTokenStore Store(Path, TEXT("game-a"));
		FFlockStoredTokens Tokens;
		Tokens.AccessToken = TEXT("a");
		Store.Save(Tokens);
		FFlockFileTokenStore OtherStore(Path, TEXT("game-b"));
		FFlockStoredTokens Loaded;
		TestFalse(TEXT("wrong key -> false"), OtherStore.Load(Loaded));
	}
	return true;
}

/** New tokens are written beside the saved sign-in and moved over it, so a kill part-way through never leaves a torn file. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockFileTokenStoreSavesThroughATemporaryFileTest, "Flock.Auth.TokenStore.SavesThroughATemporaryFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockFileTokenStoreSavesThroughATemporaryFileTest::RunTest(const FString& Parameters)
{
	const FString Path = TestFilePath(TEXT("through_temporary.dat"));
	IFileManager::Get().Delete(*Path);
	FFlockFileTokenStore Store(Path, TEXT("test-game"));
	FFlockStoredTokens Tokens;
	Tokens.AccessToken = TEXT("access-1");
	Tokens.RefreshToken = TEXT("refresh-1");
	Store.Save(Tokens);

	bool bWrittenAside = false;
	FString AccessReadDuringSave;
	ON_SCOPE_EXIT { FFlockTemporaryFiles::SetBeforeNextMoveForTesting(nullptr); };
	FFlockTemporaryFiles::SetBeforeNextMoveForTesting([&bWrittenAside, &AccessReadDuringSave, &Path](const FString&)
	{
		// The game is killed at this moment: what does the next launch restore?
		bWrittenAside = true;
		FFlockFileTokenStore NextLaunch(Path, TEXT("test-game"));
		FFlockStoredTokens Previous;
		if (NextLaunch.Load(Previous))
		{
			AccessReadDuringSave = Previous.AccessToken;
		}
	});
	Tokens.AccessToken = TEXT("access-2");
	Store.Save(Tokens);

	TestTrue(TEXT("The new tokens are written to a temporary file of their own first"), bWrittenAside);
	TestEqual(TEXT("Until they are moved into place, the previous sign-in still restores"), AccessReadDuringSave, FString(TEXT("access-1")));
	FFlockStoredTokens Loaded;
	TestTrue(TEXT("The new tokens land"), Store.Load(Loaded) && Loaded.AccessToken == TEXT("access-2"));
	TestEqual(TEXT("No temporary file is left"), FFlockTemporaryFiles::FindTemporaryFilesOf(Path).Num(), 0);
	Store.Clear();
	return true;
}

/** Tokens a kill cut off between the old file's delete and the new one's move still restore the sign-in. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockFileTokenStoreKeepsSignInCutOffMidSaveTest, "Flock.Auth.TokenStore.KeepsASignInCutOffMidSave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockFileTokenStoreKeepsSignInCutOffMidSaveTest::RunTest(const FString& Parameters)
{
	const FString Written = TestFilePath(TEXT("cutoff_written.dat"));
	IFileManager::Get().Delete(*Written);
	{
		FFlockFileTokenStore Writer(Written, TEXT("test-game"));
		FFlockStoredTokens Tokens;
		Tokens.AccessToken = TEXT("access-cut");
		Tokens.RefreshToken = TEXT("refresh-cut");
		Writer.Save(Tokens);
	}
	TArray<uint8> WholeFile;
	TestTrue(TEXT("Precondition: whole tokens are written"), FFileHelper::LoadFileToArray(WholeFile, *Written));
	IFileManager::Get().Delete(*Written);

	// No file, and the new tokens whole in their temporary file. The next launch may come days later.
	{
		const FString Path = TestFilePath(TEXT("cutoff.dat"));
		IFileManager::Get().Delete(*Path);
		const FString TemporaryFile = FFlockTemporaryFiles::MakePath(Path);
		FFileHelper::SaveArrayToFile(WholeFile, *TemporaryFile);
		FlockMoveTestFileTimeBack(TemporaryFile);
		FFlockFileTokenStore Store(Path, TEXT("test-game"));
		FFlockStoredTokens Loaded;
		TestTrue(TEXT("The sign-in still restores"), Store.Load(Loaded));
		TestEqual(TEXT("With the tokens that were being saved"), Loaded.AccessToken, FString(TEXT("access-cut")));
		TestEqual(TEXT("Leaving no temporary file"), FFlockTemporaryFiles::FindTemporaryFilesOf(Path).Num(), 0);
		Store.Clear();
	}

	// Control: a temporary file cut off while it was written restores nothing.
	{
		const FString Path = TestFilePath(TEXT("halfwritten.dat"));
		IFileManager::Get().Delete(*Path);
		const FString TemporaryFile = FFlockTemporaryFiles::MakePath(Path);
		TArray<uint8> HalfFile(WholeFile.GetData(), WholeFile.Num() / 2);
		FFileHelper::SaveArrayToFile(HalfFile, *TemporaryFile);
		FFlockFileTokenStore Store(Path, TEXT("test-game"));
		FFlockStoredTokens Loaded;
		TestFalse(TEXT("Control: half-written tokens restore nothing"), Store.Load(Loaded));
		IFileManager::Get().Delete(*TemporaryFile);
	}

	// Control: the newest temporary file was cut off while it was written; the whole one before it restores the sign-in.
	{
		const FString Path = TestFilePath(TEXT("newesthalf.dat"));
		IFileManager::Get().Delete(*Path);
		const FString WholeTemporaryFile = FFlockTemporaryFiles::MakePath(Path);
		const FString HalfTemporaryFile = FFlockTemporaryFiles::MakePath(Path);
		TArray<uint8> HalfFile(WholeFile.GetData(), WholeFile.Num() / 2);
		FFileHelper::SaveArrayToFile(WholeFile, *WholeTemporaryFile);
		FFileHelper::SaveArrayToFile(HalfFile, *HalfTemporaryFile);
		FlockMoveTestFileTimeBack(WholeTemporaryFile, 5.0);
		FlockMoveTestFileTimeBack(HalfTemporaryFile, 2.0);
		FFlockFileTokenStore Store(Path, TEXT("test-game"));
		FFlockStoredTokens Loaded;
		TestTrue(TEXT("Control: a half-written newest file is passed over for the whole one before it"),
			Store.Load(Loaded) && Loaded.AccessToken == TEXT("access-cut"));
		IFileManager::Get().Delete(*HalfTemporaryFile);
		Store.Clear();
	}

	// A sign-in that cannot be moved into place stays in its temporary file, so a later launch still finds it.
	{
		const FString Path = TestFilePath(TEXT("cannotmove.dat"));
		IFileManager::Get().Delete(*Path);
		const FString TemporaryFile = FFlockTemporaryFiles::MakePath(Path);
		FFileHelper::SaveArrayToFile(WholeFile, *TemporaryFile);
		FlockMoveTestFileTimeBack(TemporaryFile);
		// A folder stands where the file goes, so nothing can be moved over it.
		IFileManager::Get().MakeDirectory(*Path, /*Tree*/ true);

		FFlockFileTokenStore Store(Path, TEXT("test-game"));
		TestTrue(TEXT("The tokens are kept where they are rather than swept, as they are the only copy"),
			IFileManager::Get().FileExists(*TemporaryFile));
		IFileManager::Get().Delete(*TemporaryFile);
		IFileManager::Get().DeleteDirectory(*Path, /*RequireExists*/ false, /*Tree*/ true);
	}

	// Control: tokens another game wrote with another key are not taken for this game's sign-in.
	{
		const FString Path = TestFilePath(TEXT("otherkey.dat"));
		IFileManager::Get().Delete(*Path);
		const FString TemporaryFile = FFlockTemporaryFiles::MakePath(Path);
		FFileHelper::SaveArrayToFile(WholeFile, *TemporaryFile);
		FFlockFileTokenStore Store(Path, TEXT("another-game"));
		FFlockStoredTokens Loaded;
		TestFalse(TEXT("Control: tokens under another key restore nothing"), Store.Load(Loaded));
		IFileManager::Get().Delete(*TemporaryFile);
	}
	return true;
}

/** Signing out erases a save from moments ago too, so nothing signs the player back in on the next launch. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockFileTokenStoreClearForgetsASaveInProgressTest, "Flock.Auth.TokenStore.ClearForgetsASaveMomentsOld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockFileTokenStoreClearForgetsASaveInProgressTest::RunTest(const FString& Parameters)
{
	const FString Path = TestFilePath(TEXT("cleared.dat"));
	IFileManager::Get().Delete(*Path);
	FFlockFileTokenStore Store(Path, TEXT("test-game"));
	FFlockStoredTokens Tokens;
	Tokens.AccessToken = TEXT("access-1");
	Store.Save(Tokens);

	// A save from moments ago, cut off before its move: fresh, so no sweep would touch it.
	TArray<uint8> SavedBytes;
	TestTrue(TEXT("Precondition: the saved sign-in reads back"), FFileHelper::LoadFileToArray(SavedBytes, *Path));
	const FString TemporaryFile = FFlockTemporaryFiles::MakePath(Path);
	TestTrue(TEXT("Precondition: a fresh temporary file sits beside it"), FFileHelper::SaveArrayToFile(SavedBytes, *TemporaryFile));

	Store.Clear();
	TestFalse(TEXT("The saved sign-in is gone"), IFileManager::Get().FileExists(*Path));
	TestEqual(TEXT("And so is the save from moments ago"), FFlockTemporaryFiles::FindTemporaryFilesOf(Path).Num(), 0);
	FFlockFileTokenStore NextLaunch(Path, TEXT("test-game"));
	FFlockStoredTokens Loaded;
	TestFalse(TEXT("So the next launch restores nothing"), NextLaunch.Load(Loaded));
	return true;
}

/** Temporary files a crash left beside the saved sign-in are deleted once they are old; a fresh one may be another game's save. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockFileTokenStoreDeletesOldLeftOverFilesTest, "Flock.Auth.TokenStore.DeletesOnlyOldLeftOverFiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockFileTokenStoreDeletesOldLeftOverFilesTest::RunTest(const FString& Parameters)
{
	const FString Path = TestFilePath(TEXT("leftovers.dat"));
	FFileHelper::SaveStringToFile(TEXT("not tokens"), *Path);
	const FString OldFile = FFlockTemporaryFiles::MakePath(Path);
	const FString FreshFile = FFlockTemporaryFiles::MakePath(Path);
	FFileHelper::SaveStringToFile(TEXT("half"), *OldFile);
	FFileHelper::SaveStringToFile(TEXT("half"), *FreshFile);
	FlockMoveTestFileTimeBack(OldFile);

	const FFlockFileTokenStore Store(Path, TEXT("test-game"));
	TestFalse(TEXT("A temporary file a crash left long ago is deleted"), IFileManager::Get().FileExists(*OldFile));
	TestTrue(TEXT("A fresh one may be another game's save in progress, and is kept"), IFileManager::Get().FileExists(*FreshFile));
	IFileManager::Get().Delete(*FreshFile);
	IFileManager::Get().Delete(*Path);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
