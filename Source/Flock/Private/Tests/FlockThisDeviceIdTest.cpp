// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Async/Async.h"
#include "Auth/FlockAuthSession.h"
#include "Auth/FlockDeviceIdFile.h"
#include "Auth/FlockFileTokenStore.h"
#include "Dom/JsonObject.h"
#include "FlockDevelopmentCommands.h"
#include "FlockInitConfig.h"
#include "FlockLogger.h"
#include "FlockSubsystem.h"
#include "GenericPlatform/GenericPlatformProperties.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Http/FlockEndpoints.h"
#include "Http/FlockHttpClient.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/FlockEngineCompat.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Providers/FlockAuthProvider.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockRecordingLogger.h"
#include "Tests/Support/FlockTemporaryFilesTestSupport.h"
#include "Tests/Support/FlockTestSdk.h"

// ThisDeviceId: made once, kept in the SDK's saved files across starts, and sent by the device calls when the game's id is
// empty. Every test keeps the file in a folder of its own; none touches the project's saved files.
namespace FlockThisDeviceIdTestHelpers
{
	inline FString NewFolder()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
			FString::Printf(TEXT("device_id_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
	}

	inline void DeleteFolder(const FString& Folder)
	{
		IFileManager::Get().DeleteDirectory(*Folder, /*bRequireExists*/ false, /*bTree*/ true);
	}

	/** An id the SDK makes: a lower-case GUID with hyphens. */
	inline bool IsNewId(const FString& Text)
	{
		FGuid Guid;
		return FGuid::ParseExact(Text, EGuidFormats::DigitsWithHyphensLower, Guid) && Guid.IsValid()
			&& Guid.ToString(EGuidFormats::DigitsWithHyphensLower).Equals(Text, ESearchCase::CaseSensitive);
	}

	inline FString ReadText(const FString& Path)
	{
		FString Text;
		return FFileHelper::LoadFileToString(Text, *Path) ? Text : FString();
	}

	inline TArray<uint8> ReadBytes(const FString& Path)
	{
		TArray<uint8> Bytes;
		FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent);
		return Bytes;
	}

	/** Written long enough ago that no start can still be writing it. */
	inline bool WriteLeftOver(const FString& Path, const FString& Contents)
	{
		if (!FFileHelper::SaveStringToFile(Contents, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			return false;
		}
		FlockMoveTestFileTimeBack(Path);
		return true;
	}

	/** The names of the files in Folder, sorted. */
	inline TArray<FString> FilesIn(const FString& Folder)
	{
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *FPaths::Combine(Folder, TEXT("*")), /*bFiles*/ true, /*bDirectories*/ false);
		Names.Sort();
		return Names;
	}

	inline FString MakeJwt(const FString& PlayerId)
	{
		const int64 Exp = FDateTime::UtcNow().ToUnixTimestamp() + 3600;
		FString Payload = FBase64::Encode(FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId, Exp));
		Payload.ReplaceInline(TEXT("+"), TEXT("-"));
		Payload.ReplaceInline(TEXT("/"), TEXT("_"));
		Payload.ReplaceInline(TEXT("="), TEXT(""));
		return FString::Printf(TEXT("h.%s.s"), *Payload);
	}

	inline FFlockRetryPolicy NoRetry()
	{
		FFlockRetryPolicy Policy;
		Policy.MaxRetries = 0;
		return Policy;
	}

	/** One start of the SDK: an auth provider whose device id file is in Folder, answering the three device routes. */
	struct FStart
	{
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockHttpClient> Client;
		TSharedRef<FFlockAuthSession> Session;
		TSharedRef<FFlockRecordingLogger> Log = MakeShared<FFlockRecordingLogger>();
		TUniquePtr<FFlockAuthProvider> Auth;

		explicit FStart(const FString& Folder)
			: Client(MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>()))
			, Session(MakeShared<FFlockAuthSession>(Client, MakeShared<FFlockMemoryTokenStore>(), MakeShared<FFlockNullLogger>(),
				TEXT("http://x/v1"), TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") } }))
		{
			// The player routes answer at the root, not under `result`.
			const FString Login = FString::Printf(TEXT("{\"player_id\":\"player-a\",\"access_token\":\"%s\",\"refresh_token\":\"refresh-a\"}"),
				*MakeJwt(TEXT("player-a")));
			Fake->On(FlockEndpoints::PlayerLoginDevice, FFlockFakeTransport::Ok(Login));
			Fake->On(FlockEndpoints::PlayerRegisterDevice, FFlockFakeTransport::Ok(Login));
			Fake->On(FlockEndpoints::PlayerLinkDevice,
				FFlockFakeTransport::Ok(TEXT("{\"accounts\":[{\"provider\":\"device_id\",\"provider_user_id\":\"linked\"}]}")));
			Auth = MakeUnique<FFlockAuthProvider>(Client, NoRetry(), Log, Session, nullptr, TEXT("http://x/v1"),
				MakeShared<FFlockDeviceIdFile>(Folder));
		}

		/** The body of the last request to Route, or null when none was sent. */
		TSharedPtr<FJsonObject> LastBodyTo(const FString& Route) const
		{
			for (int32 Index = Fake->Requests.Num() - 1; Index >= 0; --Index)
			{
				if (Fake->Requests[Index].Url.Contains(Route, ESearchCase::CaseSensitive))
				{
					TSharedPtr<FJsonObject> Body;
					FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Fake->Requests[Index].JsonBody), Body);
					return Body;
				}
			}
			return nullptr;
		}

		int32 WarningsAbout(const FString& Fragment) const
		{
			int32 Count = 0;
			for (const FString& Line : Log->Warnings)
			{
				Count += Line.Contains(Fragment, ESearchCase::CaseSensitive) ? 1 : 0;
			}
			return Count;
		}
	};

	/** The device calls send exactly these fields: the device type, the id, and the name when the game gives one. */
	inline void TestDeviceBody(FAutomationTestBase& Test, const FString& What, const TSharedPtr<FJsonObject>& Body,
		const FString& ExpectedId, const TArray<FString>& OtherFields = {})
	{
		if (!Test.TestTrue(FString::Printf(TEXT("%s: a body was sent"), *What), Body.IsValid()))
		{
			return;
		}
		TArray<FString> Fields;
		for (const auto& Field : Body->Values)
		{
			Fields.Add(FString(*Field.Key));
		}
		TArray<FString> Expected = { TEXT("device_type"), TEXT("device_id") };
		Expected.Append(OtherFields);
		Fields.Sort();
		Expected.Sort();
		// Letter for letter: TestEqual on strings ignores case.
		auto Spelled = [&Test, &What](const TCHAR* Check, const FString& Actual, const FString& Wanted)
		{
			Test.TestTrue(FString::Printf(TEXT("%s: %s is '%s', expected '%s'"), *What, Check, *Actual, *Wanted),
				Actual.Equals(Wanted, ESearchCase::CaseSensitive));
		};
		Spelled(TEXT("the body's fields"), FString::Join(Fields, TEXT(",")), FString::Join(Expected, TEXT(",")));
		FString Id;
		Test.TestTrue(FString::Printf(TEXT("%s: the id is a string"), *What), Body->TryGetStringField(TEXT("device_id"), Id));
		Spelled(TEXT("the id"), Id, ExpectedId);
		FString Type;
		Body->TryGetStringField(TEXT("device_type"), Type);
		Spelled(TEXT("the device type"), Type, FString(FPlatformProperties::IniPlatformName()));
	}

	inline FFlockInitConfig MakeConfig()
	{
		FFlockInitConfig Config;
		Config.ApiUrl = TEXT("https://api-flock.qwacks.com");
		Config.ApiKey = TEXT("secret");
		Config.GameId = TEXT("my-game");
		Config.GameVersion = TEXT("1.2.3");
		Config.GameVersionId = TEXT("ver-abc");
		return Config;
	}
}

using namespace FlockThisDeviceIdTestHelpers;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockThisDeviceIdMadeOnceTest, "Flock.Auth.ThisDeviceId.IsMadeOnceAndTheNextStartReadsItBack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockThisDeviceIdMadeOnceTest::RunTest(const FString& Parameters)
{
	const FString Folder = NewFolder();
	ON_SCOPE_EXIT { DeleteFolder(Folder); };
	const FString File = FPaths::Combine(Folder, FFlockDeviceIdFile::FileName);

	FString First;
	{
		FStart Start(Folder);
		First = Start.Auth->ThisDeviceId();
		TestTrue(FString::Printf(TEXT("a lower-case GUID: %s"), *First), IsNewId(First));
		TestEqualSensitive(TEXT("kept in the SDK's saved files, as written"), ReadText(File), First);

		// Read once a start: a file changed under a running game does not change its id.
		FFileHelper::SaveStringToFile(FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower), *File);
		TestEqualSensitive(TEXT("the same for the rest of the start"), Start.Auth->ThisDeviceId(), First);
		FFileHelper::SaveStringToFile(First, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	// The next start remembers nothing and reads the file.
	FStart Next(Folder);
	TestEqualSensitive(TEXT("the next start reads it back"), Next.Auth->ThisDeviceId(), First);
	Next.Auth->LoginWithDevice(FString(), nullptr);
	TestDeviceBody(*this, TEXT("login"), Next.LastBodyTo(FlockEndpoints::PlayerLoginDevice), First);
	TestEqual(TEXT("nothing to warn about"), Next.Log->Warnings.Num(), 0);
	TestEqualSensitive(TEXT("one file, and no temporary file left"), FString::Join(FilesIn(Folder), TEXT(",")), FString(FFlockDeviceIdFile::FileName));
	return true;
}

/** Each start of the SDK (a launch, a Play In Editor session) reads the file again; within one start the id is remembered. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockThisDeviceIdEachStartTest, "Flock.Auth.ThisDeviceId.EachStartOfTheSdkReadsTheFileAgain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockThisDeviceIdEachStartTest::RunTest(const FString& Parameters)
{
	const FString ProjectFile = FPaths::Combine(FFlockFileTokenStore::DefaultFolder(), FFlockDeviceIdFile::FileName);
	// A missing file reads as the minimum time, so a file made or deleted shows up as a change too.
	const FDateTime ProjectFileBefore = IFileManager::Get().GetTimeStamp(*ProjectFile);

	FFlockTestSdk Sdk;
	Sdk->InitializeWithConfig(MakeConfig());
	if (!TestTrue(TEXT("the SDK started"), Sdk->IsInitialized() && Sdk->GetAuthProvider() != nullptr))
	{
		return false;
	}
	const FString File = FPaths::Combine(Sdk.Folder, FFlockDeviceIdFile::FileName);
	const FString First = Sdk->GetAuthProvider()->ThisDeviceId();
	TestTrue(FString::Printf(TEXT("a lower-case GUID: %s"), *First), IsNewId(First));
	TestEqualSensitive(TEXT("kept beside the saved sign-in, in the SDK's saved files folder"), ReadText(File), First);

	IFileManager::Get().Delete(*File);
	TestEqualSensitive(TEXT("remembered for the rest of the start"), Sdk->GetAuthProvider()->ThisDeviceId(), First);

	Sdk->ShutdownSdk();
	Sdk->InitializeWithConfig(MakeConfig());
	if (!TestTrue(TEXT("the SDK started again"), Sdk->IsInitialized() && Sdk->GetAuthProvider() != nullptr))
	{
		return false;
	}
	const FString Next = Sdk->GetAuthProvider()->ThisDeviceId();
	TestTrue(FString::Printf(TEXT("a lower-case GUID: %s"), *Next), IsNewId(Next));
	TestNotEqual(TEXT("the deleted file gave a new id"), Next, First);
	TestEqualSensitive(TEXT("and it is kept"), ReadText(File), Next);

	TestTrue(TEXT("the project's own device id file is untouched"), IFileManager::Get().GetTimeStamp(*ProjectFile) == ProjectFileBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockThisDeviceIdEmptyIdTest, "Flock.Auth.ThisDeviceId.TheDeviceCallsSendTheKeptIdWhenTheirIdIsEmpty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockThisDeviceIdEmptyIdTest::RunTest(const FString& Parameters)
{
	const FString Folder = NewFolder();
	ON_SCOPE_EXIT { DeleteFolder(Folder); };

	FStart Start(Folder);
	Start.Auth->RegisterWithDevice(FString(), FString(), nullptr);
	Start.Auth->LoginWithDevice(FString(), nullptr);
	bool bLinked = false;
	Start.Auth->LinkDevice(FString(), [&bLinked](TFlockResult<FFlockPlayerAccountsResponse> Result) { bLinked = Result.bSuccess; });

	const FString Kept = ReadText(FPaths::Combine(Folder, FFlockDeviceIdFile::FileName));
	TestTrue(FString::Printf(TEXT("the kept id was made: %s"), *Kept), IsNewId(Kept));
	TestEqualSensitive(TEXT("and is ThisDeviceId"), Start.Auth->ThisDeviceId(), Kept);
	TestDeviceBody(*this, TEXT("register"), Start.LastBodyTo(FlockEndpoints::PlayerRegisterDevice), Kept);
	TestDeviceBody(*this, TEXT("login"), Start.LastBodyTo(FlockEndpoints::PlayerLoginDevice), Kept);
	TestDeviceBody(*this, TEXT("link"), Start.LastBodyTo(FlockEndpoints::PlayerLinkDevice), Kept);
	TestTrue(TEXT("the link went through, not refused before sending"), bLinked);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockThisDeviceIdGamesOwnIdTest, "Flock.Auth.ThisDeviceId.AnIdTheGamePassesIsSentAsItIsAndNoIdIsKept",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockThisDeviceIdGamesOwnIdTest::RunTest(const FString& Parameters)
{
	const FString Folder = NewFolder();
	ON_SCOPE_EXIT { DeleteFolder(Folder); };
	const FString OwnId = TEXT(" the game's own id ");

	{
		FStart Start(Folder);
		Start.Auth->RegisterWithDevice(OwnId, TEXT("Sam"), nullptr);
		Start.Auth->LoginWithDevice(OwnId, nullptr);
		Start.Auth->LinkDevice(OwnId, nullptr);

		const TSharedPtr<FJsonObject> Registration = Start.LastBodyTo(FlockEndpoints::PlayerRegisterDevice);
		TestDeviceBody(*this, TEXT("register"), Registration, OwnId, { TEXT("name") });
		FString Name;
		if (Registration.IsValid())
		{
			Registration->TryGetStringField(TEXT("name"), Name);
		}
		TestEqualSensitive(TEXT("the name"), Name, FString(TEXT("Sam")));
		TestDeviceBody(*this, TEXT("login"), Start.LastBodyTo(FlockEndpoints::PlayerLoginDevice), OwnId);
		TestDeviceBody(*this, TEXT("link"), Start.LastBodyTo(FlockEndpoints::PlayerLinkDevice), OwnId);
	}
	TestFalse(TEXT("a game passing its own id never makes the kept one"), IFileManager::Get().DirectoryExists(*Folder));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockThisDeviceIdNoGuidTest, "Flock.Auth.ThisDeviceId.AFileHoldingNoGuidGetsANewId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockThisDeviceIdNoGuidTest::RunTest(const FString& Parameters)
{
	const TCHAR* const Cases[] = { TEXT(""), TEXT("not a device id"), TEXT("6f9619ff-8b86-d011"), TEXT("{6f9619ff-8b86-d011-b42d-00c04fc964ff") };
	for (const TCHAR* Contents : Cases)
	{
		const FString Folder = NewFolder();
		ON_SCOPE_EXIT { DeleteFolder(Folder); };
		const FString File = FPaths::Combine(Folder, FFlockDeviceIdFile::FileName);
		if (!TestTrue(FString::Printf(TEXT("[%s] the old file is written"), Contents), WriteLeftOver(File, Contents)))
		{
			continue;
		}
		FStart Start(Folder);
		const FString Id = Start.Auth->ThisDeviceId();
		TestTrue(FString::Printf(TEXT("[%s] a new id: %s"), Contents, *Id), IsNewId(Id));
		TestEqualSensitive(FString::Printf(TEXT("[%s] made in place of what was there"), Contents), ReadText(File), Id);
		TestEqualSensitive(FString::Printf(TEXT("[%s] what was moved aside is gone"), Contents),
			FString::Join(FilesIn(Folder), TEXT(",")), FString(FFlockDeviceIdFile::FileName));
		TestEqual(FString::Printf(TEXT("[%s] nothing to warn about"), Contents), Start.Log->Warnings.Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockThisDeviceIdWrittenAnotherWayTest, "Flock.Auth.ThisDeviceId.AGuidWrittenAnotherWayIsUsedAsWrittenAndTheFileLeftAlone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockThisDeviceIdWrittenAnotherWayTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		const TCHAR* Contents;
		bool bByteOrderMark;
		const TCHAR* Expected;
	};
	// The server may already know the id this way, so it is never rewritten.
	const FCase Cases[] = {
		{ TEXT("6F9619FF-8B86-D011-B42D-00C04FC964FF"), false, TEXT("6F9619FF-8B86-D011-B42D-00C04FC964FF") },
		{ TEXT("6f9619ff-8b86-d011-b42d-00c04fc964ff\n"), false, TEXT("6f9619ff-8b86-d011-b42d-00c04fc964ff") },
		{ TEXT("  6f9619ff-8b86-d011-b42d-00c04fc964ff\r\n"), false, TEXT("6f9619ff-8b86-d011-b42d-00c04fc964ff") },
		{ TEXT("6f9619ff-8b86-d011-b42d-00c04fc964ff"), true, TEXT("6f9619ff-8b86-d011-b42d-00c04fc964ff") },
		{ TEXT("00000000-0000-0000-0000-000000000000"), false, TEXT("00000000-0000-0000-0000-000000000000") },
		{ TEXT("{6f9619ff-8b86-d011-b42d-00c04fc964ff}"), false, TEXT("{6f9619ff-8b86-d011-b42d-00c04fc964ff}") },
		{ TEXT("6f9619ff8b86d011b42d00c04fc964ff"), false, TEXT("6f9619ff8b86d011b42d00c04fc964ff") },
	};
	for (const FCase& Case : Cases)
	{
		const FString Folder = NewFolder();
		ON_SCOPE_EXIT { DeleteFolder(Folder); };
		const FString File = FPaths::Combine(Folder, FFlockDeviceIdFile::FileName);
		const FTCHARToUTF8 Utf8(Case.Contents);
		TArray<uint8> Bytes;
		if (Case.bByteOrderMark)
		{
			Bytes.Append({ 0xEF, 0xBB, 0xBF });
		}
		Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		if (!TestTrue(TEXT("the file is written"), FFileHelper::SaveArrayToFile(Bytes, *File)))
		{
			continue;
		}

		const FString What = FString::Printf(TEXT("[%s%s]"), Case.bByteOrderMark ? TEXT("byte-order mark, ") : TEXT(""), *FString(Case.Contents).TrimStartAndEnd());
		FStart Start(Folder);
		TestEqualSensitive(FString::Printf(TEXT("%s used as written"), *What), Start.Auth->ThisDeviceId(), FString(Case.Expected));
		Start.Auth->LoginWithDevice(FString(), nullptr);
		TestDeviceBody(*this, What, Start.LastBodyTo(FlockEndpoints::PlayerLoginDevice), Case.Expected);
		TestTrue(FString::Printf(TEXT("%s the file is as it was"), *What), ReadBytes(File) == Bytes);
		TestEqualSensitive(FString::Printf(TEXT("%s nothing else saved"), *What), FString::Join(FilesIn(Folder), TEXT(",")), FString(FFlockDeviceIdFile::FileName));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockThisDeviceIdStillWritingTest, "Flock.Auth.ThisDeviceId.AnEmptyFileAnotherStartIsStillWritingIsReadOnceItHoldsTheId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockThisDeviceIdStillWritingTest::RunTest(const FString& Parameters)
{
	const FString Folder = NewFolder();
	ON_SCOPE_EXIT { DeleteFolder(Folder); };
	const FString File = FPaths::Combine(Folder, FFlockDeviceIdFile::FileName);
	const FString OtherStart = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);

	// Another start has made the file and not yet written its id.
	if (!TestTrue(TEXT("the empty file is written"), FFileHelper::SaveStringToFile(FString(), *File)))
	{
		return false;
	}
	TFuture<bool> Writer = Async(EAsyncExecution::Thread, [File, OtherStart]()
	{
		FPlatformProcess::Sleep(0.2f);
		const double GiveUpAt = FPlatformTime::Seconds() + 3.0;
		while (FPlatformTime::Seconds() < GiveUpAt)
		{
			// The start under test may be reading it this moment.
			if (FFileHelper::SaveStringToFile(OtherStart, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				return true;
			}
			FPlatformProcess::Sleep(0.005f);
		}
		return false;
	});

	FStart Start(Folder);
	TestEqualSensitive(TEXT("the other start's id, once it is written"), Start.Auth->ThisDeviceId(), OtherStart);
	TestTrue(TEXT("the other start wrote its id"), Writer.Get());
	TestEqualSensitive(TEXT("the file holds it"), ReadText(File), OtherStart);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockThisDeviceIdUnreadableTest, "Flock.Auth.ThisDeviceId.AFileThatCannotBeReadIsLeftAloneAndTheStartStillSignsIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockThisDeviceIdUnreadableTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS
	const FString Folder = NewFolder();
	ON_SCOPE_EXIT { DeleteFolder(Folder); };
	const FString File = FPaths::Combine(Folder, FFlockDeviceIdFile::FileName);
	const FString Saved = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	if (!TestTrue(TEXT("the saved id is written"), WriteLeftOver(File, Saved)))
	{
		return false;
	}

	{
		// Held, shared with nobody, as another program can hold it.
		const TUniquePtr<IFileHandle> Held(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*File, /*bAppend*/ true, /*bAllowRead*/ false));
		if (!TestTrue(TEXT("the file is held"), Held.IsValid()))
		{
			return false;
		}
		FStart Start(Folder);
		Start.Auth->LoginWithDevice(FString(), nullptr);

		TestTrue(TEXT("signed in all the same"), Start.Session->IsAuthenticated());
		FString Used;
		const TSharedPtr<FJsonObject> Body = Start.LastBodyTo(FlockEndpoints::PlayerLoginDevice);
		if (Body.IsValid())
		{
			Body->TryGetStringField(TEXT("device_id"), Used);
		}
		TestTrue(FString::Printf(TEXT("a new id for this start: %s"), *Used), IsNewId(Used));
		TestNotEqual(TEXT("not the saved one"), Used, Saved);
		TestEqualSensitive(TEXT("one id for the whole start"), Start.Auth->ThisDeviceId(), Used);
		TestEqual(TEXT("said once"), Start.WarningsAbout(File), 1);
		TestEqual(TEXT("names the cause"), Start.WarningsAbout(TEXT("could not be read")), 1);
		// Windows' own words for a file another program holds (ERROR_SHARING_VIOLATION), in the machine's language.
		TCHAR HeldReason[512] = { 0 };
		FPlatformMisc::GetSystemErrorMessage(HeldReason, UE_ARRAY_COUNT(HeldReason), 32);
		TestEqual(TEXT("names the system's reason"),
			Start.WarningsAbout(FString::Printf(TEXT("could not be read (%s)"), *FString(HeldReason).TrimStartAndEnd())), 1);
		TestEqual(TEXT("names the fix"), Start.WarningsAbout(TEXT("start the game again")), 1);
	}
	TestEqualSensitive(TEXT("left alone"), ReadText(File), Saved);
#else
	AddInfo(TEXT("Holding a file so nothing else can read it is Windows' own; other platforms let a held file be read."));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockThisDeviceIdCannotSaveTest, "Flock.Auth.ThisDeviceId.ANewIdThatCannotBeSavedIsUsedForTheStartAndSaidOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockThisDeviceIdCannotSaveTest::RunTest(const FString& Parameters)
{
	const FString Folder = NewFolder();
	ON_SCOPE_EXIT { DeleteFolder(Folder); };
	// A file where the folder should be, so nothing can be saved in it.
	const FString NotAFolder = FPaths::Combine(Folder, TEXT("not-a-folder"));
	if (!TestTrue(TEXT("the file in the folder's place is written"), FFileHelper::SaveStringToFile(FString(), *NotAFolder)))
	{
		return false;
	}

	FStart Start(NotAFolder);
	Start.Auth->LoginWithDevice(FString(), nullptr);
	FString Used;
	const TSharedPtr<FJsonObject> Body = Start.LastBodyTo(FlockEndpoints::PlayerLoginDevice);
	if (Body.IsValid())
	{
		Body->TryGetStringField(TEXT("device_id"), Used);
	}
	TestTrue(FString::Printf(TEXT("a new id for this start: %s"), *Used), IsNewId(Used));
	TestEqualSensitive(TEXT("one id for the whole start"), Start.Auth->ThisDeviceId(), Used);
	TestEqual(TEXT("said once"), Start.WarningsAbout(TEXT("could not be saved")), 1);
	TestEqual(TEXT("names the fix"), Start.WarningsAbout(TEXT("pass a device id of your own")), 1);
	TestEqual(TEXT("and nothing else"), Start.Log->Warnings.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockThisDeviceIdFirstSaveWinsTest, "Flock.Auth.ThisDeviceId.AnotherStartMakingTheFileFirstWins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockThisDeviceIdFirstSaveWinsTest::RunTest(const FString& Parameters)
{
	const FString Folder = NewFolder();
	ON_SCOPE_EXIT
	{
		FFlockDeviceIdFile::SetBeforeNextSaveForTesting(nullptr);
		DeleteFolder(Folder);
	};
	const FString File = FPaths::Combine(Folder, FFlockDeviceIdFile::FileName);
	const FString OtherStart = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);

	// Between this start finding no file and making it, another start makes it.
	FFlockDeviceIdFile::SetBeforeNextSaveForTesting([File, OtherStart]()
	{
		FFileHelper::SaveStringToFile(OtherStart, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	});
	FStart Start(Folder);
	TestEqualSensitive(TEXT("the other start's id"), Start.Auth->ThisDeviceId(), OtherStart);
	TestEqualSensitive(TEXT("never written over"), ReadText(File), OtherStart);
	TestEqualSensitive(TEXT("nothing else left"), FString::Join(FilesIn(Folder), TEXT(",")), FString(FFlockDeviceIdFile::FileName));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockThisDeviceIdReplacingFirstWinsTest, "Flock.Auth.ThisDeviceId.AnotherStartReplacingABadFileFirstWins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockThisDeviceIdReplacingFirstWinsTest::RunTest(const FString& Parameters)
{
	const FString Folder = NewFolder();
	ON_SCOPE_EXIT
	{
		FFlockDeviceIdFile::SetBeforeNextSaveForTesting(nullptr);
		DeleteFolder(Folder);
	};
	const FString File = FPaths::Combine(Folder, FFlockDeviceIdFile::FileName);
	const FString OtherStart = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	if (!TestTrue(TEXT("the bad file is written"), WriteLeftOver(File, TEXT("not a device id"))))
	{
		return false;
	}

	// Both starts found the bad file; the other one put its new id in first, just before this one moves the file aside.
	FFlockDeviceIdFile::SetBeforeNextSaveForTesting([File, OtherStart]()
	{
		FFileHelper::SaveStringToFile(OtherStart, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	});
	FStart Start(Folder);
	TestEqualSensitive(TEXT("the other start's id"), Start.Auth->ThisDeviceId(), OtherStart);
	TestEqualSensitive(TEXT("put back"), ReadText(File), OtherStart);
	TestEqualSensitive(TEXT("what was moved aside is gone"), FString::Join(FilesIn(Folder), TEXT(",")), FString(FFlockDeviceIdFile::FileName));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockThisDeviceIdLeftOverTest, "Flock.Auth.ThisDeviceId.ATemporaryFileAStartLeftIsDeletedAndAFreshOneKept",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockThisDeviceIdLeftOverTest::RunTest(const FString& Parameters)
{
	const FString Folder = NewFolder();
	ON_SCOPE_EXIT { DeleteFolder(Folder); };
	const FString File = FPaths::Combine(Folder, FFlockDeviceIdFile::FileName);
	const FString Old = File + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
	const FString Fresh = File + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
	if (!TestTrue(TEXT("the old file is written"), FFileHelper::SaveStringToFile(TEXT("left over"), *Old))
		|| !TestTrue(TEXT("the fresh file is written"), FFileHelper::SaveStringToFile(TEXT("another start's file under way"), *Fresh)))
	{
		return false;
	}
	FlockMoveTestFileTimeBack(Old, 5.0);

	FStart Start(Folder);
	TestTrue(TEXT("a new id"), IsNewId(Start.Auth->ThisDeviceId()));
	TestFalse(TEXT("the old one is deleted"), IFileManager::Get().FileExists(*Old));
	TestTrue(TEXT("the fresh one is kept"), IFileManager::Get().FileExists(*Fresh));
	return true;
}

#if !UE_BUILD_SHIPPING
/** `Flock.LoginWithDevice` with no argument signs in as this install (ThisDeviceId); a device id given is sent as it is. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockThisDeviceIdConsoleTest, "Flock.Auth.ThisDeviceId.TheConsoleSignInUsesTheKeptIdWithoutAnArgument",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockThisDeviceIdConsoleTest::RunTest(const FString& Parameters)
{
	const FString Folder = NewFolder();
	ON_SCOPE_EXIT { DeleteFolder(Folder); };

	TestTrue(TEXT("no argument: an empty id"), FlockDevelopmentCommands::DeviceIdToSignInWith({}).IsEmpty());
	TestEqualSensitive(TEXT("an argument: as it is"), FlockDevelopmentCommands::DeviceIdToSignInWith({ TEXT(" a player ") }), FString(TEXT(" a player ")));

	// What the command then sends: an empty id is this install's.
	FStart Start(Folder);
	Start.Auth->LoginWithDevice(FlockDevelopmentCommands::DeviceIdToSignInWith({}), nullptr);
	const FString Kept = ReadText(FPaths::Combine(Folder, FFlockDeviceIdFile::FileName));
	TestTrue(FString::Printf(TEXT("the kept id was made: %s"), *Kept), IsNewId(Kept));
	TestDeviceBody(*this, TEXT("console login"), Start.LastBodyTo(FlockEndpoints::PlayerLoginDevice), Kept);
	return true;
}
#endif

#endif // WITH_AUTOMATION_TESTS
