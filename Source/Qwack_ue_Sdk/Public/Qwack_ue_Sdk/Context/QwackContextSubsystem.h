// SDK-owned default fields injected into every analytics event's `properties` block
// and every log event's `extra_data` block. Holds:
//   - a one-shot snapshot of platform/device info captured at Initialize
//   - a persistent per-install GUID (Saved/Flock/install_id.txt)
//   - session-lifetime accounting: process start, plus accumulated background time
//     subtracted from gameplay_time_sec via FCoreDelegates focus/background hooks
//   - the active analytics session_id, cached when StartSession succeeds
//
// Identity fields (player_id, game_version) are pulled lazily from Auth/Config at
// merge time so callers don't have to push state changes through this subsystem.

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

	// Merges SDK-owned defaults into Target. Keys already present in Target are
	// preserved — caller-supplied values always win on conflict.
	void MergeDefaults(const TSharedRef<FJsonObject>& Target) const;

	// Called by analytics StartSession / EndSession callbacks so subsequent events
	// pick up the active session id automatically.
	void SetSessionId(const FString& InSessionId);
	void ClearSessionId();

	FString GetInstallId() const { return InstallId; }

	// Wall-clock seconds since SDK init, minus accumulated background/deactivated time.
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
