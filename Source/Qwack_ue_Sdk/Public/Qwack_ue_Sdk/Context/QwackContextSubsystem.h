// Default fields merged into every analytics `properties` and log `extra_data` block.
// Snapshots platform/device once, persists an install GUID, tracks gameplay time
// (minus backgrounded time), and caches the live session_id.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "QwackContextSubsystem.generated.h"

UCLASS()
class QWACK_UE_SDK_API UQwackContextSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// Existing keys in Target are kept; caller values always win.
	void MergeDefaults(const TSharedRef<FJsonObject>& Target) const;

	void SetSessionId(const FString& InSessionId);
	void ClearSessionId();

	FString GetInstallId() const { return InstallId; }

	double GetGameplayTimeSeconds() const;

private:
	void SnapshotPlatform();
	void LoadOrCreateInstallId();

	void OnEnterBackground();
	void OnEnterForeground();

	FDelegateHandle DeactivateHandle;
	FDelegateHandle ReactivateHandle;
	FDelegateHandle BackgroundHandle;
	FDelegateHandle ForegroundHandle;

	FString InstallId;
	FString SessionId;

	FString Platform;
	FString DeviceType;
	FString DeviceModel;
	FString OsVersion;
	FString CpuBrand;
	FString GpuBrand;
	FString Locale;
	FString EngineVersionString;
	FString SdkVersionString;

	FDateTime SessionStartUtc;
	double BackgroundEnterMonotonic = 0.0;
	double BackgroundAccumulatedSec = 0.0;
	bool bInBackground = false;
};
