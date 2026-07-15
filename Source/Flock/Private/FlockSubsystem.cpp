// Copyright 2022, Qwacks. All Rights Reserved.

#include "FlockSubsystem.h"
#include "Flock.h"
#include "Config/FlockConfig.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

const FString UFlockSubsystem::ApiVersion = TEXT("v1");
const FString UFlockSubsystem::SdkVersion = TEXT("0.3.0");

UFlockSubsystem* UFlockSubsystem::Get(const UObject* WorldContextObject)
{
	if (!GEngine)
	{
		return nullptr;
	}

	if (const UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull))
	{
		if (UGameInstance* GameInstance = World->GetGameInstance())
		{
			return GameInstance->GetSubsystem<UFlockSubsystem>();
		}
	}

	return nullptr;
}

void UFlockSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const UFlockConfig* Config = GetDefault<UFlockConfig>();
	if (!Config->bAutoInitializeOnLoad)
	{
		// The game initializes the SDK itself (e.g. after a splash screen or EULA).
		GetLogger().LogInfo(
			TEXT("Auto-Initialize On Load is off; the SDK stays uninitialized until you call InitializeFromSettings()/InitializeWithConfig()."));
		return;
	}

	InitializeFromSettings();
}

void UFlockSubsystem::Deinitialize()
{
	ShutdownSdk();
	Super::Deinitialize();
}

void UFlockSubsystem::InitializeFromSettings()
{
	const UFlockConfig* Config = GetDefault<UFlockConfig>();

	FString ValidationError;
	if (!Config->IsValid(ValidationError))
	{
		InitializationError = FString::Printf(
			TEXT("Flock config is incomplete: %s Open Project Settings > Flock SDK to fix it, or turn off Auto-Initialize On Load."),
			*ValidationError);
		GetLogger().LogError(InitializationError);
		OnFlockInitializationFailed.Broadcast(InitializationError);
		return;
	}

	InitializeWithConfig(FFlockInitConfig::FromSettings(*Config));
}

void UFlockSubsystem::InitializeWithConfig(const FFlockInitConfig& Config)
{
	// Honor this config's debug-logs flag for the default logger (unless a custom one was injected).
	if (!Logger.IsValid())
	{
		Logger = MakeShared<FFlockUnrealLogger>(Config.bEnableDebugLogs);
	}

	// Misuse guard for an already-initialized SDK. Don't broadcast a
	// failure: the SDK is already initialized and working.
	if (bInitialized)
	{
		GetLogger().LogWarning(
			TEXT("SDK is already initialized. Call ShutdownSdk() first to re-initialize with a different config."));
		return;
	}

	GetLogger().LogInfo(TEXT("Initializing Flock SDK."));

	FString Error;
	if (!TryInitialize(Config, Error))
	{
		InitializationError = Error;
		GetLogger().LogError(FString::Printf(TEXT("Initialize failed: %s"), *Error));
		OnFlockInitializationFailed.Broadcast(Error);
		return;
	}

	bInitialized = true;
	InitializationError.Empty();
	GetLogger().LogInfo(FString::Printf(TEXT("SDK initialized (GameId=%s, GameVersionId=%s)."),
		*ActiveConfig.GameId, *ActiveConfig.GameVersionId));
	OnFlockInitialized.Broadcast();
}

bool UFlockSubsystem::TryInitialize(const FFlockInitConfig& Config, FString& OutError)
{
	// The Game Version ID is baked at edit time; runtime init never contacts the server.
	if (Config.GameVersionId.IsEmpty())
	{
		OutError = TEXT("Game Version not resolved. Open Tools > Flock > Resolve Game Version (or Project Settings > Flock SDK) ")
			TEXT("while online to resolve your Game Version, then rebuild. The Game Version ID is baked at edit time; ")
			TEXT("runtime init never contacts the server.");
		return false;
	}

	ActiveConfig = Config;
	// NOTE: SDK providers (Authentication / Config / Game / Player / ...) are constructed here in later tickets.
	return true;
}

void UFlockSubsystem::ShutdownSdk()
{
	if (!bInitialized)
	{
		return;
	}

	// NOTE: provider teardown happens here in later tickets.
	ActiveConfig = FFlockInitConfig();
	bInitialized = false;
	GetLogger().LogInfo(TEXT("SDK shut down."));
}

FString UFlockSubsystem::GetVersionedApiUrl() const
{
	return FString::Printf(TEXT("%s/%s"), *ActiveConfig.ApiUrl, *ApiVersion);
}

void UFlockSubsystem::SetLogger(const TSharedRef<IFlockLogger>& InLogger)
{
	Logger = InLogger;
}

IFlockLogger& UFlockSubsystem::GetLogger()
{
	if (!Logger.IsValid())
	{
		Logger = MakeShared<FFlockUnrealLogger>(GetDefault<UFlockConfig>()->bEnableDebugLogs);
	}
	return *Logger;
}
