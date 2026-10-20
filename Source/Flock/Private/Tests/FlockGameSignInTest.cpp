// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Analytics/FlockAnalyticsLaunches.h"
#include "Auth/FlockFileTokenStore.h"
#include "Auth/FlockGameTokenStore.h"
#include "Config/FlockConfig.h"
#include "Config/FlockGameKey.h"
#include "FlockInitConfig.h"
#include "FlockSubsystem.h"
#include "HAL/FileManager.h"
#include "Misc/Base64.h"
#include "Misc/FlockLaunchFolder.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockTestSdk.h"
#include "Tests/Support/FlockTestSpelling.h"

#define FLOCK_GAME_SIGN_IN_TEST(TestClass, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(TestClass, "Flock.Auth.GameSignIn." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

namespace FlockGameSignInTesting
{
	/** Two games' own stores and the one every game shared before 1.33.0, in memory. */
	struct FStores
	{
		TSharedRef<FFlockMemoryTokenStore> GameA = MakeShared<FFlockMemoryTokenStore>();
		TSharedRef<FFlockMemoryTokenStore> GameB = MakeShared<FFlockMemoryTokenStore>();
		TSharedRef<FFlockMemoryTokenStore> EveryGame = MakeShared<FFlockMemoryTokenStore>();

		FFlockGameTokenStore A() const { return FFlockGameTokenStore(GameA, EveryGame); }
		FFlockGameTokenStore B() const { return FFlockGameTokenStore(GameB, EveryGame); }
	};

	inline FFlockStoredTokens Tokens(const FString& Access)
	{
		FFlockStoredTokens Out;
		Out.AccessToken = Access;
		Out.RefreshToken = Access + TEXT("-refresh");
		return Out;
	}

	/** An access token that reads back as PlayerId for an hour. */
	inline FString SignedInAs(const FString& PlayerId)
	{
		const FString Payload = FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId, FDateTime::UtcNow().ToUnixTimestamp() + 3600);
		FString Encoded = FBase64::Encode(Payload);
		Encoded.ReplaceInline(TEXT("+"), TEXT("-"));
		Encoded.ReplaceInline(TEXT("/"), TEXT("_"));
		Encoded.ReplaceInline(TEXT("="), TEXT(""));
		return FString::Printf(TEXT("h.%s.s"), *Encoded);
	}

	inline FFlockInitConfig GameConfig(const FString& ApiKey)
	{
		FFlockInitConfig Config;
		Config.ApiUrl = TEXT("https://api.example.com");
		Config.ApiKey = ApiKey;
		Config.GameId = TEXT("One Game Name");
		Config.GameVersion = TEXT("1.0.0");
		Config.GameVersionId = TEXT("ver-1");
		return Config;
	}
}

using namespace FlockGameSignInTesting;

FLOCK_GAME_SIGN_IN_TEST(FFlockGameSignInApartTest, "TwoGamesKeepTheirSignInsApart")
bool FFlockGameSignInApartTest::RunTest(const FString& Parameters)
{
	FStores Stores;
	Stores.A().Save(Tokens(TEXT("a")));
	FFlockStoredTokens Read;
	TestFalse(TEXT("Another game restores nobody"), Stores.B().Load(Read));
	TestTrue(TEXT("Its own game restores its player"), Stores.A().Load(Read) && Read.AccessToken == TEXT("a"));
	return true;
}

FLOCK_GAME_SIGN_IN_TEST(FFlockGameSignInEarlierTest, "TheSignInAnEarlierBuildKeptForEveryGameIsTakenOverByTheFirstGameOnce")
bool FFlockGameSignInEarlierTest::RunTest(const FString& Parameters)
{
	FStores Stores;
	Stores.EveryGame->Save(Tokens(TEXT("earlier")));
	FFlockStoredTokens Read;
	TestTrue(TEXT("Nobody is signed out by updating"), Stores.A().Load(Read));
	TestEqualSensitive(TEXT("The earlier sign-in"), Read.AccessToken, FString(TEXT("earlier")));
	TestEqualSensitive(TEXT("With its refresh token"), Read.RefreshToken, FString(TEXT("earlier-refresh")));
	TestFalse(TEXT("No longer kept for every game"), Stores.EveryGame->bHasTokens);
	FFlockStoredTokens Second;
	TestFalse(TEXT("A second game takes nothing over"), Stores.B().Load(Second));
	TestTrue(TEXT("The first game keeps it as its own"), Stores.A().Load(Second) && Second.AccessToken == TEXT("earlier"));
	return true;
}

FLOCK_GAME_SIGN_IN_TEST(FFlockGameSignInSaveFailedTest, "TheEarlierBuildsSignInIsKeptWhenTheGamesOwnSlotCouldNotSaveIt")
bool FFlockGameSignInSaveFailedTest::RunTest(const FString& Parameters)
{
	FStores Stores;
	Stores.EveryGame->Save(Tokens(TEXT("earlier")));
	// The game's own file could not be written, as on a full disk, and nothing said so.
	Stores.GameA->bFailSave = true;
	FFlockStoredTokens Read;
	TestTrue(TEXT("This launch is still signed in"), Stores.A().Load(Read) && Read.AccessToken == TEXT("earlier"));
	TestTrue(TEXT("And the earlier sign-in waits for the next launch, not lost"), Stores.EveryGame->bHasTokens);
	return true;
}

FLOCK_GAME_SIGN_IN_TEST(FFlockGameSignInReplacesTest, "ANewSignInReplacesTheOneAnEarlierBuildKept")
bool FFlockGameSignInReplacesTest::RunTest(const FString& Parameters)
{
	FStores Stores;
	Stores.EveryGame->Save(Tokens(TEXT("earlier")));
	Stores.A().Save(Tokens(TEXT("a")));
	FFlockStoredTokens Read;
	TestFalse(TEXT("No later game takes the earlier sign-in over"), Stores.B().Load(Read));
	return true;
}

FLOCK_GAME_SIGN_IN_TEST(FFlockGameSignInSignOutTest, "SigningOutForgetsTheOneAnEarlierBuildKeptToo")
bool FFlockGameSignInSignOutTest::RunTest(const FString& Parameters)
{
	FStores Stores;
	Stores.EveryGame->Save(Tokens(TEXT("earlier")));
	Stores.GameA->Save(Tokens(TEXT("a")));
	Stores.A().Clear();
	FFlockStoredTokens Read;
	TestFalse(TEXT("Signed out stays signed out at the next launch"), Stores.A().Load(Read));
	return true;
}

FLOCK_GAME_SIGN_IN_TEST(FFlockGameKeyTest, "TheGameKeyIsSixteenHexCharactersAndReadsTheAddressAsTheServerDoes")
bool FFlockGameKeyTest::RunTest(const FString& Parameters)
{
	const FString Key = FFlockGameKey::Of(TEXT("https://api.example.com"), TEXT("the-key"));
	TestEqual(TEXT("Sixteen characters"), Key.Len(), 16);
	bool bHex = true;
	for (const TCHAR Each : Key)
	{
		bHex &= (Each >= TEXT('0') && Each <= TEXT('9')) || (Each >= TEXT('a') && Each <= TEXT('f'));
	}
	TestTrue(TEXT("Of lower-case hex"), bHex);
	TestEqualSensitive(TEXT("Spaces, a trailing slash and letter case in the address change nothing"),
		FFlockGameKey::Of(TEXT(" HTTPS://API.Example.com/ "), TEXT(" the-key ")), Key);
	TestFalse(TEXT("An API key's letter case is its own"), FFlockGameKey::Of(TEXT("https://api.example.com"), TEXT("The-key")).Equals(Key, ESearchCase::CaseSensitive));
	TestFalse(TEXT("Another backend is another game"), FFlockGameKey::Of(TEXT("https://dev.example.com"), TEXT("the-key")).Equals(Key, ESearchCase::CaseSensitive));
	TestFalse(TEXT("The API key is never written as it is"), Key.Contains(TEXT("the-key")));
	return true;
}

FLOCK_GAME_SIGN_IN_TEST(FFlockGameSignInTheSdksOwnTest, "TheSdkKeepsItsGamesSignInAndLaunchesUnderItsGame")
bool FFlockGameSignInTheSdksOwnTest::RunTest(const FString& Parameters)
{
	UFlockConfig* Settings = GetMutableDefault<UFlockConfig>();
	const bool bAnalyticsWas = Settings->bAnalyticsEnabled;
	Settings->bAnalyticsEnabled = true;
	ON_SCOPE_EXIT { Settings->bAnalyticsEnabled = bAnalyticsWas; };

	const FFlockTestSdk Test;
	Test->SetHttpAdapterForTesting(MakeShared<FFlockFakeTransport>());
	const FFlockInitConfig Config = GameConfig(TEXT("key-a"));
	const FString GameKey = FFlockGameKey::Of(Config.ApiUrl, Config.ApiKey);
	// The sign-in an earlier build kept for every game, sealed as it was: by the Game Name.
	FFlockFileTokenStore(FFlockFileTokenStore::PathOf(Test.Folder, FString()), Config.GameId).Save(Tokens(SignedInAs(TEXT("player-earlier"))));

	Test->InitializeWithConfig(Config);
	TestTrue(TEXT("Precondition: initialized"), Test->IsInitialized());
	TestEqualSensitive(TEXT("Nobody is signed out by updating"), Test->GetPlayerId(), FString(TEXT("player-earlier")));
	TestTrue(TEXT("Kept from now on in the game's own file"), IFileManager::Get().FileExists(*FFlockFileTokenStore::PathOf(Test.Folder, GameKey)));
	TestFalse(TEXT("And no longer for every game"), IFileManager::Get().FileExists(*FFlockFileTokenStore::PathOf(Test.Folder, FString())));
	TestEqual(TEXT("This launch's analytics folder is its game's"),
		FFlockLaunchFolder::FindFolders(FFlockAnalyticsLaunches::LaunchesFolderOf(FPaths::Combine(Test.Folder, TEXT("analytics")), GameKey)).Num(), 1);
	return true;
}

FLOCK_GAME_SIGN_IN_TEST(FFlockGameSignInNotAnotherGamesTest, "TheSdkRestoresNoOtherGamesSignIn")
bool FFlockGameSignInNotAnotherGamesTest::RunTest(const FString& Parameters)
{
	const FFlockTestSdk Test;
	Test->SetHttpAdapterForTesting(MakeShared<FFlockFakeTransport>());
	// Another game of the same name on another API key: its file reads back under this game's name, so only its place tells it apart.
	const FFlockInitConfig Other = GameConfig(TEXT("key-b"));
	const FString OtherFile = FFlockFileTokenStore::PathOf(Test.Folder, FFlockGameKey::Of(Other.ApiUrl, Other.ApiKey));
	FFlockFileTokenStore(OtherFile, Other.GameId).Save(Tokens(SignedInAs(TEXT("player-of-another-game"))));

	Test->InitializeWithConfig(GameConfig(TEXT("key-a")));
	TestTrue(TEXT("Precondition: initialized"), Test->IsInitialized());
	TestTrue(TEXT("Nobody is restored"), Test->GetPlayerId().IsEmpty());
	TestTrue(TEXT("And the other game's sign-in is left for it"), IFileManager::Get().FileExists(*OtherFile));
	return true;
}

#endif
