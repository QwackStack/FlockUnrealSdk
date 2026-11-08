// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockOnlineStoreConnection.h"
#include "Modules/ModuleManager.h"
#include "Store/FlockStore.h"

/** Hands Flock its way to the platform's store as the module starts, and takes it back as the module shuts down. */
class FFlockPlatformStoreModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UFlockStore::SetStoreLibrary([]() -> TSharedPtr<IFlockStoreConnection> { return MakeShared<FFlockOnlineStoreConnection>(); });
	}

	virtual void ShutdownModule() override
	{
		UFlockStore::ClearStoreLibrary();
	}
};

IMPLEMENT_MODULE(FFlockPlatformStoreModule, FlockPlatformStore)
