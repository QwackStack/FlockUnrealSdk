// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

/**
 * Injects a setup-status banner above the Flock SDK properties in Project Settings.
 *
 * The second view of the one findings list. A developer who goes looking in Project Settings finds the
 * same answer as one who opens the Flock tab: two surfaces that each know different things is the failure
 * this design exists to prevent.
 */
class FFlockConfigDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	/** The banner's first line for these findings, ordered most severe first; empty when there are none. */
	static FText MakeHeadline(const TArray<struct FFlockSetupFinding>& Findings);

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
};
