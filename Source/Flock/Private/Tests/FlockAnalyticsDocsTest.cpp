// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace FlockAnalyticsDocsTestHelpers
{
	/** Reads a file relative to this test's own folder, Source/Flock/Private/Tests, so the scans read the sources of this build. */
	inline bool LoadNextToThisTest(const FString& RelativePath, FString& OutText)
	{
		const FString TestsFolder = FPaths::GetPath(FString(ANSI_TO_TCHAR(__FILE__)));
		return FFileHelper::LoadFileToString(OutText, *FPaths::ConvertRelativePathToFull(FPaths::Combine(TestsFolder, RelativePath)));
	}

	struct FDeclaredFunction
	{
		FString Name;
		FString DocComment;
		/** The UFUNCTION macro above it, if any: what a graph reads is in there, not in the doc comment. */
		FString FunctionMacro;
	};

	/** Every function a header declares, with the doc comment directly above it. A UFUNCTION macro may sit between the two. */
	inline TArray<FDeclaredFunction> DeclaredFunctions(const FString& HeaderText)
	{
		TArray<FString> Lines;
		HeaderText.ParseIntoArrayLines(Lines, /*InCullEmpty*/ false);

		TArray<FDeclaredFunction> Out;
		FString Comment;
		FString Macro;
		bool bInsideComment = false;
		int32 OpenMacroParentheses = 0;
		for (const FString& RawLine : Lines)
		{
			const FString Line = RawLine.TrimStartAndEnd();
			if (bInsideComment)
			{
				Comment += Line + TEXT("\n");
				bInsideComment = !Line.Contains(TEXT("*/"));
				continue;
			}
			if (OpenMacroParentheses > 0 || Line.StartsWith(TEXT("UFUNCTION(")))
			{
				Macro += Line;
				for (const TCHAR Character : Line)
				{
					OpenMacroParentheses += Character == TEXT('(') ? 1 : (Character == TEXT(')') ? -1 : 0);
				}
				continue;
			}
			if (Line.StartsWith(TEXT("/**")))
			{
				Comment = Line + TEXT("\n");
				bInsideComment = !Line.Contains(TEXT("*/"));
				continue;
			}
			if (Line.IsEmpty() || Line.StartsWith(TEXT("//")) || Line.StartsWith(TEXT("#")))
			{
				continue;
			}

			int32 OpenParenthesis = INDEX_NONE;
			if (Line.FindChar(TEXT('('), OpenParenthesis))
			{
				const FString BeforeParenthesis = Line.Left(OpenParenthesis).TrimEnd();
				const int32 NameStart = BeforeParenthesis.FindLastCharByPredicate(
					[](TCHAR Character) { return Character == TEXT(' ') || Character == TEXT('*') || Character == TEXT('&'); });
				Out.Add({ BeforeParenthesis.Mid(NameStart + 1), Comment, Macro });
			}
			// A member, a declaration or a function: the comment and macro above belonged to it either way.
			Comment.Reset();
			Macro.Reset();
		}
		return Out;
	}

	inline bool StartsWithWord(const FString& Name, const TCHAR* Word)
	{
		const int32 WordLength = FCString::Strlen(Word);
		return Name.StartsWith(Word, ESearchCase::CaseSensitive) && Name.Len() > WordLength && FChar::IsUpper(Name[WordLength]);
	}

	/** The surface a call's name puts it on: log_event for Log…, analytics for Track… and Record…, none otherwise. */
	inline FString SurfaceForName(const FString& Name)
	{
		if (Name.EndsWith(TEXT("ForTesting")))
		{
			return FString();
		}
		if (StartsWithWord(Name, TEXT("Log")))
		{
			return TEXT("log_event");
		}
		if (StartsWithWord(Name, TEXT("Track")) || StartsWithWord(Name, TEXT("Record")))
		{
			return TEXT("analytics");
		}
		return FString();
	}

	/** The row of a comparison table whose first cell is Label, or empty. */
	inline FString TableRow(const FString& Page, const FString& Label)
	{
		TArray<FString> Lines;
		Page.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			if (Line.StartsWith(FString::Printf(TEXT("| %s |"), *Label)))
			{
				return Line;
			}
		}
		return FString();
	}

	/** The first comparison table's header row: the one that starts with an empty first cell. */
	inline FString TableHeader(const FString& Page)
	{
		TArray<FString> Lines;
		Page.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			if (Line.StartsWith(TEXT("| | ")))
			{
				return Line;
			}
		}
		return FString();
	}
}

/**
 * Every call that records something says which dashboard reads it, in its doc comment and in the Blueprint category a
 * graph picks it from. The surface is derived from the call's name, so a new Log…, Track… or Record… call cannot ship
 * unlabelled or in the wrong drawer; a call that records nothing must carry no label, which is the control that stops a
 * scan labelling everything from passing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsSurfaceLabelTest, "Flock.Analytics.Docs.SurfaceLabels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsSurfaceLabelTest::RunTest(const FString& Parameters)
{
	using namespace FlockAnalyticsDocsTestHelpers;

	struct FHeaderToScan
	{
		const TCHAR* RelativePath;
		/** The recording calls the header is known to have; a scan that finds fewer is not reading it. */
		int32 AtLeastRecordingCalls;
	};
	const FHeaderToScan Headers[] = {
		{ TEXT("../../Public/FlockSubsystem.h"), 5 },
		{ TEXT("../../Public/Blueprint/FlockLibrary.h"), 5 },
		{ TEXT("../../Public/Providers/FlockAnalyticsProvider.h"), 6 },
	};

	for (const FHeaderToScan& Header : Headers)
	{
		FString Text;
		if (!TestTrue(FString::Printf(TEXT("%s is readable"), Header.RelativePath), LoadNextToThisTest(Header.RelativePath, Text)))
		{
			continue;
		}

		int32 RecordingCalls = 0;
		int32 OtherCalls = 0;
		for (const FDeclaredFunction& Function : DeclaredFunctions(Text))
		{
			const bool bLabelledAnalytics = Function.DocComment.Contains(TEXT("Surface: analytics"));
			const bool bLabelledLogEvent = Function.DocComment.Contains(TEXT("Surface: log_event"));
			const FString Surface = SurfaceForName(Function.Name);
			const bool bInDiagnosticsDrawer = Function.FunctionMacro.Contains(TEXT("Category = \"Flock|Diagnostics\""));
			const bool bInAnalyticsDrawer = Function.FunctionMacro.Contains(TEXT("Category = \"Flock|Analytics\""));
			if (Surface == TEXT("log_event"))
			{
				++RecordingCalls;
				TestTrue(FString::Printf(TEXT("%s is labelled log_event, and only that"), *Function.Name),
					bLabelledLogEvent && !bLabelledAnalytics);
				// The label is a doc comment, which nobody reading a graph's context menu ever sees. The
				// category is what they do see, so a diagnostics call sitting in the analytics drawer is the
				// same mislabelling in the place it actually misleads.
				TestFalse(FString::Printf(TEXT("%s is not offered in the analytics drawer"), *Function.Name),
					bInAnalyticsDrawer);
			}
			else if (Surface == TEXT("analytics"))
			{
				++RecordingCalls;
				TestTrue(FString::Printf(TEXT("%s is labelled analytics, and only that"), *Function.Name),
					bLabelledAnalytics && !bLabelledLogEvent);
				TestFalse(FString::Printf(TEXT("%s is not offered in the diagnostics drawer"), *Function.Name),
					bInDiagnosticsDrawer);
			}
			else
			{
				++OtherCalls;
				TestFalse(FString::Printf(TEXT("%s records nothing and carries no surface label"), *Function.Name),
					bLabelledAnalytics || bLabelledLogEvent);
			}
		}
		TestTrue(FString::Printf(TEXT("%s: found its recording calls"), Header.RelativePath),
			RecordingCalls >= Header.AtLeastRecordingCalls);
		TestTrue(FString::Printf(TEXT("%s: found calls that record nothing"), Header.RelativePath), OtherCalls > 0);
	}
	return true;
}

/**
 * The analytics and diagnostics pages open with the same comparison table, so a reader landing on either sees the whole
 * split. Two copies are only safe while something checks they agree — and analytics is the first column on both, which
 * is what makes the rows comparable at all. Flip one page's columns and every row silently means the opposite.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsDocsComparisonTableTest, "Flock.Analytics.Docs.ComparisonTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsDocsComparisonTableTest::RunTest(const FString& Parameters)
{
	using namespace FlockAnalyticsDocsTestHelpers;

	FString Analytics;
	FString Diagnostics;
	const bool bAnalyticsRead = TestTrue(TEXT("analytics.md is readable"),
		LoadNextToThisTest(TEXT("../../../../Documentation/analytics.md"), Analytics));
	const bool bDiagnosticsRead = TestTrue(TEXT("diagnostics.md is readable"),
		LoadNextToThisTest(TEXT("../../../../Documentation/diagnostics.md"), Diagnostics));
	if (!bAnalyticsRead || !bDiagnosticsRead)
	{
		return false;
	}

	for (const TCHAR* Label : { TEXT("Answers"), TEXT("Read by"), TEXT("Routes"), TEXT("Dashboard"), TEXT("Calls"),
		TEXT("Blueprint category"), TEXT("Custom data") })
	{
		const FString AnalyticsRow = TableRow(Analytics, Label);
		TestFalse(FString::Printf(TEXT("analytics.md has the %s row"), Label), AnalyticsRow.IsEmpty());
		TestEqual(FString::Printf(TEXT("the %s row is the same on both pages"), Label), TableRow(Diagnostics, Label), AnalyticsRow);
	}

	for (const TPair<FString, FString>& Page : { TPair<FString, FString>(TEXT("analytics.md"), Analytics),
		TPair<FString, FString>(TEXT("diagnostics.md"), Diagnostics) })
	{
		const FString Header = TableHeader(Page.Value);
		const int32 AnalyticsColumn = Header.Find(TEXT("Analytics"));
		const int32 LogEventsColumn = Header.Find(TEXT("Log events"));
		TestTrue(FString::Printf(TEXT("%s: analytics is the first column"), *Page.Key),
			AnalyticsColumn != INDEX_NONE && LogEventsColumn != INDEX_NONE && AnalyticsColumn < LogEventsColumn);
	}

	// The dashboards the pages name are the ones the code's labels name, so the docs cannot drift from the calls.
	const FString Dashboards = TableRow(Analytics, TEXT("Dashboard"));
	TestTrue(TEXT("analytics is read on Game Metrics"), Dashboards.Contains(TEXT("Game Metrics")));
	TestTrue(TEXT("log events are read on Diagnostics"), Dashboards.Contains(TEXT("Diagnostics")));
	FString Subsystem;
	if (TestTrue(TEXT("the subsystem header is readable"), LoadNextToThisTest(TEXT("../../Public/FlockSubsystem.h"), Subsystem)))
	{
		TestTrue(TEXT("the code labels name Game Metrics too"), Subsystem.Contains(TEXT("Surface: analytics")) && Subsystem.Contains(TEXT("Game Metrics")));
		TestTrue(TEXT("the code labels name Diagnostics too"), Subsystem.Contains(TEXT("Surface: log_event")) && Subsystem.Contains(TEXT("Diagnostics")));
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
