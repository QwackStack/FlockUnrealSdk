// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

DECLARE_LOG_CATEGORY_EXTERN(LogFlockEditor, Log, All);

/**
 * Editor module for the Flock SDK: registers the play-mode setup guard and the
 * Tools > Flock > Resolve Game Version action. The build/validate guard (UFlockConfigValidator)
 * is discovered automatically by the Data Validation system.
 */
class FFlockEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();
	static void OnResolveGameVersion();
};
