// Copyright 2022, Qwack. All Rights Reserved.

#include "Flock.h"

#define LOCTEXT_NAMESPACE "FFlockModule"

DEFINE_LOG_CATEGORY(LogFlock);

void FFlockModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is
	// specified in the .uplugin file per-module.
}

void FFlockModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFlockModule, Flock)
