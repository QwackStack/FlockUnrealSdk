// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Misc/FlockEngineCompat.h"

#include "Analytics/FlockAnalyticsLibrary.h"
#include "Analytics/FlockMetadata.h"
#include "Dom/JsonObject.h"
#include "Models/FlockCommandModels.h"

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
	TestEqualSensitive(TEXT("bool"), Built.FindRef(TEXT("flawless")), TEXT("true"));
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
		TestEqualSensitive(*FString::Printf(TEXT("'%s' matches C++"), *Pair.Key),
			FromBlueprint.FindRef(Pair.Key), Pair.Value);
	}
	return true;
}

/**
 * The event-property chain a graph builds for Flock Track Event. It is a different container from the
 * metadata chain above on purpose: a diagnostic entry's extra data is text, while an event property keeps
 * its type so the dashboards can chart it. The values are read back as the JSON that goes on the wire for
 * exactly that reason — a number that arrived quoted would still read as the right value as a string.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsLibraryEventPropertyChainTest, "Flock.Analytics.Library.EventPropertyChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsLibraryEventPropertyChainTest::RunTest(const FString& Parameters)
{
	FFlockCommandData Built = UFlockAnalyticsLibrary::MakeEventProperties();
	TestTrue(TEXT("seed is empty"), Built.IsEmpty());

	Built = UFlockAnalyticsLibrary::AddEventPropertyInt(Built, TEXT("level"), 3);
	Built = UFlockAnalyticsLibrary::AddEventPropertyBool(Built, TEXT("flawless"), true);
	Built = UFlockAnalyticsLibrary::AddEventPropertyString(Built, TEXT("zone"), TEXT("cavern"));
	Built = UFlockAnalyticsLibrary::AddEventPropertyFloat(Built, TEXT("elapsed"), 12.5f);
	Built = UFlockAnalyticsLibrary::AddEventPropertyStringArray(Built, TEXT("tags"), { TEXT("beta"), TEXT("tutorial") });

	TestEqual(TEXT("five properties"), Built.GetFieldNames().Num(), 5);

	const TSharedRef<FJsonObject> Fields = Built.ToJsonObject();
	// Each value is checked by the JSON type it kept, because that is the whole difference from metadata:
	// a level that reached the dashboards as "3" cannot be charted.
	TestTrue(TEXT("an integer stays a number"),
		Fields->HasTypedField<EJson::Number>(TEXT("level")) && Fields->GetNumberField(TEXT("level")) == 3.0);
	TestTrue(TEXT("a flag stays a boolean"),
		Fields->HasTypedField<EJson::Boolean>(TEXT("flawless")) && Fields->GetBoolField(TEXT("flawless")));
	TestTrue(TEXT("a string stays a string"), Fields->HasTypedField<EJson::String>(TEXT("zone")));
	TestEqualSensitive(TEXT("the string's value"), Fields->GetStringField(TEXT("zone")), TEXT("cavern"));
	TestTrue(TEXT("a float stays a number"),
		Fields->HasTypedField<EJson::Number>(TEXT("elapsed")) && Fields->GetNumberField(TEXT("elapsed")) == 12.5);
	TestTrue(TEXT("a string array stays an array"), Fields->HasTypedField<EJson::Array>(TEXT("tags")));

	// Each link copies rather than mutating, so a graph can branch a chain without surprises.
	const FFlockCommandData Base = UFlockAnalyticsLibrary::AddEventPropertyInt(
		UFlockAnalyticsLibrary::MakeEventProperties(), TEXT("shared"), 1);
	const FFlockCommandData BranchA = UFlockAnalyticsLibrary::AddEventPropertyInt(Base, TEXT("a"), 1);
	const FFlockCommandData BranchB = UFlockAnalyticsLibrary::AddEventPropertyInt(Base, TEXT("b"), 2);
	TestEqual(TEXT("base untouched by branching"), Base.GetFieldNames().Num(), 1);
	TestFalse(TEXT("branch A has no B"), BranchA.ToJsonObject()->HasField(TEXT("b")));
	TestFalse(TEXT("branch B has no A"), BranchB.ToJsonObject()->HasField(TEXT("a")));

	// Keys reach the dashboards exactly as they were written, so a chain must not case-fold them.
	const FFlockCommandData Spelled = UFlockAnalyticsLibrary::AddEventPropertyInt(
		UFlockAnalyticsLibrary::MakeEventProperties(), TEXT("MaxHealth"), 1);
	TestTrue(TEXT("the key keeps its letter case"),
		Spelled.ToJsonString().Contains(TEXT("\"MaxHealth\""), ESearchCase::CaseSensitive));
	return true;
}

/**
 * The event-property nodes are the analytics surface's name for the struct the game-commands nodes write,
 * so the two must produce the same thing — a property set in a graph and the same property set in C++ have
 * to reach the dashboards identically. The nodes delegate to FFlockCommandData::Set for that reason; this
 * is what stops a reimplementation creeping in.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsLibraryEventPropertyParityTest, "Flock.Analytics.Library.EventPropertyCppParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsLibraryEventPropertyParityTest::RunTest(const FString& Parameters)
{
	FFlockCommandData FromCpp;
	FromCpp.Set(TEXT("level"), 3)
		.Set(TEXT("flawless"), true)
		.Set(TEXT("zone"), TEXT("cavern"))
		.Set(TEXT("elapsed"), 12.5f);

	FFlockCommandData FromBlueprint = UFlockAnalyticsLibrary::MakeEventProperties();
	FromBlueprint = UFlockAnalyticsLibrary::AddEventPropertyInt(FromBlueprint, TEXT("level"), 3);
	FromBlueprint = UFlockAnalyticsLibrary::AddEventPropertyBool(FromBlueprint, TEXT("flawless"), true);
	FromBlueprint = UFlockAnalyticsLibrary::AddEventPropertyString(FromBlueprint, TEXT("zone"), TEXT("cavern"));
	FromBlueprint = UFlockAnalyticsLibrary::AddEventPropertyFloat(FromBlueprint, TEXT("elapsed"), 12.5f);

	const TArray<FString> Names = FromCpp.GetFieldNames();
	TestEqual(TEXT("same property count"), FromBlueprint.GetFieldNames().Num(), Names.Num());

	const TSharedRef<FJsonObject> Cpp = FromCpp.ToJsonObject();
	const TSharedRef<FJsonObject> Graph = FromBlueprint.ToJsonObject();
	for (const FString& Name : Names)
	{
		const TSharedPtr<FJsonValue> CppValue = Cpp->TryGetField(Name);
		const TSharedPtr<FJsonValue> GraphValue = Graph->TryGetField(Name);
		if (TestNotNull(*FString::Printf(TEXT("'%s' was set from the graph"), *Name), GraphValue.Get())
			&& TestNotNull(*FString::Printf(TEXT("'%s' was set from C++"), *Name), CppValue.Get()))
		{
			TestTrue(*FString::Printf(TEXT("'%s' matches C++"), *Name),
				FJsonValue::CompareEqual(*CppValue, *GraphValue));
		}
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
