// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

/**
 * Injects a setup-status banner above the Flock SDK properties in Project Settings.
 *
 * The second view of the one findings list. A developer who goes looking in Project Settings finds the
 * same answer as one who opens the Flock tab, which is the failure mode this whole design exists to
 * remove — the canonical SDK's popup and its settings page each knew different things.
 */
class FFlockConfigDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
};
