// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "EditorValidatorBase.h"
#include "Setup/FlockSetupStatus.h"
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
	 * Takes findings rather than the baked ID alone, which closes a real hole: the old check looked only
	 * at GameVersionId, so a key rotated or cleared *after* the ID was baked still packaged clean and
	 * failed at runtime instead.
	 *
	 * @param Findings      everything FFlockSetupStatus reports about this project
	 * @param bCanResolve   whether a real (non-stub) lookup is registered
	 * @param bGuardEnabled UFlockConfig::bFailBuildIfVersionUnresolved
	 */
	static FString GetBuildBlockReason(const TArray<FFlockSetupFinding>& Findings, bool bCanResolve, bool bGuardEnabled);
};
