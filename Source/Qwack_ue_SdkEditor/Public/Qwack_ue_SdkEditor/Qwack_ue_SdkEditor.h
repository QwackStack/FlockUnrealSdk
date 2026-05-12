#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class SNotificationItem;

class FQwack_ue_SdkEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void CheckFirstRun();
	void OpenFlockSettings();
	void DismissNotification();

	TWeakPtr<SNotificationItem> ActiveNotification;
};
