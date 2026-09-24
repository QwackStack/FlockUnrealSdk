// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Misc/FlockEngineCompat.h"

#include "ProtokitePlaytestConfig.h"
#include "ProtokitePlaytestConsent.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/FlockTemporaryFiles.h"
#include "Misc/Paths.h"
#include "SProtokitePlaytestConsentWidget.h"

namespace
{
	/** A consent file in a folder of this test's own, removed when the test ends. */
	struct FConsentTestFile
	{
		FString Folder = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("ProtokitePlaytestTests"),
			FString::Printf(TEXT("Consent-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

		FString Path() const { return FPaths::Combine(Folder, TEXT("playtest_consent.json")); }

		FProtokitePlaytestConsentFile File() const { return FProtokitePlaytestConsentFile(Path()); }

		/** Puts Contents in the file, the way a build of another version, or a half-written save, could leave it. */
		void Write(const FString& Contents) const
		{
			FFileHelper::SaveStringToFile(Contents, *Path(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		~FConsentTestFile()
		{
			IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists*/ false, /*Tree*/ true);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestConsentAllowsTest, "Protokite.Playtest.Consent.WhatEachAnswerAllows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestConsentAllowsTest::RunTest(const FString& Parameters)
{
	using namespace ProtokitePlaytestConsent;

	TestTrue(TEXT("Everything allows the screen"), AllowsVideoRecording(EProtokitePlaytestConsentChoice::VideoAndPlayData));
	TestTrue(TEXT("Everything allows play data"), AllowsPlayData(EProtokitePlaytestConsentChoice::VideoAndPlayData));

	TestTrue(TEXT("Video only allows the screen"), AllowsVideoRecording(EProtokitePlaytestConsentChoice::VideoOnly));
	TestFalse(TEXT("Video only leaves play data out"), AllowsPlayData(EProtokitePlaytestConsentChoice::VideoOnly));

	TestFalse(TEXT("Play data only leaves the screen out"), AllowsVideoRecording(EProtokitePlaytestConsentChoice::PlayDataOnly));
	TestTrue(TEXT("Play data only allows play data"), AllowsPlayData(EProtokitePlaytestConsentChoice::PlayDataOnly));

	for (const EProtokitePlaytestConsentChoice Nothing : { EProtokitePlaytestConsentChoice::Nothing, EProtokitePlaytestConsentChoice::NotAnswered })
	{
		const FString Which = ToWire(Nothing);
		TestFalse(*(Which + TEXT(": no screen")), AllowsVideoRecording(Nothing));
		TestFalse(*(Which + TEXT(": no play data")), AllowsPlayData(Nothing));
		TestFalse(*(Which + TEXT(": nothing at all")), CollectsAnything(Nothing));
	}

	// The two are not the same answer, and only one of them is an answer.
	TestTrue(TEXT("Nothing is an answer"), IsAnswered(EProtokitePlaytestConsentChoice::Nothing));
	TestFalse(TEXT("Not answered is not"), IsAnswered(EProtokitePlaytestConsentChoice::NotAnswered));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestConsentFeaturesTest, "Protokite.Playtest.Consent.WhichFeaturesEachAnswerAllows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestConsentFeaturesTest::RunTest(const FString& Parameters)
{
	using namespace ProtokitePlaytestConsent;

	TestTrue(TEXT("Video only allows video recording"),
		AllowsFeature(EProtokitePlaytestConsentChoice::VideoOnly, ProtokitePlaytestFeatures::VideoRecording));
	TestFalse(TEXT("And not heavy analytics"),
		AllowsFeature(EProtokitePlaytestConsentChoice::VideoOnly, ProtokitePlaytestFeatures::HeavyAnalytics));
	TestFalse(TEXT("And not the exceptions a playtest asks for"),
		AllowsFeature(EProtokitePlaytestConsentChoice::VideoOnly, ProtokitePlaytestFeatures::ExceptionCapturing));

	TestFalse(TEXT("Play data only leaves video recording out"),
		AllowsFeature(EProtokitePlaytestConsentChoice::PlayDataOnly, ProtokitePlaytestFeatures::VideoRecording));
	TestTrue(TEXT("And allows heavy analytics"),
		AllowsFeature(EProtokitePlaytestConsentChoice::PlayDataOnly, ProtokitePlaytestFeatures::HeavyAnalytics));
	TestTrue(TEXT("And the exceptions a playtest asks for"),
		AllowsFeature(EProtokitePlaytestConsentChoice::PlayDataOnly, ProtokitePlaytestFeatures::ExceptionCapturing));

	// A feature name the server added after this build: nobody described it to the player, so only the answer that
	// allows everything allows it. Neither half is guessed at.
	const FString FeatureNobodyDescribed = TEXT("something_the_server_added");
	TestTrue(TEXT("Everything allows a feature this build does not know"),
		AllowsFeature(EProtokitePlaytestConsentChoice::VideoAndPlayData, FeatureNobodyDescribed));
	TestFalse(TEXT("Video only does not"), AllowsFeature(EProtokitePlaytestConsentChoice::VideoOnly, FeatureNobodyDescribed));
	TestFalse(TEXT("Play data only does not"), AllowsFeature(EProtokitePlaytestConsentChoice::PlayDataOnly, FeatureNobodyDescribed));
	TestFalse(TEXT("And nothing does not"), AllowsFeature(EProtokitePlaytestConsentChoice::Nothing, FeatureNobodyDescribed));

	// A feature name is the server's spelling, letter for letter, exactly as the config's own switches are read.
	TestFalse(TEXT("A feature name in another letter case is not the video feature"),
		AllowsFeature(EProtokitePlaytestConsentChoice::VideoOnly, TEXT("Video_Recording")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestConsentWireTest, "Protokite.Playtest.Consent.WireSpellings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestConsentWireTest::RunTest(const FString& Parameters)
{
	using namespace ProtokitePlaytestConsent;

	// The spellings a saved answer and a session start both hold, checked letter for letter: Unreal's ordinary string
	// checks ignore letter case, and these are read by a server and by the next launch.
	TestEqualSensitive(TEXT("Everything"), *ToWire(EProtokitePlaytestConsentChoice::VideoAndPlayData), TEXT("video_and_play_data"));
	TestEqualSensitive(TEXT("Video only"), *ToWire(EProtokitePlaytestConsentChoice::VideoOnly), TEXT("video_only"));
	TestEqualSensitive(TEXT("Play data only"), *ToWire(EProtokitePlaytestConsentChoice::PlayDataOnly), TEXT("play_data_only"));
	TestEqualSensitive(TEXT("Nothing"), *ToWire(EProtokitePlaytestConsentChoice::Nothing), TEXT("nothing"));
	TestEqualSensitive(TEXT("Not answered"), *ToWire(EProtokitePlaytestConsentChoice::NotAnswered), TEXT("not_answered"));

	for (const EProtokitePlaytestConsentChoice Choice : { EProtokitePlaytestConsentChoice::NotAnswered,
		EProtokitePlaytestConsentChoice::VideoAndPlayData, EProtokitePlaytestConsentChoice::VideoOnly,
		EProtokitePlaytestConsentChoice::PlayDataOnly, EProtokitePlaytestConsentChoice::Nothing })
	{
		TestEqual(*FString::Printf(TEXT("%s goes out and comes back"), *ToWire(Choice)),
			static_cast<int32>(FromWire(ToWire(Choice))), static_cast<int32>(Choice));
		TestFalse(*FString::Printf(TEXT("%s is described"), *ToWire(Choice)), Describe(Choice).IsEmpty());
	}

	// Anything else is an answer this build cannot be sure of, so the player is asked again rather than having one read
	// into it -- a different letter case included.
	for (const TCHAR* Unreadable : { TEXT(""), TEXT("Video_Only"), TEXT("VIDEO_ONLY"), TEXT("everything"), TEXT("yes") })
	{
		TestEqual(*FString::Printf(TEXT("'%s' is not an answer"), Unreadable), static_cast<int32>(FromWire(Unreadable)),
			static_cast<int32>(EProtokitePlaytestConsentChoice::NotAnswered));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestConsentFileTest, "Protokite.Playtest.Consent.TheAnswerIsKeptForTheNextLaunch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestConsentFileTest::RunTest(const FString& Parameters)
{
	const FConsentTestFile Scratch;

	TestEqual(TEXT("With no file, nobody has answered"), static_cast<int32>(Scratch.File().Read()),
		static_cast<int32>(EProtokitePlaytestConsentChoice::NotAnswered));

	for (const EProtokitePlaytestConsentChoice Choice : { EProtokitePlaytestConsentChoice::VideoAndPlayData,
		EProtokitePlaytestConsentChoice::VideoOnly, EProtokitePlaytestConsentChoice::PlayDataOnly, EProtokitePlaytestConsentChoice::Nothing })
	{
		const FString Which = ProtokitePlaytestConsent::ToWire(Choice);
		TestTrue(*(Which + TEXT(" is saved")), Scratch.File().Save(Choice));
		// Read through a second file object, the way the next launch reads it: nothing is remembered in memory.
		TestEqual(*(Which + TEXT(" is what a later launch reads")), static_cast<int32>(Scratch.File().Read()),
			static_cast<int32>(Choice));
	}

	// Forgetting the answer takes the file away, so the question is put again rather than the file holding "no".
	TestTrue(TEXT("The answer is forgotten"), Scratch.File().Save(EProtokitePlaytestConsentChoice::NotAnswered));
	TestFalse(TEXT("The file is gone"), IFileManager::Get().FileExists(*Scratch.Path()));
	TestEqual(TEXT("And nobody has answered"), static_cast<int32>(Scratch.File().Read()),
		static_cast<int32>(EProtokitePlaytestConsentChoice::NotAnswered));
	return true;
}

/**
 * A save that cannot be written takes the older answer with it. Leaving it would have the next launch collect under an
 * answer this player has replaced -- silently, because a file that holds an answer is never questioned again.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestConsentFailedSaveTest, "Protokite.Playtest.Consent.AFailedSaveForgetsTheOlderAnswer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestConsentFailedSaveTest::RunTest(const FString& Parameters)
{
	const FConsentTestFile Scratch;
	if (!TestTrue(TEXT("Precondition: they had allowed everything"),
		Scratch.File().Save(EProtokitePlaytestConsentChoice::VideoAndPlayData)))
	{
		return false;
	}

	// A file the save cannot write over, which is what a full disk or a file another program holds comes to.
	IPlatformFile& Disk = FPlatformFileManager::Get().GetPlatformFile();
	Disk.SetReadOnly(*Scratch.Path(), true);
	const bool bSaved = Scratch.File().Save(EProtokitePlaytestConsentChoice::Nothing);
	Disk.SetReadOnly(*Scratch.Path(), false);

	if (bSaved)
	{
		// The platform let the save through after all; then the answer is simply theirs, and there is nothing to forget.
		TestEqual(TEXT("Their new answer is what the file holds"), static_cast<int32>(Scratch.File().Read()),
			static_cast<int32>(EProtokitePlaytestConsentChoice::Nothing));
		return true;
	}

	TestEqual(TEXT("The older answer is gone, so the next launch asks rather than collecting under it"),
		static_cast<int32>(Scratch.File().Read()), static_cast<int32>(EProtokitePlaytestConsentChoice::NotAnswered));
	TestFalse(TEXT("And nothing on disk still holds it"), IFileManager::Get().FileExists(*Scratch.Path()));
	return true;
}

/** Forgetting an answer takes its temporary files with it, so no copy of it is left beside the file that lost it. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestConsentForgetsTemporaryFilesTest,
	"Protokite.Playtest.Consent.ForgettingLeavesNoCopyBehind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestConsentForgetsTemporaryFilesTest::RunTest(const FString& Parameters)
{
	const FConsentTestFile Scratch;
	Scratch.File().Save(EProtokitePlaytestConsentChoice::VideoAndPlayData);

	// A temporary file of a save from moments ago, the way a killed launch leaves one. It is too fresh for the
	// ordinary leftover sweep, which only takes files a minute old.
	FFlockTemporaryFiles::SaveThenMove(TEXT("{\"playtest_consent\":\"video_and_play_data\"}"),
		Scratch.Path() + TEXT(".copy"), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FFileHelper::SaveStringToFile(TEXT("{\"playtest_consent\":\"video_and_play_data\"}"),
		*(Scratch.Path() + TEXT(".0123456789abcdef0123456789abcdef.tmp")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	TestTrue(TEXT("The answer is forgotten"), Scratch.File().Save(EProtokitePlaytestConsentChoice::NotAnswered));

	TestEqual(TEXT("And no temporary file of it is left"), FFlockTemporaryFiles::FindTemporaryFilesOf(Scratch.Path()).Num(), 0);
	TestEqual(TEXT("Nor does anything read as an answer"), static_cast<int32>(Scratch.File().Read()),
		static_cast<int32>(EProtokitePlaytestConsentChoice::NotAnswered));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestConsentUnreadableFileTest, "Protokite.Playtest.Consent.AFileItCannotReadAsksAgain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestConsentUnreadableFileTest::RunTest(const FString& Parameters)
{
	const FConsentTestFile Scratch;

	// Every way the file can go wrong reads as nobody having answered, which is the safe direction: the player is asked
	// again and nothing is collected meanwhile. Guessing an answer would collect from somebody who never gave one.
	const TArray<TPair<FString, FString>> Unreadable = {
		{ TEXT("an empty file"), TEXT("") },
		{ TEXT("half a file, the way a killed launch leaves one"), TEXT("{\"playtest_consent\":\"video_") },
		{ TEXT("a file with no answer in it"), TEXT("{\"answered_at\":\"2026-09-21T00:00:00Z\"}") },
		{ TEXT("an answer this build does not know"), TEXT("{\"playtest_consent\":\"video_and_my_microphone\"}") },
		{ TEXT("an answer in another letter case"), TEXT("{\"playtest_consent\":\"Video_Only\"}") },
	};
	for (const TPair<FString, FString>& Case : Unreadable)
	{
		Scratch.Write(Case.Value);
		TestEqual(*Case.Key, static_cast<int32>(Scratch.File().Read()),
			static_cast<int32>(EProtokitePlaytestConsentChoice::NotAnswered));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestConsentOptionsTest, "Protokite.Playtest.Consent.EveryAnswerIsOffered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestConsentOptionsTest::RunTest(const FString& Parameters)
{
	const TArrayView<const FProtokitePlaytestConsentOption> Offered = SProtokitePlaytestConsentWidget::GetOptions();

	// Every answer a player can give is on the question, and only those: one they cannot pick would be a rule with no
	// way to reach it, and a missing one would leave them unable to say it.
	TArray<EProtokitePlaytestConsentChoice> Expected = { EProtokitePlaytestConsentChoice::VideoAndPlayData,
		EProtokitePlaytestConsentChoice::VideoOnly, EProtokitePlaytestConsentChoice::PlayDataOnly, EProtokitePlaytestConsentChoice::Nothing };
	if (TestEqual(TEXT("Four answers are offered"), Offered.Num(), Expected.Num()))
	{
		for (int32 Index = 0; Index < Offered.Num(); ++Index)
		{
			TestEqual(*FString::Printf(TEXT("Answer %d"), Index), static_cast<int32>(Offered[Index].Choice),
				static_cast<int32>(Expected[Index]));
			TestTrue(*FString::Printf(TEXT("Answer %d has words of its own"), Index),
				FCString::Strlen(Offered[Index].Title) > 0 && FCString::Strlen(Offered[Index].Explanation) > 0);
		}
	}
	return true;
}

/**
 * The question is the playtest's own, and its words say so. A player who is told "we collect data" and is then recorded
 * has been misled, so the wording is pinned rather than left to be tidied away.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestConsentWordingTest, "Protokite.Playtest.Consent.ItsWordsKeepItSeparate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestConsentWordingTest::RunTest(const FString& Parameters)
{
	const TArrayView<const FProtokitePlaytestConsentOption> Offered = SProtokitePlaytestConsentWidget::GetOptions();

	const FProtokitePlaytestConsentOption* Nothing = Offered.FindByPredicate(
		[](const FProtokitePlaytestConsentOption& Option) { return Option.Choice == EProtokitePlaytestConsentChoice::Nothing; });
	if (TestNotNull(TEXT("Collecting nothing is offered"), Nothing))
	{
		// It has to say what it really does, which is everything a build with playtesting turned off does.
		TestTrue(TEXT("And says it collects nothing at all"), FString(Nothing->Explanation).Contains(TEXT("nothing at all")));
	}

	const FProtokitePlaytestConsentOption* Video = Offered.FindByPredicate(
		[](const FProtokitePlaytestConsentOption& Option) { return Option.Choice == EProtokitePlaytestConsentChoice::VideoOnly; });
	if (TestNotNull(TEXT("Recording the screen only is offered"), Video))
	{
		TestTrue(TEXT("And says the screen is recorded"), FString(Video->Explanation).Contains(TEXT("on screen")));
	}
	return true;
}

#endif
