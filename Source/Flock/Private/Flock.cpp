// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Flock.h"
#include "Multiplayer/Relay/FlockRelayConnection.h"

#define LOCTEXT_NAMESPACE "FFlockModule"

DEFINE_LOG_CATEGORY(LogFlock);

void FFlockModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is
	// specified in the .uplugin file per-module.
}

void FFlockModule::ShutdownModule()
{
	// No relay thread may outlive the module's code: each gives its address back, if its owner had not, and is waited for.
	FFlockRelayConnection::StopEveryRelayThread();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFlockModule, Flock)
