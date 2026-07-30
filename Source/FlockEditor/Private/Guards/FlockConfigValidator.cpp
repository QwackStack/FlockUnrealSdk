// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Guards/FlockConfigValidator.h"
#include "Config/FlockConfig.h"
#include "Setup/FlockSetupContext.h"
#include "Version/FlockVersionLookup.h"
#include "AssetRegistry/AssetData.h"
#include "Misc/DataValidation.h"

UFlockConfigValidator::UFlockConfigValidator()
{
	bIsEnabled = true;
}

bool UFlockConfigValidator::CanValidateAsset_Implementation(const FAssetData& InAssetData, UObject* InAsset, FDataValidationContext& InContext) const
{
	return InAsset != nullptr && InAsset->IsA<UFlockConfig>();
}

EDataValidationResult UFlockConfigValidator::ValidateLoadedAsset_Implementation(const FAssetData& InAssetData, UObject* InAsset, FDataValidationContext& InContext)
{
	const UFlockConfig* Config = Cast<UFlockConfig>(InAsset);
	if (!Config)
	{
		AssetPasses(InAsset);
		return EDataValidationResult::Valid;
	}

	const bool bCanResolve = FFlockVersionLookupRegistry::Get().CanResolve();
	const FString Reason = GetBuildBlockReason(
		FFlockSetupContext::Evaluate(), bCanResolve, Config->bFailBuildIfVersionUnresolved);

	if (!Reason.IsEmpty())
	{
		AssetFails(InAsset, FText::FromString(Reason));
		return EDataValidationResult::Invalid;
	}

	AssetPasses(InAsset);
	return EDataValidationResult::Valid;
}

FString UFlockConfigValidator::GetBuildBlockReason(const TArray<FFlockSetupFinding>& Findings, bool bCanResolve,
	bool bGuardEnabled)
{
	if (!bGuardEnabled)
	{
		return FString();
	}

	// Inert until a real lookup is registered — never block a package when the version can't be resolved.
	if (!bCanResolve)
	{
		return FString();
	}

	// Errors only. A warning by definition still initializes, so it must not stop a package — the same
	// severity policy the panel's auto-summon uses.
	const TArray<FFlockSetupFinding> Blocking =
		FFlockSetupStatus::AtLeast(Findings, EFlockSetupSeverity::Error);
	if (Blocking.Num() == 0)
	{
		return FString();
	}

	TArray<FString> Lines;
	Lines.Reserve(Blocking.Num() + 1);
	Lines.Add(TEXT("Flock is not ready to package:"));
	for (const FFlockSetupFinding& Finding : Blocking)
	{
		Lines.Add(FString::Printf(TEXT("  - %s. %s"), *Finding.Title.ToString(), *Finding.Detail.ToString()));
	}
	Lines.Add(TEXT("Fix these in the Flock panel (Tools > Flock > Flock Panel), or turn off "
		"'Fail Build If Version Unresolved' in Project Settings > Flock SDK."));

	return FString::Join(Lines, TEXT("\n"));
}
