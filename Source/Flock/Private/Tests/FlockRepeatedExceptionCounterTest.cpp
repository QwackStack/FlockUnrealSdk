// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Analytics/FlockRepeatedExceptionCounter.h"
#include "Tests/Support/FlockTestSafeIndex.h"

namespace FlockRepeatedExceptionCounterTestHelpers
{
	/** The details a captured fault carries, reduced to its category. */
	inline FFlockLogDetails DetailsForCategory(const TCHAR* Category)
	{
		FFlockLogDetails Details;
		Details.ExtraData.Add(TEXT("category"), Category);
		return Details;
	}
}

using namespace FlockRepeatedExceptionCounterTestHelpers;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRepeatedExceptionCounterSameFaultKeyTest, "Flock.Analytics.RepeatedExceptionCounter.SameFaultKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockRepeatedExceptionCounterSameFaultKeyTest::RunTest(const FString& Parameters)
{
	const FName Game(TEXT("LogGame"));
	const FString Stack = TEXT("UnrealEditor-Game.dll+0x1a2b\nUnrealEditor-Game.dll+0x3c4d\nUnrealEditor-Engine.dll+0x5e6f");

	// Numbers and addresses change between occurrences of one fault; the key must not.
	TestEqual(TEXT("digit runs are ignored"),
		FFlockRepeatedExceptionCounter::MakeSameFaultKey(Game, TEXT("Index 7 out of range 12"), Stack),
		FFlockRepeatedExceptionCounter::MakeSameFaultKey(Game, TEXT("Index 130 out of range 4"), Stack));
	TestEqual(TEXT("0x addresses are ignored, hex letters included"),
		FFlockRepeatedExceptionCounter::MakeSameFaultKey(Game, TEXT("Bad pointer 0x7ffdeadbeef"), Stack),
		FFlockRepeatedExceptionCounter::MakeSameFaultKey(Game, TEXT("Bad pointer 0x1a"), Stack));

	// What does tell faults apart must still split them.
	TestNotEqual(TEXT("different words differ"),
		FFlockRepeatedExceptionCounter::MakeSameFaultKey(Game, TEXT("Index 7 out of range"), Stack),
		FFlockRepeatedExceptionCounter::MakeSameFaultKey(Game, TEXT("Key 7 not found"), Stack));
	TestNotEqual(TEXT("different categories differ"),
		FFlockRepeatedExceptionCounter::MakeSameFaultKey(Game, TEXT("boom"), Stack),
		FFlockRepeatedExceptionCounter::MakeSameFaultKey(FName(TEXT("LogScript")), TEXT("boom"), Stack));
	TestNotEqual(TEXT("different call sites differ"),
		FFlockRepeatedExceptionCounter::MakeSameFaultKey(Game, TEXT("boom"), Stack),
		FFlockRepeatedExceptionCounter::MakeSameFaultKey(Game, TEXT("boom"), TEXT("UnrealEditor-Game.dll+0x9999\nUnrealEditor-Game.dll+0x3c4d")));

	// Only the first frames count, so a deeper frame cannot split one bug into many.
	TestEqual(TEXT("frames past the second are ignored"),
		FFlockRepeatedExceptionCounter::MakeSameFaultKey(Game, TEXT("boom"), TEXT("A.dll+0x1\nA.dll+0x2\nB.dll+0x3")),
		FFlockRepeatedExceptionCounter::MakeSameFaultKey(Game, TEXT("boom"), TEXT("A.dll+0x1\nA.dll+0x2\nC.dll+0x4")));

	// Every captured log line starts in the same logging frames; counted, they would make every call site the same fault.
	TestNotEqual(TEXT("the logging frames are skipped"),
		FFlockRepeatedExceptionCounter::MakeSameFaultKey(Game, TEXT("boom"),
			TEXT("UnrealEditor-Core.dll+0x10\nUnrealEditor-Core.dll+0x20\nA.dll+0x1\nA.dll+0x2")),
		FFlockRepeatedExceptionCounter::MakeSameFaultKey(Game, TEXT("boom"),
			TEXT("UnrealEditor-Core.dll+0x10\nUnrealEditor-Core.dll+0x20\nB.dll+0x1\nB.dll+0x2")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRepeatedExceptionCounterWindowTest, "Flock.Analytics.RepeatedExceptionCounter.Window",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockRepeatedExceptionCounterWindowTest::RunTest(const FString& Parameters)
{
	const FFlockLogDetails Game = DetailsForCategory(TEXT("LogGame"));
	const FString Key = TEXT("key-a");

	{
		FFlockRepeatedExceptionCounter Counter(60.f);
		TestTrue(TEXT("first occurrence reported"), Counter.ShouldReportNow(Key, TEXT("boom"), TEXT("stack"), Game, 100.0));
		TestFalse(TEXT("a repeat inside the window is held back"), Counter.ShouldReportNow(Key, TEXT("boom"), TEXT("stack"), Game, 110.0));
		TestFalse(TEXT("up to the window's last moment"), Counter.ShouldReportNow(Key, TEXT("boom"), TEXT("stack"), Game, 159.9));
		TestEqual(TEXT("nothing to report while the window is open"), Counter.CollectFinished(159.9).Num(), 0);

		const TArray<FFlockRepeatedExceptionCounter::FRepeatReport> Finished = Counter.CollectFinished(160.0);
		TestEqual(TEXT("one repeat report when the window closes"), Finished.Num(), 1);
		TestEqual(TEXT("counting the repeats, not the first report"), FlockTestAt(Finished, 0).Repeats, 2);
		TestEqual(TEXT("first seen"), FlockTestAt(Finished, 0).FirstSeenSeconds, 100.0);
		TestEqual(TEXT("last seen"), FlockTestAt(Finished, 0).LastSeenSeconds, 159.9);
		TestEqual(TEXT("message kept"), FlockTestAt(Finished, 0).Message, TEXT("boom"));
		TestEqual(TEXT("what the first report carried is kept"),
			FlockTestAt(Finished, 0).Details.ExtraData.FindRef(TEXT("category")), FString(TEXT("LogGame")));
		TestEqual(TEXT("window freed"), Counter.OpenWindowCount(), 0);

		// A window nobody repeated in closes quietly: the first report already said everything.
		TestTrue(TEXT("a lone fault is reported"), Counter.ShouldReportNow(TEXT("key-b"), TEXT("once"), FString(), Game, 200.0));
		TestEqual(TEXT("and owes no repeat report"), Counter.CollectFinished(300.0).Num(), 0);

		// Once its window has closed, the same fault is news again.
		TestTrue(TEXT("reported again in a new window"), Counter.ShouldReportNow(Key, TEXT("boom"), TEXT("stack"), Game, 400.0));
	}

	{
		// A window that ran out with no tick in between still owes its repeats when the fault comes back.
		FFlockRepeatedExceptionCounter Late(60.f);
		Late.ShouldReportNow(Key, TEXT("boom"), FString(), Game, 0.0);
		Late.ShouldReportNow(Key, TEXT("boom"), FString(), Game, 1.0);
		TestTrue(TEXT("a new window opens"), Late.ShouldReportNow(Key, TEXT("boom"), FString(), Game, 90.0));
		const TArray<FFlockRepeatedExceptionCounter::FRepeatReport> Owed = Late.CollectFinished(90.0);
		TestEqual(TEXT("the old window's repeats are not lost"), Owed.Num(), 1);
		TestEqual(TEXT("with their count"), FlockTestAt(Owed, 0).Repeats, 1);

		// Shutdown collects windows that are still open.
		TestFalse(TEXT("a repeat in the new window"), Late.ShouldReportNow(Key, TEXT("boom"), FString(), Game, 91.0));
		const TArray<FFlockRepeatedExceptionCounter::FRepeatReport> Remaining = Late.CollectAll();
		TestEqual(TEXT("the open window is collected"), Remaining.Num(), 1);
		TestEqual(TEXT("with its repeat"), FlockTestAt(Remaining, 0).Repeats, 1);
		TestEqual(TEXT("nothing left open afterwards"), Late.OpenWindowCount(), 0);
	}

	{
		// A window of 0 is the project switching repeat counting off.
		FFlockRepeatedExceptionCounter Off(0.f);
		TestTrue(TEXT("reported"), Off.ShouldReportNow(Key, TEXT("boom"), FString(), Game, 0.0));
		TestTrue(TEXT("and reported again"), Off.ShouldReportNow(Key, TEXT("boom"), FString(), Game, 0.0));
		TestEqual(TEXT("no window opened"), Off.OpenWindowCount(), 0);
		TestEqual(TEXT("nothing to collect"), Off.CollectAll().Num(), 0);
	}

	{
		// With no room left to count a new fault's repeats, they are reported rather than hide a fault nobody has seen.
		FFlockRepeatedExceptionCounter Small(60.f, /*MaxTracked*/ 1);
		TestTrue(TEXT("counted"), Small.ShouldReportNow(TEXT("key-1"), TEXT("one"), FString(), Game, 0.0));
		TestTrue(TEXT("a fault with no room is still reported"), Small.ShouldReportNow(TEXT("key-2"), TEXT("two"), FString(), Game, 1.0));
		TestTrue(TEXT("and so is its repeat, since nothing counts it"), Small.ShouldReportNow(TEXT("key-2"), TEXT("two"), FString(), Game, 2.0));
		TestFalse(TEXT("the counted fault is still held back"), Small.ShouldReportNow(TEXT("key-1"), TEXT("one"), FString(), Game, 3.0));

		// Room is made by closing windows that have run out.
		TestTrue(TEXT("counted once the old window runs out"), Small.ShouldReportNow(TEXT("key-3"), TEXT("three"), FString(), Game, 61.0));
		TestFalse(TEXT("so its repeat is counted"), Small.ShouldReportNow(TEXT("key-3"), TEXT("three"), FString(), Game, 62.0));
		TestEqual(TEXT("the closed window's report is kept"), Small.CollectFinished(62.0).Num(), 1);
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
