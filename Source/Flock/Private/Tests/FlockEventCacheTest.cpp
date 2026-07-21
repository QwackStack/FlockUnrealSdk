// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Analytics/FlockAnalyticsJson.h"
#include "Analytics/FlockFileEventCache.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/Support/FlockMemoryEventCache.h"
#include "Tests/Support/FlockTestSafeIndex.h"

namespace
{
	/** A throwaway root per case so cases never see each other's spool. */
	FString MakeTempRoot()
	{
		return FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
			FString::Printf(TEXT("cache_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	void DeleteTempRoot(const FString& Root)
	{
		IFileManager::Get().DeleteDirectory(*Root, false, true);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockEventCacheRoundTripTest, "Flock.Analytics.Cache.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockEventCacheRoundTripTest::RunTest(const FString& Parameters)
{
	const FString Root = MakeTempRoot();
	{
		FFlockFileEventCache Cache(TEXT("log_events"), 100, Root);
		TestEqual(TEXT("starts empty"), Cache.PendingCount(), 0);

		const FString H1 = Cache.Enqueue(TEXT("{\"n\":1}"));
		const FString H2 = Cache.Enqueue(TEXT("{\"n\":2}"));
		const FString H3 = Cache.Enqueue(TEXT("{\"n\":3}"));
		TestFalse(TEXT("handle 1 issued"), H1.IsEmpty());
		TestFalse(TEXT("handle 2 issued"), H2.IsEmpty());
		TestTrue(TEXT("handles are distinct"), H1 != H2 && H2 != H3);
		TestEqual(TEXT("three pending"), Cache.PendingCount(), 3);

		FString Payload;
		TestTrue(TEXT("reads back"), Cache.Read(H2, Payload));
		TestEqual(TEXT("payload intact"), Payload, TEXT("{\"n\":2}"));
		TestFalse(TEXT("unknown handle reads false"), Cache.Read(TEXT("nope"), Payload));

		// Batches come out oldest first and stay spooled until removed.
		TArray<FString> Handles;
		TArray<FString> Payloads;
		Cache.PeekBatch(2, Handles, Payloads);
		TestEqual(TEXT("batch size honoured"), Handles.Num(), 2);
		TestEqual(TEXT("oldest first"), FlockTestAt(Payloads, 0), TEXT("{\"n\":1}"));
		TestEqual(TEXT("then next"), FlockTestAt(Payloads, 1), TEXT("{\"n\":2}"));
		TestEqual(TEXT("peek does not consume"), Cache.PendingCount(), 3);

		// Removing acknowledges a send.
		Cache.Remove(H1);
		TestEqual(TEXT("one gone"), Cache.PendingCount(), 2);
		Cache.Remove(TEXT("nope"));
		TestEqual(TEXT("unknown remove is a no-op"), Cache.PendingCount(), 2);

		// Replace keeps queue position — this is what retag-after-auth relies on.
		Cache.Replace(H2, TEXT("{\"n\":22}"));
		Cache.PeekBatch(1, Handles, Payloads);
		TestEqual(TEXT("replaced in place"), FlockTestAt(Payloads, 0), TEXT("{\"n\":22}"));
		TestEqual(TEXT("still head of queue"), FlockTestAt(Handles, 0), H2);

		Cache.Clear();
		TestEqual(TEXT("cleared"), Cache.PendingCount(), 0);
	}
	DeleteTempRoot(Root);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockEventCacheCapTest, "Flock.Analytics.Cache.Cap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockEventCacheCapTest::RunTest(const FString& Parameters)
{
	// Oldest entries are dropped once the cap is hit.
	{
		const FString Root = MakeTempRoot();
		FFlockFileEventCache Cache(TEXT("log_events"), 3, Root);
		for (int32 Index = 1; Index <= 5; ++Index)
		{
			Cache.Enqueue(FString::Printf(TEXT("{\"n\":%d}"), Index));
		}
		TestEqual(TEXT("capped"), Cache.PendingCount(), 3);

		TArray<FString> Handles;
		TArray<FString> Payloads;
		Cache.PeekBatch(10, Handles, Payloads);
		TestEqual(TEXT("kept the newest three"), Payloads.Num(), 3);
		TestEqual(TEXT("oldest surviving"), FlockTestAt(Payloads, 0), TEXT("{\"n\":3}"));
		TestEqual(TEXT("newest"), FlockTestAt(Payloads, 2), TEXT("{\"n\":5}"));
		DeleteTempRoot(Root);
	}

	// A cap of zero retains nothing and says so by returning an empty handle.
	{
		const FString Root = MakeTempRoot();
		FFlockFileEventCache Cache(TEXT("log_events"), 0, Root);
		const FString Handle = Cache.Enqueue(TEXT("{\"n\":1}"));
		TestTrue(TEXT("no handle issued"), Handle.IsEmpty());
		TestEqual(TEXT("nothing retained"), Cache.PendingCount(), 0);
		DeleteTempRoot(Root);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockEventCachePersistenceTest, "Flock.Analytics.Cache.Persistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockEventCachePersistenceTest::RunTest(const FString& Parameters)
{
	const FString Root = MakeTempRoot();

	// Spool survives the process: this is the whole point of the write-ahead cache.
	{
		FFlockFileEventCache Cache(TEXT("log_events"), 100, Root);
		Cache.Enqueue(TEXT("{\"n\":1}"));
		Cache.Enqueue(TEXT("{\"n\":2}"));
		Cache.Enqueue(TEXT("{\"n\":3}"));
	}
	{
		FFlockFileEventCache Reloaded(TEXT("log_events"), 100, Root);
		TestEqual(TEXT("reloaded all"), Reloaded.PendingCount(), 3);

		TArray<FString> Handles;
		TArray<FString> Payloads;
		Reloaded.PeekBatch(10, Handles, Payloads);
		TestEqual(TEXT("age order survived the restart"), FlockTestAt(Payloads, 0), TEXT("{\"n\":1}"));
		TestEqual(TEXT("age order survived the restart (last)"), FlockTestAt(Payloads, 2), TEXT("{\"n\":3}"));
	}

	// A cap lowered between runs applies to what is already on disk.
	{
		FFlockFileEventCache Lowered(TEXT("log_events"), 1, Root);
		TestEqual(TEXT("evicted on load"), Lowered.PendingCount(), 1);

		TArray<FString> Handles;
		TArray<FString> Payloads;
		Lowered.PeekBatch(10, Handles, Payloads);
		TestEqual(TEXT("kept the newest"), FlockTestAt(Payloads, 0), TEXT("{\"n\":3}"));
	}

	// Separate subfolders do not see each other.
	{
		FFlockFileEventCache Sessions(TEXT("session_ends"), 100, Root);
		TestEqual(TEXT("independent subfolder"), Sessions.PendingCount(), 0);
	}

	DeleteTempRoot(Root);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockEventCacheResilienceTest, "Flock.Analytics.Cache.Resilience",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockEventCacheResilienceTest::RunTest(const FString& Parameters)
{
	const FString Root = MakeTempRoot();
	FString Directory;
	FString SurvivingHandle;

	{
		FFlockFileEventCache Cache(TEXT("log_events"), 100, Root);
		Directory = Cache.GetDirectory();
		Cache.Enqueue(TEXT("{\"n\":1}"));
		SurvivingHandle = Cache.Enqueue(TEXT("{\"n\":2}"));
	}

	// A foreign file in the directory is ignored rather than treated as an entry.
	FFileHelper::SaveStringToFile(FString(TEXT("not an entry")), *FPaths::Combine(Directory, TEXT("README.txt")));

	{
		FFlockFileEventCache Cache(TEXT("log_events"), 100, Root);
		TestEqual(TEXT("foreign file ignored"), Cache.PendingCount(), 2);

		// An entry deleted underneath us is skipped, not fatal, and the batch still delivers the rest.
		TArray<FString> AllHandles = Cache.AllHandles();
		IFileManager::Get().Delete(*FPaths::Combine(Directory, FlockTestAt(AllHandles, 0) + TEXT(".json")));

		TArray<FString> Handles;
		TArray<FString> Payloads;
		Cache.PeekBatch(10, Handles, Payloads);
		TestEqual(TEXT("skips the vanished entry"), Payloads.Num(), 1);
		TestEqual(TEXT("delivers the survivor"), FlockTestAt(Payloads, 0), TEXT("{\"n\":2}"));

		FString Ignored;
		TestFalse(TEXT("read of a vanished entry fails cleanly"), Cache.Read(FlockTestAt(AllHandles, 0), Ignored));
	}

	DeleteTempRoot(Root);
	return true;
}

/** The spool stores exactly what the wire builder produced — including game-authored keys. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockEventCacheLogEventTest, "Flock.Analytics.Cache.LogEventPayload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockEventCacheLogEventTest::RunTest(const FString& Parameters)
{
	const FString Root = MakeTempRoot();
	FString Handle;

	{
		FFlockLogEventRequest Event;
		Event.Message = TEXT("spooled");
		Event.Data.Type = EFlockLogEventType::Exception;
		Event.Data.ErrorMessage = TEXT("boom");
		Event.Data.ExtraData.Add(TEXT("playerLevel"), TEXT("7"));

		FFlockFileEventCache Cache(TEXT("log_events"), 100, Root);
		Handle = Cache.Enqueue(FFlockAnalyticsJson::SerializeEvent(Event));
		TestFalse(TEXT("spooled"), Handle.IsEmpty());
	}

	// Reload in a fresh instance, as a crash-recovery flush would.
	{
		FFlockFileEventCache Reloaded(TEXT("log_events"), 100, Root);
		TArray<FString> Handles;
		TArray<FString> Payloads;
		Reloaded.PeekBatch(1, Handles, Payloads);
		TestEqual(TEXT("one entry"), Payloads.Num(), 1);

		FFlockLogEventRequest Back;
		TestTrue(TEXT("deserializes"), FFlockAnalyticsJson::DeserializeEvent(FlockTestAt(Payloads, 0), Back));
		TestEqual(TEXT("message"), Back.Message, TEXT("spooled"));
		TestTrue(TEXT("type survived"), Back.Data.Type == EFlockLogEventType::Exception);
		TestEqual(TEXT("error message"), Back.Data.ErrorMessage, TEXT("boom"));
		const FString* Level = Back.Data.ExtraData.Find(TEXT("playerLevel"));
		TestTrue(TEXT("game-authored key survived the spool"), Level != nullptr);
		if (Level != nullptr)
		{
			TestEqual(TEXT("value"), *Level, TEXT("7"));
		}
	}

	DeleteTempRoot(Root);
	return true;
}

/**
 * Differential test: every provider test runs against the in-memory fake, so the fake drifting from
 * the real cache would silently invalidate all of them. This drives one operation sequence through
 * BOTH implementations and asserts the observable results match at each step — testing the fake
 * alone (as this once did) proves nothing about the thing it stands in for.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockEventCacheMemoryParityTest, "Flock.Analytics.Cache.MemoryParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockEventCacheMemoryParityTest::RunTest(const FString& Parameters)
{
	const FString Root = MakeTempRoot();
	FFlockFileEventCache File(TEXT("log_events"), 3, Root);
	FFlockMemoryEventCache Memory(3);

	TArray<FString> FileHandles, FilePayloads, MemHandles, MemPayloads;
	const auto PeekBoth = [&]()
	{
		File.PeekBatch(10, FileHandles, FilePayloads);
		Memory.PeekBatch(10, MemHandles, MemPayloads);
	};
	const auto Joined = [](const TArray<FString>& In) { return FString::Join(In, TEXT(",")); };

	// Overfill past the cap: both must evict the same entries, in the same order.
	for (int32 Index = 1; Index <= 5; ++Index)
	{
		const FString Payload = FString::Printf(TEXT("{\"n\":%d}"), Index);
		File.Enqueue(Payload);
		Memory.Enqueue(Payload);
	}
	TestEqual(TEXT("same pending count after overfill"), Memory.PendingCount(), File.PendingCount());

	PeekBoth();
	TestEqual(TEXT("same batch contents and order"), Joined(MemPayloads), Joined(FilePayloads));
	TestEqual(TEXT("and it is the newest three"), Joined(FilePayloads),
		FString(TEXT("{\"n\":3},{\"n\":4},{\"n\":5}")));

	// Acknowledging the head.
	File.Remove(FlockTestAt(FileHandles, 0));
	Memory.Remove(FlockTestAt(MemHandles, 0));
	TestEqual(TEXT("same pending after remove"), Memory.PendingCount(), File.PendingCount());

	// An unknown handle is a no-op in both.
	File.Remove(TEXT("nope"));
	Memory.Remove(TEXT("nope"));
	TestEqual(TEXT("unknown remove ignored by both"), Memory.PendingCount(), File.PendingCount());

	// Replace keeps queue position in both.
	PeekBoth();
	File.Replace(FlockTestAt(FileHandles, 0), TEXT("{\"n\":99}"));
	Memory.Replace(FlockTestAt(MemHandles, 0), TEXT("{\"n\":99}"));
	PeekBoth();
	TestEqual(TEXT("same payloads after replace"), Joined(MemPayloads), Joined(FilePayloads));
	TestEqual(TEXT("replaced in place, still at the head"), FlockTestAt(FilePayloads, 0), TEXT("{\"n\":99}"));

	// A cap of zero retains nothing in either.
	{
		FFlockFileEventCache ZeroFile(TEXT("zero"), 0, Root);
		FFlockMemoryEventCache ZeroMemory(0);
		TestTrue(TEXT("file cache issues no handle at cap 0"), ZeroFile.Enqueue(TEXT("{}")).IsEmpty());
		TestTrue(TEXT("memory fake issues no handle at cap 0"), ZeroMemory.Enqueue(TEXT("{}")).IsEmpty());
		TestEqual(TEXT("both retain nothing"), ZeroMemory.PendingCount(), ZeroFile.PendingCount());
	}

	// Clear empties both.
	File.Clear();
	Memory.Clear();
	TestEqual(TEXT("same after clear"), Memory.PendingCount(), File.PendingCount());
	TestEqual(TEXT("and both are empty"), File.PendingCount(), 0);

	// The fake's write-failure switch has no file-cache counterpart (it stands in for a full disk),
	// so it is checked on its own.
	Memory.bFailWrites = true;
	TestTrue(TEXT("failed write yields no handle"), Memory.Enqueue(TEXT("{}")).IsEmpty());
	TestEqual(TEXT("and stores nothing"), Memory.PendingCount(), 0);

	DeleteTempRoot(Root);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
