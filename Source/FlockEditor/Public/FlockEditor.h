// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

DECLARE_LOG_CATEGORY_EXTERN(LogFlockEditor, Log, All);

struct FPropertyChangedEvent;

/**
 * Editor module for the Flock SDK: registers the play-mode setup guard, the
 * Tools > Flock > Resolve Game Version action, and edit-time auto-baking of the Game Version ID.
 * The build/validate guard (UFlockConfigValidator) is discovered automatically by Data Validation.
 */
class FFlockEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * Tools > Flock > Sync Schemas: fetch the backend schema and regenerate everything from it. Public
	 * because the `Flock.SyncSchemas` console command shares it — the menu cannot be driven headlessly,
	 * and a sync has to be verifiable against a real backend.
	 */
	static void OnSyncSchemas();

	/**
	 * Tools > Flock > Clean Generated: delete everything a sync wrote. Public for the same reason as
	 * OnSyncSchemas — `Flock.CleanGenerated` shares it.
	 */
	static void OnCleanGenerated();

private:
	void RegisterMenus();
	static void OnResolveGameVersion();

	// Auto-bake: resolve+bake the Game Version ID without the manual menu click. Fires when a resolve
	// input (API URL/Key or Game Version) is edited to a valid state, and once on editor startup if the
	// project has a version name but no baked ID. Runtime init stays synchronous + network-free.
	void OnConfigPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent);
	void OnEditorInitialized(double Duration);
	static void TryAutoResolve(const TCHAR* Reason);

	FDelegateHandle ConfigChangedHandle;
	FDelegateHandle EditorInitializedHandle;
};
