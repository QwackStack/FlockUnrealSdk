// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LOG_FLOCK_GAME_SDK, Log, All);
DECLARE_LOG_CATEGORY_EXTERN(LOG_QWACK_SDK, Log, All);
class FQwack_ue_SdkModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
