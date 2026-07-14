// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "FlockInitConfig.h"
#include "FlockLogger.h"
#include "FlockSubsystem.generated.h"

/** Broadcast once the SDK has successfully initialized. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFlockOnInitialized);

/** Broadcast when initialization fails; carries the error message. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockOnInitializationFailed, const FString&, Error);

/**
 * The global Flock SDK accessor.
 *
 * A GameInstanceSubsystem is created automatically when the game starts (PIE and packaged) and lives
 * for the whole game session — the UE analog of the Unity SDK's static FlockClient that auto-initializes
 * before the first scene loads. Fetch it with UFlockSubsystem::Get(WorldContext) or the standard
 * GetGameInstance()->GetSubsystem<UFlockSubsystem>().
 *
 * Initialization is synchronous and needs no network: the Game Version ID is resolved and baked at
 * edit time (Tools > Flock > Resolve Game Version), and runtime init uses it directly.
 *
 * NOTE (foundation slate): SDK providers (Authentication / Config / Game / Player / Commands / Shop /
 * Asset / Analytics) are wired into this subsystem in later tickets. This branch delivers the accessor,
 * auto/manual init, init state + events, and the baked-version gate.
 */
UCLASS()
class FLOCK_API UFlockSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/**
	 * API version segment appended to the API URL for all SDK HTTP calls. Single source of truth —
	 * bump here (and in the Unity SDK for parity) when the backend cuts a new major API version.
	 */
	static const FString ApiVersion;

	/** Returns the Flock subsystem for the given world context, or null if unavailable. */
	UFUNCTION(BlueprintPure, Category = "Flock", meta = (WorldContext = "WorldContextObject"))
	static UFlockSubsystem* Get(const UObject* WorldContextObject);

	//~ Begin USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem interface

	/** Validates the project's UFlockConfig and initializes the SDK from it. No-op (with warning) if already initialized. */
	UFUNCTION(BlueprintCallable, Category = "Flock")
	void InitializeFromSettings();

	/** Initializes the SDK from an explicit config. No-op (with warning) if already initialized; fails cleanly if the Game Version ID is missing. */
	UFUNCTION(BlueprintCallable, Category = "Flock")
	void InitializeWithConfig(const FFlockInitConfig& Config);

	/** Tears down SDK state, allowing a later re-initialization. */
	UFUNCTION(BlueprintCallable, Category = "Flock")
	void ShutdownSdk();

	/**
	 * Injects a custom logger so SDK breadcrumbs/errors flow into your own telemetry or debugger.
	 * C++ only (IFlockLogger is not a UObject), mirroring Unity's FlockClient.Create(config, logger).
	 * If never called, a default logger is used (verbose when Enable Debug Logs is on).
	 */
	void SetLogger(const TSharedRef<IFlockLogger>& InLogger);

	/** True once initialization has succeeded. */
	UFUNCTION(BlueprintPure, Category = "Flock")
	bool IsInitialized() const { return bInitialized; }

	/** The last initialization error; empty when none, or after a success. */
	UFUNCTION(BlueprintPure, Category = "Flock")
	FString GetInitializationError() const { return InitializationError; }

	UFUNCTION(BlueprintPure, Category = "Flock")
	FString GetGameId() const { return ActiveConfig.GameId; }

	UFUNCTION(BlueprintPure, Category = "Flock")
	FString GetGameVersionId() const { return ActiveConfig.GameVersionId; }

	UFUNCTION(BlueprintPure, Category = "Flock")
	FString GetApiUrl() const { return ActiveConfig.ApiUrl; }

	/** API base URL with the ApiVersion segment appended (e.g. https://api-flock.qwacks.com/v1). */
	UFUNCTION(BlueprintPure, Category = "Flock")
	FString GetVersionedApiUrl() const;

	/** Fires after the SDK successfully initializes. */
	UPROPERTY(BlueprintAssignable, Category = "Flock")
	FFlockOnInitialized OnFlockInitialized;

	/** Fires when initialization fails; carries the error message. */
	UPROPERTY(BlueprintAssignable, Category = "Flock")
	FFlockOnInitializationFailed OnFlockInitializationFailed;

private:
	/** Applies the baked-version gate and adopts the config. Returns false with OutError on failure. */
	bool TryInitialize(const FFlockInitConfig& Config, FString& OutError);

	/** The active logger, lazily defaulted from the project's Enable Debug Logs setting when unset. */
	IFlockLogger& GetLogger();

	bool bInitialized = false;
	FString InitializationError;
	FFlockInitConfig ActiveConfig;
	TSharedPtr<IFlockLogger> Logger;
};
