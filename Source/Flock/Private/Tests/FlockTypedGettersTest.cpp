// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Models/FlockGameModels.h"
#include "Models/FlockJsonData.h"
#include "Models/FlockStructuredData.h"

namespace FlockTypedGettersTestHelpers
{
	/** The calls into the engine's Json and JsonUtilities modules that compile into whoever instantiates or inlines them. */
	const TCHAR* const JsonModuleCalls[] = {
		TEXT("FJsonObjectConverter::"),
		TEXT("FJsonSerializer::"),
		TEXT("TJsonReaderFactory"),
		TEXT("TJsonWriterFactory"),
	};

	/** This module's folder (Source/Flock), found from this file's own path. */
	inline FString FlockModuleFolder()
	{
		const FString TestsFolder = FPaths::GetPath(FString(ANSI_TO_TCHAR(__FILE__)));
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(TestsFolder, TEXT(".."), TEXT("..")));
	}

	/** Every code line (comments left out) of the headers under Folder that holds one of Calls, as "file:line". */
	inline TArray<FString> LinesCalling(const FString& Folder, TArrayView<const TCHAR* const> Calls, const FString& LeaveOutFolder = FString())
	{
		TArray<FString> Headers;
		IFileManager::Get().FindFilesRecursive(Headers, *Folder, TEXT("*.h"), /*Files*/ true, /*Directories*/ false);
		TArray<FString> Found;
		for (const FString& Header : Headers)
		{
			const FString Normalized = FPaths::ConvertRelativePathToFull(Header).Replace(TEXT("\\"), TEXT("/"));
			if (!LeaveOutFolder.IsEmpty() && Normalized.StartsWith(LeaveOutFolder, ESearchCase::IgnoreCase))
			{
				continue;
			}
			TArray<FString> Lines;
			FFileHelper::LoadFileToStringArray(Lines, *Header);
			for (int32 Index = 0; Index < Lines.Num(); ++Index)
			{
				// A comment line; a code line can also start with '*' (`*OutValue = ...`), so only "*", "* ..." and "*/" count.
				const FString Code = Lines[Index].TrimStartAndEnd();
				const bool bComment = Code.StartsWith(TEXT("//")) || Code.StartsWith(TEXT("/*")) || Code == TEXT("*")
					|| Code.StartsWith(TEXT("* ")) || Code.StartsWith(TEXT("*/"));
				if (bComment)
				{
					continue;
				}
				for (const TCHAR* Call : Calls)
				{
					if (Code.Contains(Call, ESearchCase::CaseSensitive))
					{
						Found.Add(FString::Printf(TEXT("%s:%d"), *FPaths::GetCleanFilename(Header), Index + 1));
						break;
					}
				}
			}
		}
		return Found;
	}
}

using namespace FlockTypedGettersTestHelpers;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockJsonDataGetAsTest, "Flock.Models.JsonData.GetAsFillsAStruct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockJsonDataGetAsTest::RunTest(const FString& Parameters)
{
	FFlockJsonData Data;
	Data.Json = TEXT("{\"Id\":\"payload-id\",\"ReleaseType\":\"prod\"}");

	FFlockGameVersionSchema Bound;
	TestTrue(TEXT("GetAs binds"), Data.GetAs(Bound));
	TestEqualSensitive(TEXT("Id bound"), Bound.Id, FString(TEXT("payload-id")));
	TestEqualSensitive(TEXT("ReleaseType bound"), Bound.ReleaseType, FString(TEXT("prod")));

	FFlockGameVersionSchema AtRunTime;
	TestTrue(TEXT("a type known at run time binds the same"), Data.GetAsStruct(FFlockGameVersionSchema::StaticStruct(), &AtRunTime));
	TestEqualSensitive(TEXT("run-time Id bound"), AtRunTime.Id, FString(TEXT("payload-id")));

	TestFalse(TEXT("no type is refused"), Data.GetAsStruct(nullptr, &AtRunTime));
	TestFalse(TEXT("no target is refused"), Data.GetAsStruct(FFlockGameVersionSchema::StaticStruct(), nullptr));

	FFlockJsonData Empty;
	FFlockGameVersionSchema Untouched;
	TestFalse(TEXT("no data reads nothing"), Empty.GetAs(Untouched));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockStructuredDataGetDataAsStructTest, "Flock.Models.StructuredData.GetDataAsStructTakesARunTimeType",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockStructuredDataGetDataAsStructTest::RunTest(const FString& Parameters)
{
	FFlockStructuredData Data;
	Data.FlatJson = TEXT("{\"Id\":\"structured-id\",\"ReleaseType\":\"beta\"}");

	FFlockGameVersionSchema AtRunTime;
	TestTrue(TEXT("a type known at run time binds"), Data.GetDataAsStruct(FFlockGameVersionSchema::StaticStruct(), &AtRunTime));
	TestEqualSensitive(TEXT("Id bound"), AtRunTime.Id, FString(TEXT("structured-id")));
	TestEqualSensitive(TEXT("ReleaseType bound"), AtRunTime.ReleaseType, FString(TEXT("beta")));

	TestFalse(TEXT("no type is refused"), Data.GetDataAsStruct(nullptr, &AtRunTime));
	TestFalse(TEXT("no target is refused"), Data.GetDataAsStruct(FFlockGameVersionSchema::StaticStruct(), nullptr));

	FFlockStructuredData Empty;
	TestFalse(TEXT("no data reads nothing"), Empty.GetDataAsStruct(FFlockGameVersionSchema::StaticStruct(), &AtRunTime));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockPublicHeadersJsonModulesTest, "Flock.Build.GameFacingHeadersCallNoJsonModuleInline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockPublicHeadersJsonModulesTest::RunTest(const FString& Parameters)
{
	// A call into Json or JsonUtilities in a public template or inline function compiles into the game's own module, and
	// a game module listing Flock (private or public) does not link those modules, so its editor build fails to link.
	// Only the HTTP layer (Public/Http), which code extending Flock's requests builds on, may do it.
	const FString Module = FlockModuleFolder();
	const FString HttpFolder = FPaths::Combine(Module, TEXT("Public"), TEXT("Http")).Replace(TEXT("\\"), TEXT("/")) + TEXT("/");

	// Control: each name, alone, is found in the JSON utilities, so a name that matches nothing cannot pass.
	for (const TCHAR* Call : JsonModuleCalls)
	{
		const TArray<FString> InHttp = LinesCalling(FPaths::Combine(Module, TEXT("Public"), TEXT("Http")), MakeArrayView(&Call, 1));
		TestTrue(FString::Printf(TEXT("control: the scan finds %s in the JSON utilities"), Call),
			InHttp.ContainsByPredicate([](const FString& Where) { return Where.StartsWith(TEXT("FlockJsonUtils.h:"), ESearchCase::CaseSensitive); }));
	}

	const TArray<FString> GameFacing = LinesCalling(FPaths::Combine(Module, TEXT("Public")), JsonModuleCalls, HttpFolder);
	TestEqual(TEXT("game-facing Flock headers that call a JSON module inline"), GameFacing.Num(), 0);
	for (const FString& Where : GameFacing)
	{
		AddError(FString::Printf(TEXT("%s calls Json or JsonUtilities inline; move the call into a Flock .cpp"), *Where));
	}

	const FString EditorPublic = FPaths::ConvertRelativePathToFull(FPaths::Combine(Module, TEXT(".."), TEXT("FlockEditor"), TEXT("Public")));
	const TArray<FString> EditorFacing = LinesCalling(EditorPublic, JsonModuleCalls);
	TestEqual(TEXT("FlockEditor's public headers that call a JSON module inline"), EditorFacing.Num(), 0);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
