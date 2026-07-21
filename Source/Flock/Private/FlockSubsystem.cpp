// Copyright 2022, Qwacks. All Rights Reserved.

#include "FlockSubsystem.h"
#include "Flock.h"
#include "FlockEvents.h"
#include "Auth/FlockFileTokenStore.h"
#include "Config/FlockConfig.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

const FString UFlockSubsystem::ApiVersion = TEXT("v1");
const FString UFlockSubsystem::SdkVersion = TEXT("0.6.0");

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
		GetEvents()->InvokeInitializationFailed(InitializationError);
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
		if (Events)
		{
			Events->SetLogger(Logger);
		}
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
		GetEvents()->InvokeInitializationFailed(Error);
		return;
	}

	bInitialized = true;
	InitializationError.Empty();
	GetLogger().LogInfo(FString::Printf(TEXT("SDK initialized (GameId=%s, GameVersionId=%s)."),
		*ActiveConfig.GameId, *ActiveConfig.GameVersionId));
	GetEvents()->InvokeInitialized();

	// Resume any persisted session in the background; the outcome surfaces via
	// OnSessionRestored / OnAuthenticated rather than a return value.
	AuthProvider->TryRestoreSession(nullptr);
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

	const UFlockConfig* Settings = GetDefault<UFlockConfig>();
	FFlockRetryPolicy RetryPolicy;
	RetryPolicy.MaxRetries = Settings->RetryMaxRetries;
	RetryPolicy.bUseJitter = Settings->bRetryUseJitter;

	GetLogger(); // make sure the shared logger exists before wiring it into the auth stack
	const TSharedRef<IFlockLogger> LoggerRef = Logger.ToSharedRef();

	HttpClient = TestHttpAdapter.IsValid()
		? MakeShared<FFlockHttpClient>(TestHttpAdapter.ToSharedRef(), LoggerRef, Settings->HttpTimeoutSeconds)
		: FFlockHttpClient::CreateDefault(Settings->HttpTimeoutSeconds, LoggerRef);

	TokenStore = TestTokenStore.IsValid()
		? TestTokenStore
		: TSharedPtr<IFlockTokenStore>(MakeShared<FFlockFileTokenStore>(FFlockFileTokenStore::DefaultPath(), Config.GameId));

	AuthSession = MakeShared<FFlockAuthSession>(HttpClient.ToSharedRef(), TokenStore, LoggerRef,
		GetVersionedApiUrl(), Config.GetBaseHeaders());
	AuthSession->SetEvents(GetEvents());
	AuthProvider = MakeUnique<FFlockAuthProvider>(HttpClient.ToSharedRef(), RetryPolicy, LoggerRef,
		AuthSession.ToSharedRef(), GetEvents(), GetVersionedApiUrl());
	return true;
}

void UFlockSubsystem::ShutdownSdk()
{
	if (!bInitialized)
	{
		return;
	}

	AuthProvider.Reset();
	AuthSession.Reset();
	HttpClient.Reset();
	TokenStore.Reset();
	ActiveConfig = FFlockInitConfig();
	bInitialized = false;
	GetLogger().LogInfo(TEXT("SDK shut down."));
	GetEvents()->InvokeShutdown();
}

FString UFlockSubsystem::GetVersionedApiUrl() const
{
	return FString::Printf(TEXT("%s/%s"), *ActiveConfig.ApiUrl, *ApiVersion);
}

UFlockEvents* UFlockSubsystem::GetEvents()
{
	if (!Events)
	{
		Events = NewObject<UFlockEvents>(this);
		Events->SetLogger(Logger);
	}
	return Events;
}

void UFlockSubsystem::SetLogger(const TSharedRef<IFlockLogger>& InLogger)
{
	Logger = InLogger;
	if (Events)
	{
		Events->SetLogger(Logger);
	}
}

IFlockLogger& UFlockSubsystem::GetLogger()
{
	if (!Logger.IsValid())
	{
		Logger = MakeShared<FFlockUnrealLogger>(GetDefault<UFlockConfig>()->bEnableDebugLogs);
		if (Events)
		{
			Events->SetLogger(Logger);
		}
	}
	return *Logger;
}

bool UFlockSubsystem::IsAuthenticated() const
{
	return AuthSession.IsValid() && AuthSession->IsAuthenticated();
}

FString UFlockSubsystem::GetPlayerId() const
{
	return AuthSession.IsValid() ? AuthSession->GetPlayerId() : FString();
}

bool UFlockSubsystem::IsRestoringSession() const
{
	return AuthProvider.IsValid() && AuthProvider->IsRestoringSession();
}

void UFlockSubsystem::Logout()
{
	if (!AuthProvider.IsValid())
	{
		GetLogger().LogWarning(TEXT("Logout called before the SDK was initialized; nothing to do."));
		return;
	}
	AuthProvider->Logout();
}
