// Copyright 2022, Qwacks. All Rights Reserved.

#include "Guards/FlockConfigValidator.h"
#include "Config/FlockConfig.h"
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
	const FString Reason = GetBuildBlockReason(Config->GameVersionId, bCanResolve, Config->bFailBuildIfVersionUnresolved);

	if (!Reason.IsEmpty())
	{
		AssetFails(InAsset, FText::FromString(Reason));
		return EDataValidationResult::Invalid;
	}

	AssetPasses(InAsset);
	return EDataValidationResult::Valid;
}

FString UFlockConfigValidator::GetBuildBlockReason(const FString& BakedGameVersionId, bool bCanResolve, bool bGuardEnabled)
{
	if (!bGuardEnabled)
	{
		return FString();
	}

	// Inert until a real (non-stub) lookup is registered — never block a package during the stub period.
	if (!bCanResolve)
	{
		return FString();
	}

	if (BakedGameVersionId.IsEmpty())
	{
		return TEXT("Flock Game Version ID is not resolved. Open Tools > Flock > Resolve Game Version while online ")
			TEXT("before building, or turn off 'Fail Build If Version Unresolved' in Project Settings > Flock SDK.");
	}

	return FString();
}
