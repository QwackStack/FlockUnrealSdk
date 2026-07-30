// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Setup/FlockSetupContext.h"
#include "Setup/FlockEditorUserSettings.h"
#include "Codegen/FlockCodegenManifest.h"
#include "Codegen/FlockCodegenPaths.h"
#include "Config/FlockConfig.h"
#include "FlockSubsystem.h"
#include "Misc/App.h"

namespace
{
	/** Session-scoped, deliberately not persisted. See MarkSummoned. */
	FFlockProbeResult GLastProbe;
	bool GSummonedThisSession = false;
}

FFlockSetupInput FFlockSetupContext::BuildInput()
{
	FFlockSetupInput Input;

	if (const UFlockConfig* Config = GetDefault<UFlockConfig>())
	{
		Input.ApiUrl = Config->ApiUrl;
		Input.ApiKey = Config->ApiKey;
		Input.GameId = Config->GameId;
		Input.GameVersion = Config->GameVersion;
		Input.GameVersionId = Config->GameVersionId;
	}

	// Cheap drift check only: the manifest's version against the baked one, no network. Detecting an
	// *edited* schema at the same version needs a fresh snapshot, which is what Sync Schemas is for.
	FString GeneratedRoot;
	FString PathError;
	if (FFlockCodegenPaths::TryResolveGeneratedRootFromSettings(GeneratedRoot, PathError))
	{
		FFlockCodegenManifest Manifest;
		if (FFlockCodegenManifest::TryRead(GeneratedRoot, Manifest))
		{
			Input.bCodegenManifestPresent = true;
			Input.CodegenManifestVersionId = Manifest.GameVersionId;
		}
	}

	Input.Probe = GLastProbe.State;
	Input.ProbeMessage = GLastProbe.Message;
	Input.ProbeServerGameName = GLastProbe.ServerGameName;

	Input.CurrentSdkVersion = UFlockSubsystem::SdkVersion;
	if (const UFlockEditorUserSettings* UserSettings = UFlockEditorUserSettings::Get())
	{
		Input.LastSeenSdkVersion = UserSettings->LastSeenSdkVersion;
	}

	return Input;
}

TArray<FFlockSetupFinding> FFlockSetupContext::Evaluate()
{
	return FFlockSetupStatus::Evaluate(BuildInput());
}

const FFlockProbeResult& FFlockSetupContext::LastProbe()
{
	return GLastProbe;
}

void FFlockSetupContext::SetLastProbe(const FFlockProbeResult& Result)
{
	GLastProbe = Result;
}

void FFlockSetupContext::ClearProbe()
{
	GLastProbe = FFlockProbeResult();
}

FFlockSummonContext FFlockSetupContext::BuildSummonContext(const TArray<FFlockSetupFinding>& Findings)
{
	FFlockSummonContext Context;
	Context.bHasError = FFlockSetupStatus::HasErrors(Findings);
	Context.bHeadless = FApp::IsUnattended() || IsRunningCommandlet();
	Context.bAlreadySummonedThisSession = GSummonedThisSession;

	if (const UFlockEditorUserSettings* UserSettings = UFlockEditorUserSettings::Get())
	{
		Context.bFirstAdd = UserSettings->IsFirstAdd();

		// Suppressed only when *every* error has been muted. One unmuted error is still worth opening
		// for — otherwise muting one finding would silence every future one alongside it.
		bool bAnyUnsuppressedError = false;
		for (const FFlockSetupFinding& Finding : Findings)
		{
			if (Finding.Severity == EFlockSetupSeverity::Error && !UserSettings->IsSuppressed(Finding.Id))
			{
				bAnyUnsuppressedError = true;
				break;
			}
		}
		Context.bSuppressedByUser = Context.bHasError && !bAnyUnsuppressedError;
	}

	return Context;
}

void FFlockSetupContext::MarkSummoned()
{
	GSummonedThisSession = true;
}

void FFlockSetupContext::ResetSessionStateForTesting()
{
	GLastProbe = FFlockProbeResult();
	GSummonedThisSession = false;
}
