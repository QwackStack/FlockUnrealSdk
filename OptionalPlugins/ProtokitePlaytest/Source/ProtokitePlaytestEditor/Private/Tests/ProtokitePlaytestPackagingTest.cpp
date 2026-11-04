// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Misc/RedirectCollector.h"
#include "ProtokitePlaytestFormLogos.h"
#include "ProtokitePlaytestSettings.h"

/**
 * The playtest editor module records Powered by Qwacks for packaging as it starts, among the references the cook packages
 * as it begins: the ones recorded while no package is loading, as a project setting's are. Only this plugin's code names
 * the mark, so without the record a packaged game draws no mark.
 *
 * It reads what the editor really recorded at start-up. Reading takes the records away, so they are put back: the cook
 * reads them in its own process, never this one, but this editor is left as it was.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestPackagesTheFormsImagesTest,
	"Protokite.Playtest.Editor.PackagingCarriesTheFeedbackFormsImages",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestPackagesTheFormsImagesTest::RunTest(const FString& Parameters)
{
	TSet<FName> Recorded;
	GRedirectCollector.ProcessSoftObjectPathPackageList(NAME_None, false, Recorded);
	for (const FName& Package : Recorded)
	{
		FSoftObjectPath(FTopLevelAssetPath(Package, FPackageName::GetShortFName(Package)), FString()).PostLoadPath(nullptr);
	}

	TestTrue(TEXT("Powered by Qwacks is recorded for packaging"), Recorded.Contains(FName(TEXT("/ProtokitePlaytest/ProtokitePlaytestPoweredByQwacks"))));
	for (const FSoftObjectPath& Image : FProtokitePlaytestFormLogos::ImagesToPackage(*GetDefault<UProtokitePlaytestSettings>()))
	{
		TestTrue(FString::Printf(TEXT("%s is recorded"), *Image.ToString()), Recorded.Contains(Image.GetLongPackageFName()));
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
