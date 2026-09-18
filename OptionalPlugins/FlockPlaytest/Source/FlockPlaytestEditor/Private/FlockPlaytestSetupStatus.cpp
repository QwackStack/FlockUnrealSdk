// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockPlaytestSetupStatus.h"

#include "Config/FlockConfig.h"
#include "FlockPlaytestSettings.h"
#include "FlockPlaytestStatus.h"

#define LOCTEXT_NAMESPACE "FlockPlaytestSetupStatus"

FFlockPlaytestSetupInput FFlockPlaytestSetupInput::FromProjectSettings()
{
	const UFlockPlaytestSettings* Playtest = GetDefault<UFlockPlaytestSettings>();
	const UFlockConfig* Flock = GetDefault<UFlockConfig>();

	FFlockPlaytestSetupInput Input;
	Input.bPlaytestingEnabled = Playtest->bPlaytestingEnabled;
	Input.ProtokiteApiUrl = Playtest->ProtokiteApiUrl;
	Input.FlockGameVersion = Flock->GameVersion;
	Input.bFlockAnalyticsEnabled = Flock->bAnalyticsEnabled;
	Input.bFlockAnalyticsAutoStartSession = Flock->bAnalyticsAutoStartSession;
	Input.bFlockAnalyticsRequireExplicitConsent = Flock->bAnalyticsRequireExplicitConsent;
	return Input;
}

TArray<FFlockPlaytestSetupFinding> FFlockPlaytestSetupStatus::Evaluate(const FFlockPlaytestSetupInput& Input)
{
	TArray<FFlockPlaytestSetupFinding> Findings;
	if (!Input.bPlaytestingEnabled)
	{
		return Findings;
	}

	// The URL, with the rule and the words the running game uses, so the editor and the log cannot disagree.
	if (Input.ProtokiteApiUrl.IsEmpty() || !IsUsableProtokiteApiUrl(Input.ProtokiteApiUrl))
	{
		const bool bMissing = Input.ProtokiteApiUrl.IsEmpty();
		FFlockPlaytestSetupFinding Finding;
		Finding.Id = bMissing ? FName(TEXT("Playtest.ProtokiteApiUrlMissing")) : FName(TEXT("Playtest.ProtokiteApiUrlUnusable"));
		Finding.Severity = EFlockPlaytestSetupSeverity::StopsPlaytesting;
		Finding.Title = bMissing ? LOCTEXT("UrlMissing", "Protokite API URL is empty") : LOCTEXT("UrlUnusable", "Protokite API URL cannot be used");
		Finding.Detail = bMissing
			? FText::FromString(DescribePlaytestStatus(EFlockPlaytestStatus::ProtokiteApiUrlMissing))
			: FText::FromString(FString::Printf(TEXT("%s Current value: '%s'."),
				*DescribePlaytestStatus(EFlockPlaytestStatus::ProtokiteApiUrlUnusable), *Input.ProtokiteApiUrl));
		Finding.Fix = EFlockPlaytestSetupFix::OpenPlaytestSettings;
		Findings.Add(Finding);
	}

	if (!Input.bFlockAnalyticsEnabled)
	{
		FFlockPlaytestSetupFinding Finding;
		Finding.Id = TEXT("Playtest.FlockAnalyticsOff");
		Finding.Severity = EFlockPlaytestSetupSeverity::StopsPlaytesting;
		Finding.Title = LOCTEXT("AnalyticsOff", "The Flock SDK's analytics is off");
		Finding.Detail = LOCTEXT("AnalyticsOffDetail", "A playtest session starts from a Flock session, and the Flock SDK starts none while Analytics Enabled is off, so no playtest session, performance event or exception reaches Protokite. Turn on Analytics Enabled in Project Settings > Plugins > Flock SDK Settings.");
		Finding.Fix = EFlockPlaytestSetupFix::OpenFlockSettings;
		Findings.Add(Finding);
	}

	// An empty Game Version is the Flock SDK's own finding, and it stops the SDK starting at all, so it is not repeated.
	if (!Input.FlockGameVersion.IsEmpty() && !Input.FlockGameVersion.StartsWith(PlaytestVersionPrefix, ESearchCase::CaseSensitive))
	{
		FFlockPlaytestSetupFinding Finding;
		Finding.Id = TEXT("Playtest.NotAPlaytestVersion");
		Finding.Severity = EFlockPlaytestSetupSeverity::Warning;
		Finding.Title = LOCTEXT("NotPlaytestVersion", "Game Version is not a playtest's version");
		Finding.Detail = FText::Format(LOCTEXT("NotPlaytestVersionDetail", "The Flock SDK's Game Version is '{0}'. Protokite links a playtest to a version it names {1} followed by the test's id, and a build carrying any other version finds no playtest. Protokite's test page shows that version's ID: set Game Version to its name, {1}<test id>, and the Flock SDK resolves the ID itself. An ID written straight into the settings file is replaced whenever the Flock SDK resolves Game Version again."),
			FText::FromString(Input.FlockGameVersion), FText::FromString(PlaytestVersionPrefix));
		Finding.Fix = EFlockPlaytestSetupFix::OpenFlockSettings;
		Findings.Add(Finding);
	}

	// Only worth saying while a Flock session can start at all; with analytics off the finding above already covers it.
	if (Input.bFlockAnalyticsEnabled && !Input.bFlockAnalyticsAutoStartSession)
	{
		FFlockPlaytestSetupFinding Finding;
		Finding.Id = TEXT("Playtest.WaitsForStartSession");
		Finding.Severity = EFlockPlaytestSetupSeverity::Info;
		Finding.Title = LOCTEXT("WaitsForStartSession", "Playtest sessions wait for a Start Session call");
		Finding.Detail = LOCTEXT("WaitsForStartSessionDetail", "Analytics Auto Start Session is off, so a Flock session, and the playtest session with it, starts only when the game calls Start Session after a player signs in.");
		Finding.Fix = EFlockPlaytestSetupFix::OpenFlockSettings;
		Findings.Add(Finding);
	}
	if (Input.bFlockAnalyticsEnabled && Input.bFlockAnalyticsRequireExplicitConsent)
	{
		FFlockPlaytestSetupFinding Finding;
		Finding.Id = TEXT("Playtest.WaitsForConsent");
		Finding.Severity = EFlockPlaytestSetupSeverity::Info;
		Finding.Title = LOCTEXT("WaitsForConsent", "Playtest sessions wait for analytics consent");
		Finding.Detail = LOCTEXT("WaitsForConsentDetail", "Analytics Require Explicit Consent is on, so no Flock session, and no playtest session, starts until the player grants consent.");
		Finding.Fix = EFlockPlaytestSetupFix::OpenFlockSettings;
		Findings.Add(Finding);
	}

	// Most serious first, and otherwise in the order above.
	Findings.StableSort([](const FFlockPlaytestSetupFinding& A, const FFlockPlaytestSetupFinding& B)
	{
		return static_cast<uint8>(A.Severity) > static_cast<uint8>(B.Severity);
	});
	return Findings;
}

#undef LOCTEXT_NAMESPACE
