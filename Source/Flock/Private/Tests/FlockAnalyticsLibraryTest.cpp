// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Analytics/FlockAnalyticsLibrary.h"
#include "Analytics/FlockMetadata.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsLibraryChainTest, "Flock.Analytics.Library.MetadataChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsLibraryChainTest::RunTest(const FString& Parameters)
{
	// The shape a graph builds: seed, then one link per value.
	TMap<FString, FString> Built = UFlockAnalyticsLibrary::MakeMetadata();
	TestEqual(TEXT("seed is empty"), Built.Num(), 0);

	Built = UFlockAnalyticsLibrary::AddMetadataInt(Built, TEXT("level"), 3);
	Built = UFlockAnalyticsLibrary::AddMetadataBool(Built, TEXT("flawless"), true);
	Built = UFlockAnalyticsLibrary::AddMetadataString(Built, TEXT("zone"), TEXT("cavern"));
	Built = UFlockAnalyticsLibrary::AddMetadataFloat(Built, TEXT("elapsed"), 12.5f);

	TestEqual(TEXT("four entries"), Built.Num(), 4);
	TestEqual(TEXT("int"), Built.FindRef(TEXT("level")), TEXT("3"));
	TestEqual(TEXT("bool"), Built.FindRef(TEXT("flawless")), TEXT("true"));
	TestEqual(TEXT("string"), Built.FindRef(TEXT("zone")), TEXT("cavern"));
	TestTrue(TEXT("float"), Built.FindRef(TEXT("elapsed")).StartsWith(TEXT("12.5")));

	// Each link copies rather than mutating, so a graph can branch a chain without surprises.
	const TMap<FString, FString> Base = UFlockAnalyticsLibrary::AddMetadataInt(
		UFlockAnalyticsLibrary::MakeMetadata(), TEXT("shared"), 1);
	const TMap<FString, FString> BranchA = UFlockAnalyticsLibrary::AddMetadataInt(Base, TEXT("a"), 1);
	const TMap<FString, FString> BranchB = UFlockAnalyticsLibrary::AddMetadataInt(Base, TEXT("b"), 2);
	TestEqual(TEXT("base untouched by branching"), Base.Num(), 1);
	TestFalse(TEXT("branch A has no B"), BranchA.Contains(TEXT("b")));
	TestFalse(TEXT("branch B has no A"), BranchB.Contains(TEXT("a")));

	// Re-adding a key overwrites, matching TMap semantics rather than silently duplicating.
	const TMap<FString, FString> Overwritten =
		UFlockAnalyticsLibrary::AddMetadataInt(BranchA, TEXT("a"), 99);
	TestEqual(TEXT("overwrites"), Overwritten.FindRef(TEXT("a")), TEXT("99"));
	TestEqual(TEXT("without growing"), Overwritten.Num(), BranchA.Num());

	// Keys are game-authored and must survive exactly — the same guarantee the wire builder gives.
	const TMap<FString, FString> Cased =
		UFlockAnalyticsLibrary::AddMetadataInt(UFlockAnalyticsLibrary::MakeMetadata(), TEXT("playerLevel"), 7);
	bool bExactKey = false;
	for (const TPair<FString, FString>& Pair : Cased)
	{
		bExactKey = bExactKey || Pair.Key.Equals(TEXT("playerLevel"), ESearchCase::CaseSensitive);
	}
	TestTrue(TEXT("caller key case preserved"), bExactKey);
	return true;
}

/**
 * Blueprint and C++ must produce byte-identical values, or the same logical event reports differently
 * depending on which language wrote it. The library delegates to FFlockMetadata for exactly this
 * reason; this test is what stops the two drifting apart.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsLibraryParityTest, "Flock.Analytics.Library.CppParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsLibraryParityTest::RunTest(const FString& Parameters)
{
	// By value, not by reference: FFlockMetadata() is a temporary, and binding a reference to a
	// member reached through a returned reference gets no lifetime extension. The first draft of this
	// test did exactly that and read freed memory.
	FFlockMetadata CppBuilder;
	CppBuilder.Add(TEXT("level"), 3)
		.Add(TEXT("flawless"), true)
		.Add(TEXT("zone"), TEXT("cavern"))
		.Add(TEXT("elapsed"), 12.5f);
	const TMap<FString, FString> FromCpp = CppBuilder.Values;

	TMap<FString, FString> FromBlueprint = UFlockAnalyticsLibrary::MakeMetadata();
	FromBlueprint = UFlockAnalyticsLibrary::AddMetadataInt(FromBlueprint, TEXT("level"), 3);
	FromBlueprint = UFlockAnalyticsLibrary::AddMetadataBool(FromBlueprint, TEXT("flawless"), true);
	FromBlueprint = UFlockAnalyticsLibrary::AddMetadataString(FromBlueprint, TEXT("zone"), TEXT("cavern"));
	FromBlueprint = UFlockAnalyticsLibrary::AddMetadataFloat(FromBlueprint, TEXT("elapsed"), 12.5f);

	TestEqual(TEXT("same entry count"), FromBlueprint.Num(), FromCpp.Num());
	for (const TPair<FString, FString>& Pair : FromCpp)
	{
		TestEqual(*FString::Printf(TEXT("'%s' matches C++"), *Pair.Key),
			FromBlueprint.FindRef(Pair.Key), Pair.Value);
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
