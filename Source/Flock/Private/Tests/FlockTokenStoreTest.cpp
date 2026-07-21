// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockFileTokenStore.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	FString TestFilePath(const FString& Name)
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FlockTests"), Name);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockFileTokenStoreRoundtripTest, "Flock.Auth.TokenStore.Roundtrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

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
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

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

#endif // WITH_AUTOMATION_TESTS
