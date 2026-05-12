// Copyright Epic Games, Inc. All Rights Reserved.

#include "Qwack_ue_Sdk.h"

#define LOCTEXT_NAMESPACE "FQwack_ue_SdkModule"
DEFINE_LOG_CATEGORY(LOG_FLOCK_GAME_SDK);
DEFINE_LOG_CATEGORY(LOG_QWACK_SDK);

void FQwack_ue_SdkModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
}

void FQwack_ue_SdkModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FQwack_ue_SdkModule, Qwack_ue_Sdk)