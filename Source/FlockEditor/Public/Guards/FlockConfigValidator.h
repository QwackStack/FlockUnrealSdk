// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorValidatorBase.h"
#include "FlockConfigValidator.generated.h"

/**
 * Best-effort build/validate guard for the baked Game Version ID, implemented with UE Data Validation.
 *
 * Two honest caveats:
 *  - UE has no guaranteed C++ pre-package gate; Data Validation validators run on assets during
 *    Validate Assets / cook, not on project settings directly. Coverage is best-effort.
 *  - It stays inert unless a real version lookup is registered, so it never blocks a package when the
 *    version can't be resolved anyway. Schema-drift checks are deferred until codegen exists.
 */
UCLASS()
class UFlockConfigValidator : public UEditorValidatorBase
{
	GENERATED_BODY()

public:
	UFlockConfigValidator();

	virtual bool CanValidateAsset_Implementation(const FAssetData& InAssetData, UObject* InAsset, FDataValidationContext& InContext) const override;
	virtual EDataValidationResult ValidateLoadedAsset_Implementation(const FAssetData& InAssetData, UObject* InAsset, FDataValidationContext& InContext) override;

	/**
	 * Pure decision: returns the build-blocking message, or empty to allow the build. Testable
	 * without the engine or network.
	 *
	 * @param BakedGameVersionId  the ID currently baked on the config (may be empty)
	 * @param bCanResolve         whether a real (non-stub) lookup is registered
	 * @param bGuardEnabled       UFlockConfig::bFailBuildIfVersionUnresolved
	 */
	static FString GetBuildBlockReason(const FString& BakedGameVersionId, bool bCanResolve, bool bGuardEnabled);
};
