// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Setup/FlockSetupStatus.h"
#include "Algo/StableSort.h"

#define LOCTEXT_NAMESPACE "FlockEditor"

namespace
{
	FFlockSetupFinding MakeFinding(const TCHAR* Id, EFlockSetupSeverity Severity, FText Title, FText Detail,
		EFlockSetupFix Fix)
	{
		FFlockSetupFinding Finding;
		Finding.Id = FName(Id);
		Finding.Severity = Severity;
		Finding.Title = MoveTemp(Title);
		Finding.Detail = MoveTemp(Detail);
		Finding.Fix = Fix;
		return Finding;
	}

	bool IsBlank(const FString& Value)
	{
		return Value.TrimStartAndEnd().IsEmpty();
	}

	/** Sort order only — higher is more severe, so Error floats to the top. */
	int32 SeverityRank(EFlockSetupSeverity Severity)
	{
		switch (Severity)
		{
		case EFlockSetupSeverity::Error:   return 2;
		case EFlockSetupSeverity::Warning: return 1;
		default:                           return 0;
		}
	}
}

TArray<FFlockSetupFinding> FFlockSetupStatus::Evaluate(const FFlockSetupInput& Input)
{
	TArray<FFlockSetupFinding> Findings;

	// ── Required credentials and identity ──

	if (IsBlank(Input.ApiUrl))
	{
		Findings.Add(MakeFinding(TEXT("Flock.Config.ApiUrl"), EFlockSetupSeverity::Error,
			LOCTEXT("ApiUrlMissingTitle", "API URL is empty"),
			LOCTEXT("ApiUrlMissingDetail", "Set the API URL in Project Settings > Flock SDK."),
			EFlockSetupFix::OpenSettings));
	}
	else if (!Input.ApiUrl.StartsWith(TEXT("http://")) && !Input.ApiUrl.StartsWith(TEXT("https://")))
	{
		// Caught here rather than at the first request, where it surfaces as an opaque transport failure.
		Findings.Add(MakeFinding(TEXT("Flock.Config.ApiUrl"), EFlockSetupSeverity::Error,
			LOCTEXT("ApiUrlSchemeTitle", "API URL has no http:// or https:// scheme"),
			FText::Format(LOCTEXT("ApiUrlSchemeDetail", "\"{0}\" will not resolve. Include the scheme."),
				FText::FromString(Input.ApiUrl)),
			EFlockSetupFix::OpenSettings));
	}

	if (IsBlank(Input.ApiKey))
	{
		Findings.Add(MakeFinding(TEXT("Flock.Config.ApiKey"), EFlockSetupSeverity::Error,
			LOCTEXT("ApiKeyMissingTitle", "API Key is empty"),
			LOCTEXT("ApiKeyMissingDetail", "Paste your API key from the Flock dashboard into Project Settings > Flock SDK."),
			EFlockSetupFix::OpenSettings));
	}

	if (IsBlank(Input.GameId))
	{
		Findings.Add(MakeFinding(TEXT("Flock.Config.GameId"), EFlockSetupSeverity::Error,
			LOCTEXT("GameIdMissingTitle", "Game Name is empty"),
			LOCTEXT("GameIdMissingDetail", "Set Game Name to your game's name from the Flock dashboard."),
			EFlockSetupFix::OpenSettings));
	}

	if (IsBlank(Input.GameVersion))
	{
		Findings.Add(MakeFinding(TEXT("Flock.Config.GameVersion"), EFlockSetupSeverity::Error,
			LOCTEXT("GameVersionMissingTitle", "Game Version is empty"),
			LOCTEXT("GameVersionMissingDetail", "Set Game Version to a version name from the Flock dashboard."),
			EFlockSetupFix::OpenSettings));
	}

	// A finding whose fix is blocked by another finding is noise, not help: resolving needs all four
	// fields, so reporting "unresolved" alongside "no API key" gives the developer nothing to act on.
	const bool bRequiredComplete = Findings.Num() == 0;

	if (bRequiredComplete && IsBlank(Input.GameVersionId))
	{
		Findings.Add(MakeFinding(TEXT("Flock.Version.Unresolved"), EFlockSetupSeverity::Error,
			LOCTEXT("VersionUnresolvedTitle", "Game Version ID is not resolved"),
			FText::Format(LOCTEXT("VersionUnresolvedDetail",
				"\"{0}\" has not been resolved to an ID yet, so the SDK cannot initialize. Resolving needs network access."),
				FText::FromString(Input.GameVersion)),
			EFlockSetupFix::Resolve));
	}

	// ── Connection probe ──
	// Only ever reflects an explicit Test Connection. NotRun says nothing, because silence is honest and
	// a probe on editor startup is network work nobody asked for.

	switch (Input.Probe)
	{
	case EFlockProbeState::Unreachable:
		Findings.Add(MakeFinding(TEXT("Flock.Connection.Unreachable"), EFlockSetupSeverity::Error,
			LOCTEXT("ProbeUnreachableTitle", "Could not reach the Flock API"),
			FText::Format(LOCTEXT("ProbeUnreachableDetail",
				"No response from {0}. Check the API URL and your network connection."),
				FText::FromString(Input.ApiUrl)),
			EFlockSetupFix::OpenSettings));
		break;

	case EFlockProbeState::KeyRejected:
		Findings.Add(MakeFinding(TEXT("Flock.Connection.KeyRejected"), EFlockSetupSeverity::Error,
			LOCTEXT("ProbeKeyTitle", "The API Key was rejected"),
			LOCTEXT("ProbeKeyDetail", "The server was reachable and the request well-formed, but this key was refused. Check it against the Flock dashboard."),
			EFlockSetupFix::OpenSettings));
		break;

	case EFlockProbeState::VersionNotFound:
		Findings.Add(MakeFinding(TEXT("Flock.Connection.VersionNotFound"), EFlockSetupSeverity::Error,
			LOCTEXT("ProbeVersionTitle", "No game version by that name"),
			FText::Format(LOCTEXT("ProbeVersionDetail", "The API key and game are valid, but \"{0}\" does not match a version on this game."),
				FText::FromString(Input.GameVersion)),
			EFlockSetupFix::OpenSettings));
		break;

	case EFlockProbeState::Failed:
		Findings.Add(MakeFinding(TEXT("Flock.Connection.Failed"), EFlockSetupSeverity::Error,
			LOCTEXT("ProbeFailedTitle", "The connection test failed"),
			FText::FromString(Input.ProbeMessage),
			EFlockSetupFix::TestConnection));
		break;

	default:
		break;
	}

	// ── Game name ──
	// Never sent to the server (the API key identifies the game), so nothing can reject it and this is
	// not an Error — the SDK initializes fine with the wrong name. It still matters: the name keys the
	// token store, so changing it later signs everyone out, and until then the settings page names a
	// different game than the project actually talks to.

	if (!IsBlank(Input.ProbeServerGameName) &&
		!IsBlank(Input.GameId) &&
		!Input.ProbeServerGameName.Equals(Input.GameId.TrimStartAndEnd(), ESearchCase::CaseSensitive))
	{
		Findings.Add(MakeFinding(TEXT("Flock.Connection.GameNameMismatch"), EFlockSetupSeverity::Warning,
			LOCTEXT("GameNameMismatchTitle", "Game Name does not match this API key's game"),
			FText::Format(LOCTEXT("GameNameMismatchDetail",
				"Settings say \"{0}\"; this API key belongs to \"{1}\". The name is not sent to the server, but it keys the saved-login store — changing it later signs existing players out."),
				FText::FromString(Input.GameId),
				FText::FromString(Input.ProbeServerGameName)),
			EFlockSetupFix::OpenSettings));
	}

	// ── Codegen drift ──
	// The cheap check only: manifest version against the baked version, no network. Detecting an *edited*
	// schema at the same version needs a fresh snapshot, which is what Sync Schemas is for.

	if (Input.bCodegenManifestPresent &&
		!IsBlank(Input.GameVersionId) &&
		!Input.CodegenManifestVersionId.Equals(Input.GameVersionId, ESearchCase::CaseSensitive))
	{
		Findings.Add(MakeFinding(TEXT("Flock.Codegen.Drift"), EFlockSetupSeverity::Warning,
			LOCTEXT("CodegenDriftTitle", "Generated code is from a different game version"),
			LOCTEXT("CodegenDriftDetail", "The last schema sync generated for a different version than the one baked here. Re-sync to bring them back in line."),
			EFlockSetupFix::SyncSchemas));
	}

	// ── SDK version ──
	// A first add (no last-seen record) is not an upgrade, and must not read as one.

	if (!IsBlank(Input.LastSeenSdkVersion) &&
		!IsBlank(Input.CurrentSdkVersion) &&
		!Input.LastSeenSdkVersion.Equals(Input.CurrentSdkVersion, ESearchCase::CaseSensitive))
	{
		Findings.Add(MakeFinding(TEXT("Flock.Sdk.Upgraded"), EFlockSetupSeverity::Info,
			LOCTEXT("SdkUpgradedTitle", "The Flock SDK was updated"),
			FText::Format(LOCTEXT("SdkUpgradedDetail", "Updated from {0} to {1}. See CHANGELOG.md for what changed."),
				FText::FromString(Input.LastSeenSdkVersion),
				FText::FromString(Input.CurrentSdkVersion)),
			EFlockSetupFix::None));
	}

	// Most severe first. Stable, so the deliberate emission order above survives inside a severity band.
	Algo::StableSortBy(Findings, [](const FFlockSetupFinding& Finding) { return -SeverityRank(Finding.Severity); });

	return Findings;
}

bool FFlockSetupStatus::HasErrors(const TArray<FFlockSetupFinding>& Findings)
{
	return Findings.ContainsByPredicate(
		[](const FFlockSetupFinding& Finding) { return Finding.Severity == EFlockSetupSeverity::Error; });
}

TArray<FFlockSetupFinding> FFlockSetupStatus::AtLeast(const TArray<FFlockSetupFinding>& Findings,
	EFlockSetupSeverity Min)
{
	return Findings.FilterByPredicate(
		[Min](const FFlockSetupFinding& Finding) { return SeverityRank(Finding.Severity) >= SeverityRank(Min); });
}

#undef LOCTEXT_NAMESPACE
