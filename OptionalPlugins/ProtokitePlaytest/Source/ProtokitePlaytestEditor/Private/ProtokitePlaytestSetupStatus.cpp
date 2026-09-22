// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "ProtokitePlaytestSetupStatus.h"

#include "Config/FlockConfig.h"
#include "ProtokitePlaytestSettings.h"
#include "ProtokitePlaytestStatus.h"

#define LOCTEXT_NAMESPACE "ProtokitePlaytestSetupStatus"

FProtokitePlaytestSetupInput FProtokitePlaytestSetupInput::FromProjectSettings()
{
	const UProtokitePlaytestSettings* Playtest = GetDefault<UProtokitePlaytestSettings>();
	const UFlockConfig* Flock = GetDefault<UFlockConfig>();

	FProtokitePlaytestSetupInput Input;
	Input.bPlaytestingEnabled = Playtest->bPlaytestingEnabled;
	Input.ProtokiteApiUrl = Playtest->ProtokiteApiUrl;
	Input.bAskThePlayerForPlaytestConsent = Playtest->bAskThePlayerForPlaytestConsent;
	Input.FlockGameVersion = Flock->GameVersion;
	Input.bFlockAnalyticsEnabled = Flock->bAnalyticsEnabled;
	Input.bFlockAnalyticsAutoStartSession = Flock->bAnalyticsAutoStartSession;
	Input.bFlockAnalyticsRequireExplicitConsent = Flock->bAnalyticsRequireExplicitConsent;
	return Input;
}

TArray<FProtokitePlaytestSetupFinding> FProtokitePlaytestSetupStatus::Evaluate(const FProtokitePlaytestSetupInput& Input)
{
	TArray<FProtokitePlaytestSetupFinding> Findings;
	if (!Input.bPlaytestingEnabled)
	{
		return Findings;
	}

	// The URL, with the rule and the words the running game uses, so the editor and the log cannot disagree.
	if (Input.ProtokiteApiUrl.IsEmpty() || !IsUsableProtokiteApiUrl(Input.ProtokiteApiUrl))
	{
		const bool bMissing = Input.ProtokiteApiUrl.IsEmpty();
		FProtokitePlaytestSetupFinding Finding;
		Finding.Id = bMissing ? FName(TEXT("Playtest.ProtokiteApiUrlMissing")) : FName(TEXT("Playtest.ProtokiteApiUrlUnusable"));
		Finding.Severity = EProtokitePlaytestSetupSeverity::StopsPlaytesting;
		Finding.Title = bMissing ? LOCTEXT("UrlMissing", "Protokite API URL is empty") : LOCTEXT("UrlUnusable", "Protokite API URL cannot be used");
		Finding.Detail = bMissing
			? FText::FromString(DescribePlaytestStatus(EProtokitePlaytestStatus::ProtokiteApiUrlMissing))
			: FText::FromString(FString::Printf(TEXT("%s Current value: '%s'."),
				*DescribePlaytestStatus(EProtokitePlaytestStatus::ProtokiteApiUrlUnusable), *Input.ProtokiteApiUrl));
		Finding.Fix = EProtokitePlaytestSetupFix::OpenPlaytestSettings;
		Findings.Add(Finding);
	}

	if (!Input.bFlockAnalyticsEnabled)
	{
		FProtokitePlaytestSetupFinding Finding;
		Finding.Id = TEXT("Playtest.FlockAnalyticsOff");
		Finding.Severity = EProtokitePlaytestSetupSeverity::StopsPlaytesting;
		Finding.Title = LOCTEXT("AnalyticsOff", "The Flock SDK's analytics is off");
		Finding.Detail = LOCTEXT("AnalyticsOffDetail", "A playtest session starts from a Flock session, and the Flock SDK starts none while Analytics Enabled is off, so no playtest session, performance event or exception reaches Protokite. Turn on Analytics Enabled in Project Settings > Plugins > Flock SDK Settings.");
		Finding.Fix = EProtokitePlaytestSetupFix::OpenFlockSettings;
		Findings.Add(Finding);
	}

	// An empty Game Version is the Flock SDK's own finding, and it stops the SDK starting at all, so it is not repeated.
	if (!Input.FlockGameVersion.IsEmpty() && !Input.FlockGameVersion.StartsWith(PlaytestVersionPrefix, ESearchCase::CaseSensitive))
	{
		FProtokitePlaytestSetupFinding Finding;
		Finding.Id = TEXT("Playtest.NotAPlaytestVersion");
		Finding.Severity = EProtokitePlaytestSetupSeverity::Warning;
		Finding.Title = LOCTEXT("NotPlaytestVersion", "Game Version is not a playtest's version");
		Finding.Detail = FText::Format(LOCTEXT("NotPlaytestVersionDetail", "The Flock SDK's Game Version is '{0}'. Protokite links a playtest to a version it names {1} followed by the test's id, and a build carrying any other version finds no playtest. Protokite's test page shows that version's ID: set Game Version to its name, {1}<test id>, and the Flock SDK resolves the ID itself. An ID written straight into the settings file is replaced whenever the Flock SDK resolves Game Version again."),
			FText::FromString(Input.FlockGameVersion), FText::FromString(PlaytestVersionPrefix));
		Finding.Fix = EProtokitePlaytestSetupFix::OpenFlockSettings;
		Findings.Add(Finding);
	}

	// Only worth saying while a Flock session can start at all; with analytics off the finding above already covers it.
	if (Input.bFlockAnalyticsEnabled && !Input.bFlockAnalyticsAutoStartSession)
	{
		FProtokitePlaytestSetupFinding Finding;
		Finding.Id = TEXT("Playtest.WaitsForStartSession");
		Finding.Severity = EProtokitePlaytestSetupSeverity::Info;
		Finding.Title = LOCTEXT("WaitsForStartSession", "Playtest sessions wait for a Start Session call");
		Finding.Detail = LOCTEXT("WaitsForStartSessionDetail", "Analytics Auto Start Session is off, so a Flock session, and the playtest session with it, starts only when the game calls Start Session after a player signs in.");
		Finding.Fix = EProtokitePlaytestSetupFix::OpenFlockSettings;
		Findings.Add(Finding);
	}
	if (Input.bFlockAnalyticsEnabled && Input.bFlockAnalyticsRequireExplicitConsent)
	{
		// Named for whose consent it is. This build can be waiting on two different answers from the same player, and
		// telling a developer that "consent" is missing without saying which one sends them to the wrong settings page.
		FProtokitePlaytestSetupFinding Finding;
		Finding.Id = TEXT("Playtest.WaitsForFlockAnalyticsConsent");
		Finding.Severity = EProtokitePlaytestSetupSeverity::Info;
		Finding.Title = LOCTEXT("WaitsForConsent", "Playtest sessions wait for the Flock SDK's analytics consent");
		Finding.Detail = LOCTEXT("WaitsForConsentDetail", "Analytics Require Explicit Consent is on, so no Flock session, and no playtest session, starts until the game grants the Flock SDK's analytics consent. That is the game's own consent and a separate question from the playtest's, which the playtest plugin asks itself.");
		Finding.Fix = EProtokitePlaytestSetupFix::OpenFlockSettings;
		Findings.Add(Finding);
	}
	if (Input.bAskThePlayerForPlaytestConsent)
	{
		FProtokitePlaytestSetupFinding Finding;
		Finding.Id = TEXT("Playtest.AsksThePlayerWhatToCollect");
		Finding.Severity = EProtokitePlaytestSetupSeverity::Info;
		Finding.Title = LOCTEXT("AsksThePlayer", "The playtest asks the player what it may collect");
		Finding.Detail = LOCTEXT("AsksThePlayerDetail", "Ask The Player For Playtest Consent is on, so this launch collects nothing until the player answers the playtest's own question, drawn over the game once the playtest loads: the screen and play data, either one on its own, or nothing at all. Answer it in a Development build's console with ProtokitePlaytest.AnswerConsent video_and_play_data, or turn the setting off for a build whose players are asked another way.");
		Finding.Fix = EProtokitePlaytestSetupFix::OpenPlaytestSettings;
		Findings.Add(Finding);
	}

	// Most serious first, and otherwise in the order above.
	Findings.StableSort([](const FProtokitePlaytestSetupFinding& A, const FProtokitePlaytestSetupFinding& B)
	{
		return static_cast<uint8>(A.Severity) > static_cast<uint8>(B.Severity);
	});
	return Findings;
}

#undef LOCTEXT_NAMESPACE
