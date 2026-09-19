// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockPlaytestIdentity.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeCounter.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemModule.h"
#include "OnlineSubsystemNames.h"

namespace
{
	/** A folder of its own for one test, removed when the test ends. */
	struct FScopedTestFolder
	{
		FString Path = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockPlaytestTests"),
			FGuid::NewGuid().ToString(EGuidFormats::Digits));

		~FScopedTestFolder()
		{
			IFileManager::Get().DeleteDirectory(*Path, /*bRequireExists*/ false, /*bTree*/ true);
		}

		/** The names of the files directly inside the folder. */
		TArray<FString> Files() const
		{
			TArray<FString> Found;
			IFileManager::Get().FindFiles(Found, *FPaths::Combine(Path, TEXT("*")), /*bFiles*/ true, /*bDirectories*/ false);
			return Found;
		}
	};

	bool IsLowerCaseGuid(const FString& Text)
	{
		FGuid Guid;
		return FGuid::Parse(Text, Guid) && Guid.IsValid()
			&& Guid.ToString(EGuidFormats::DigitsWithHyphensLower).Equals(Text, ESearchCase::CaseSensitive);
	}

	void ExpectDeviceIdFileResult(FAutomationTestBase& Test, const FString& What, EFlockDeviceIdFileResult Actual,
		EFlockDeviceIdFileResult Expected)
	{
		Test.TestEqual(What, static_cast<int32>(Actual), static_cast<int32>(Expected));
	}

	/** Stands in for the Steam plugin's factory and counts how often a Steam subsystem is asked for. It creates nothing. */
	class FCountingSteamFactory : public IOnlineFactory
	{
	public:
		int32 TimesAsked = 0;

		virtual IOnlineSubsystemPtr CreateSubsystem(FName InstanceName) override
		{
			++TimesAsked;
			return nullptr;
		}
	};

	const TCHAR* const SomeDeviceId = TEXT("0f8fad5b-d9cb-469f-a165-70867728950e");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestIdentityDeviceIdFileKeepsItsIdTest,
	"Flock.Playtest.Identity.DeviceIdFileKeepsItsId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestIdentityDeviceIdFileKeepsItsIdTest::RunTest(const FString& Parameters)
{
	FScopedTestFolder Folder;
	const FString Path = FPaths::Combine(Folder.Path, TEXT("device_id.txt"));

	FString First;
	ExpectDeviceIdFileResult(*this, TEXT("With no file yet, one is created"), FFlockPlaytestDeviceIdFile(Path).ReadOrCreate(First),
		EFlockDeviceIdFileResult::Created);
	TestTrue(TEXT("The id is a lower-case GUID"), IsLowerCaseGuid(First));
	FString OnDisk;
	FFileHelper::LoadFileToString(OnDisk, *Path);
	TestEqual(TEXT("It is saved exactly, with nothing around it"), OnDisk, First);

	// A new object reading the same file is what the next launch does.
	FString Second;
	ExpectDeviceIdFileResult(*this, TEXT("The next launch reads the file"), FFlockPlaytestDeviceIdFile(Path).ReadOrCreate(Second),
		EFlockDeviceIdFileResult::Read);
	TestEqual(TEXT("And gets the same id"), Second, First);
	TestEqual(TEXT("No temporary file is left behind"), Folder.Files().Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestIdentityFileWithoutADeviceIdIsReplacedTest,
	"Flock.Playtest.Identity.FileWithoutADeviceIdIsReplaced",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestIdentityFileWithoutADeviceIdIsReplacedTest::RunTest(const FString& Parameters)
{
	// Only the exact form this plugin writes counts. Whitespace is never trimmed away, and a nil GUID is no id.
	const FString NotDeviceIds[] = {
		FString(),
		TEXT("not a device id"),
		FString(SomeDeviceId) + TEXT("\n"),
		FString(TEXT(" ")) + SomeDeviceId,
		FString(SomeDeviceId).ToUpper(),
		TEXT("00000000-0000-0000-0000-000000000000"),
	};
	for (const FString& NotDeviceId : NotDeviceIds)
	{
		FScopedTestFolder Folder;
		const FString Path = FPaths::Combine(Folder.Path, TEXT("device_id.txt"));
		FFileHelper::SaveStringToFile(NotDeviceId, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

		FString DeviceId;
		const FString What = FString::Printf(TEXT("'%s'"), *NotDeviceId.ReplaceCharWithEscapedChar());
		ExpectDeviceIdFileResult(*this, What + TEXT(" is replaced"), FFlockPlaytestDeviceIdFile(Path).ReadOrCreate(DeviceId),
			EFlockDeviceIdFileResult::Replaced);
		TestTrue(What + TEXT(": the new id is a lower-case GUID"), IsLowerCaseGuid(DeviceId));
		TestNotEqual(What + TEXT(": the new id is not the old one tidied up"), DeviceId, FString(SomeDeviceId));
		FString OnDisk;
		FFileHelper::LoadFileToString(OnDisk, *Path);
		TestEqual(What + TEXT(": the file now holds the new id"), OnDisk, DeviceId);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestIdentityUnreadableFileIsLeftAloneTest,
	"Flock.Playtest.Identity.UnreadableFileIsLeftAlone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestIdentityUnreadableFileIsLeftAloneTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS
	FScopedTestFolder Folder;
	const FString Path = FPaths::Combine(Folder.Path, TEXT("device_id.txt"));
	FFileHelper::SaveStringToFile(SomeDeviceId, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	{
		// Held open for writing, without letting anyone read it, the way another program might hold it for a moment.
		const TUniquePtr<FArchive> Holder(IFileManager::Get().CreateFileWriter(*Path, FILEWRITE_Append));
		if (TestNotNull(TEXT("The file is held open"), Holder.Get()))
		{
			FString DeviceId;
			ExpectDeviceIdFileResult(*this, TEXT("A file that cannot be read"), FFlockPlaytestDeviceIdFile(Path).ReadOrCreate(DeviceId),
				EFlockDeviceIdFileResult::Unreadable);
			TestTrue(TEXT("Gives no device id"), DeviceId.IsEmpty());
		}
	}
	FString OnDisk;
	FFileHelper::LoadFileToString(OnDisk, *Path);
	TestEqual(TEXT("And is left exactly as it was"), OnDisk, FString(SomeDeviceId));
#else
	AddInfo(TEXT("Holding a file open so it cannot be read is only arranged on Windows."));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestIdentityDeviceIdThatCannotBeSavedIsNotUsedTest,
	"Flock.Playtest.Identity.DeviceIdThatCannotBeSavedIsNotUsed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestIdentityDeviceIdThatCannotBeSavedIsNotUsedTest::RunTest(const FString& Parameters)
{
	FScopedTestFolder Folder;
	// A file stands where the device id file's folder would be, so nothing can be saved there.
	const FString Blocker = FPaths::Combine(Folder.Path, TEXT("blocker"));
	FFileHelper::SaveStringToFile(TEXT("x"), *Blocker);

	FString DeviceId;
	ExpectDeviceIdFileResult(*this, TEXT("An id that cannot be saved"),
		FFlockPlaytestDeviceIdFile(FPaths::Combine(Blocker, TEXT("device_id.txt"))).ReadOrCreate(DeviceId),
		EFlockDeviceIdFileResult::CouldNotSave);
	TestTrue(TEXT("Is not used"), DeviceId.IsEmpty());
	TestEqual(TEXT("And nothing is left behind"), Folder.Files().Num(), 1);
	return true;
}

namespace
{
	/** Counts the file manager's warnings and errors while it exists. */
	class FFileManagerComplaintCounter : public FOutputDevice
	{
	public:
		FFileManagerComplaintCounter()
		{
			GLog->AddOutputDevice(this);
		}

		virtual ~FFileManagerComplaintCounter() override
		{
			GLog->RemoveOutputDevice(this);
		}

		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Category == FileManagerCategory && (Verbosity & ELogVerbosity::VerbosityMask) <= ELogVerbosity::Warning)
			{
				Complaints.Increment();
			}
		}

		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		int32 Count()
		{
			GLog->Flush();
			return Complaints.GetValue();
		}

	private:
		const FName FileManagerCategory = TEXT("LogFileManager");
		FThreadSafeCounter Complaints;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestIdentityFailedSaveGivesUpAtOnceTest,
	"Flock.Playtest.Identity.FailedSaveGivesUpAtOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestIdentityFailedSaveGivesUpAtOnceTest::RunTest(const FString& Parameters)
{
	FScopedTestFolder Folder;
	const FString Path = FPaths::Combine(Folder.Path, TEXT("device_id.txt"));
	// A folder stands where the file goes, so a new id can be written beside it but never moved into place.
	IFileManager::Get().MakeDirectory(*Path, /*bTree*/ true);

	FString DeviceId;
	const double StartedAt = FPlatformTime::Seconds();
	EFlockDeviceIdFileResult Result;
	int32 Complaints = 0;
	{
		FFileManagerComplaintCounter Counter;
		Result = FFlockPlaytestDeviceIdFile(Path).ReadOrCreate(DeviceId);
		Complaints = Counter.Count();
	}
	const double Seconds = FPlatformTime::Seconds() - StartedAt;

	ExpectDeviceIdFileResult(*this, TEXT("An id that cannot be moved into place"), Result, EFlockDeviceIdFileResult::CouldNotSave);
	TestTrue(TEXT("Is not used"), DeviceId.IsEmpty());
	// A move that retries takes five seconds. One that gives up at once finishes well inside this even when the disk holds a
	// single write for a second or two, which Windows Defender does on this machine: 0.4 s failed once at 1.38 s.
	const double SecondsWithoutARetry = 2.5;
	TestTrue(FString::Printf(TEXT("It gives up at once instead of retrying on the game thread (took %.2f s)"), Seconds),
		Seconds < SecondsWithoutARetry);
	TestEqual(TEXT("The file manager logs no warning or error about it"), Complaints, 0);
	TestEqual(TEXT("The id written beside it is removed"), Folder.Files().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestIdentityStrayTemporaryFilesAreSweptTest,
	"Flock.Playtest.Identity.StrayTemporaryFilesAreSwept",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestIdentityStrayTemporaryFilesAreSweptTest::RunTest(const FString& Parameters)
{
	FScopedTestFolder Folder;
	const FString Path = FPaths::Combine(Folder.Path, TEXT("device_id.txt"));
	FFileHelper::SaveStringToFile(SomeDeviceId, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	// One left by a launch that crashed ten minutes ago, one a launch may be about to move into place, and one that is
	// not the device id file's at all.
	const FString Stale = Path + TEXT(".0123456789abcdef0123456789abcdef.tmp");
	const FString Fresh = Path + TEXT(".fedcba9876543210fedcba9876543210.tmp");
	const FString Unrelated = FPaths::Combine(Folder.Path, TEXT("other.tmp"));
	for (const FString& File : { Stale, Fresh, Unrelated })
	{
		FFileHelper::SaveStringToFile(SomeDeviceId, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
	IFileManager::Get().SetTimeStamp(*Stale, FDateTime::UtcNow() - FTimespan::FromMinutes(10.0));
	IFileManager::Get().SetTimeStamp(*Unrelated, FDateTime::UtcNow() - FTimespan::FromMinutes(10.0));

	FString DeviceId;
	ExpectDeviceIdFileResult(*this, TEXT("The id is read as usual"), FFlockPlaytestDeviceIdFile(Path).ReadOrCreate(DeviceId),
		EFlockDeviceIdFileResult::Read);
	TestFalse(TEXT("An old temporary file is removed"), IFileManager::Get().FileExists(*Stale));
	TestTrue(TEXT("A fresh one is left for the launch that may be saving it"), IFileManager::Get().FileExists(*Fresh));
	TestTrue(TEXT("A file that is not the device id file's is left alone"), IFileManager::Get().FileExists(*Unrelated));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestIdentityUsableIdsTest,
	"Flock.Playtest.Identity.UsableIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestIdentityUsableIdsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("A Steam id"), IsUsablePlaytestId(TEXT("76561198000000001"), FlockPlaytestIdentityLimits::SteamIdLength));
	TestTrue(TEXT("Exactly the longest length"), IsUsablePlaytestId(FString::ChrN(64, TEXT('7')), 64));
	TestFalse(TEXT("One character longer"), IsUsablePlaytestId(FString::ChrN(65, TEXT('7')), 64));
	TestFalse(TEXT("Empty"), IsUsablePlaytestId(FString(), 64));
	TestFalse(TEXT("A leading space"), IsUsablePlaytestId(TEXT(" 76561198000000001"), 64));
	TestFalse(TEXT("A trailing line break"), IsUsablePlaytestId(TEXT("76561198000000001\n"), 64));
	TestFalse(TEXT("A space inside"), IsUsablePlaytestId(TEXT("7656119 8000000001"), 64));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPlaytestIdentityReadingSteamNeverCreatesItTest,
	"Flock.Playtest.Identity.ReadingSteamNeverCreatesIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockPlaytestIdentityReadingSteamNeverCreatesItTest::RunTest(const FString& Parameters)
{
	FOnlineSubsystemModule& OnlineSubsystems = FModuleManager::LoadModuleChecked<FOnlineSubsystemModule>(TEXT("OnlineSubsystem"));
	if (IOnlineSubsystem::IsLoaded(STEAM_SUBSYSTEM) || IOnlineSubsystem::DoesInstanceExist(STEAM_SUBSYSTEM))
	{
		AddInfo(TEXT("The Steam plugin is loaded in this process, so its factory cannot be stood in for."));
		return true;
	}

	// The engine's own config switches the Steam subsystem off, and then asking for it by name creates nothing anyway.
	// A game that ships on Steam switches it on, which is when asking would start Steam, so the test does the same.
	static const TCHAR* const SteamSection = TEXT("OnlineSubsystemSteam");
	bool bSteamWasEnabled = false;
	const bool bSteamSettingExisted = GConfig->GetBool(SteamSection, TEXT("bEnabled"), bSteamWasEnabled, GEngineIni);
	GConfig->SetBool(SteamSection, TEXT("bEnabled"), true, GEngineIni);

	FCountingSteamFactory Factory;
	OnlineSubsystems.RegisterPlatformService(STEAM_SUBSYSTEM, &Factory);
	const FFlockRunningSteamAccount Account = ReadRunningSteamAccount();
	const int32 AskedByTheReader = Factory.TimesAsked;
	// Control: asking for Steam by name does reach the stand-in, so a reader that asked would have been counted.
	IOnlineSubsystem::Get(STEAM_SUBSYSTEM);
	const int32 AskedByTheControl = Factory.TimesAsked - AskedByTheReader;
	OnlineSubsystems.UnregisterPlatformService(STEAM_SUBSYSTEM);

	if (bSteamSettingExisted)
	{
		GConfig->SetBool(SteamSection, TEXT("bEnabled"), bSteamWasEnabled, GEngineIni);
	}
	else
	{
		GConfig->RemoveKey(SteamSection, TEXT("bEnabled"), GEngineIni);
	}

	TestEqual(TEXT("Reading asks for no Steam subsystem"), AskedByTheReader, 0);
	TestTrue(TEXT("And reads no account"), Account.Id.IsEmpty() && Account.Nickname.IsEmpty());
	TestEqual(TEXT("Control: asking by name reaches the stand-in"), AskedByTheControl, 1);
	TestFalse(TEXT("No Steam subsystem exists afterwards"), IOnlineSubsystem::DoesInstanceExist(STEAM_SUBSYSTEM));
	return true;
}

#endif
