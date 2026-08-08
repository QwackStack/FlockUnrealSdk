// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockSubsystem.h"

#include "Analytics/FlockFileEventCache.h"
#include "Flock.h"
#include "FlockEvents.h"
#include "Auth/FlockFileTokenStore.h"
#include "Config/FlockConfig.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

const FString UFlockSubsystem::ApiVersion = TEXT("v1");
const FString UFlockSubsystem::SdkVersion = TEXT("1.1.0");

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

	// Raise the log category so debug breadcrumbs actually reach the console.
	//
	// Without this, Enable Debug Logs appears to do nothing: LogDebug writes at Verbose, LogFlock is
	// declared at Log, and UE filters anything below a category's verbosity — so every breadcrumb was
	// built, formatted, and then dropped. Raised, never lowered, so a `Log LogFlock Verbose` typed at the
	// console still wins when the setting is off.
	if (Config.bEnableDebugLogs && LogFlock.GetVerbosity() < ELogVerbosity::Verbose)
	{
		LogFlock.SetVerbosity(ELogVerbosity::Verbose);
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

	// Before the restore below can raise OnAuthenticated and open a new session: this drains any
	// crash tombstone left by the previous run and starts one for this run.
	if (AnalyticsProvider.IsValid())
	{
		AnalyticsProvider->Initialize();
	}

	// Also before the restore: the restore raises OnAuthenticated, which is what replays a queue left by
	// the previous run, and that needs the pump's triggers already live.
	if (CommandProvider.IsValid())
	{
		CommandProvider->Initialize();
	}

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

	// Read-side offline cache, shared by config and game. Built before them and pruned to the current game
	// version so switching versions doesn't accumulate stale trees. Null when disabled in settings, in
	// which case the providers degrade to plain fetches.
	if (Settings->bEnableOfflineCache)
	{
		SnapshotStore = MakeShared<FFlockSnapshotStore>(Settings->OfflineCacheDirectory, LoggerRef, SdkVersion);
		SnapshotStore->PruneOtherVersions(Config.GameVersionId);
	}

	ConfigProvider = MakeShared<FFlockConfigProvider>(HttpClient.ToSharedRef(), RetryPolicy, LoggerRef,
		AuthSession.ToSharedRef(), GetVersionedApiUrl(), SnapshotStore, Config.GameVersionId);
	GameProvider = MakeShared<FFlockGameProvider>(HttpClient.ToSharedRef(), RetryPolicy, LoggerRef,
		AuthSession.ToSharedRef(), GetVersionedApiUrl(), SnapshotStore, Config.GameVersionId);
	// No analytics dependency, so it slots in with config/game rather than after the analytics block.
	PlayerProvider = MakeShared<FFlockPlayerProvider>(HttpClient.ToSharedRef(), RetryPolicy, LoggerRef,
		AuthSession.ToSharedRef(), GetVersionedApiUrl(), SnapshotStore, Config.GameVersionId);

	const FFlockAnalyticsConfig AnalyticsConfig = FFlockAnalyticsConfig::FromSettings(*Settings);
	if (AnalyticsConfig.bEnabled)
	{
		FFlockAnalyticsDependencies Deps;
		// A cap of zero is how "don't spool" is expressed, so the caching switch maps onto it.
		Deps.LogEventCache = MakeShared<FFlockFileEventCache>(TEXT("log_events"),
			AnalyticsConfig.bCacheFailedEvents ? AnalyticsConfig.MaxCachedEvents : 0);
		// Its own queue, so ends drain ahead of events and erasing one leaves the other alone.
		Deps.SessionEndCache = MakeShared<FFlockFileEventCache>(TEXT("session_ends"),
			AnalyticsConfig.bCacheFailedEvents ? AnalyticsConfig.MaxCachedSessionEnds : 0);
		Deps.Session = MakeShared<FFlockSession>(AnalyticsConfig);
		// Off in the editor: a PIE shutdown is not a real app death and would be reported as a crash.
		Deps.TerminationTracker = MakeShared<FFlockTerminationTracker>(
			AnalyticsConfig.bPersistSessionOnDisk && !GIsEditor);
		Deps.ConsentStore = MakeShared<FFlockConsentStore>();
		Deps.Pump = MakeShared<FFlockLifecyclePump>();
		Deps.bEnableLogSink = true;

		AnalyticsProvider = MakeShared<FFlockAnalyticsProvider>(HttpClient.ToSharedRef(), RetryPolicy, LoggerRef,
			AuthSession.ToSharedRef(), GetEvents(), GetVersionedApiUrl(), AnalyticsConfig, Deps,
			Config.GameVersionId, SdkVersion);

		// The backend requires a player id on a session, so sessions follow sign-in rather than init.
		GetEvents()->OnAuthenticated.AddDynamic(this, &UFlockSubsystem::HandleAnalyticsAuthenticated);
		GetEvents()->OnLoggedOut.AddDynamic(this, &UFlockSubsystem::HandleAnalyticsLoggedOut);
	}

	// After the analytics block so the shop can record purchase transactions through it. The reference is
	// weak and may be null (analytics disabled) — the shop provider skips recording when it can't pin it.
	ShopProvider = MakeShared<FFlockShopProvider>(HttpClient.ToSharedRef(), RetryPolicy, LoggerRef,
		AuthSession.ToSharedRef(), GetVersionedApiUrl(), SnapshotStore, Config.GameVersionId);
	ShopProvider->SetAnalyticsProvider(AnalyticsProvider);

	// After the player provider, which it writes mutated rows back through. Signing in is one of its
	// auto-flush triggers; the other two come from its own lifecycle pump, started in Initialize() below.
	CommandProvider = MakeShared<FFlockCommandProvider>(HttpClient.ToSharedRef(), RetryPolicy, LoggerRef,
		AuthSession.ToSharedRef(), GetVersionedApiUrl(), SnapshotStore, Config.GameVersionId);
	CommandProvider->SetPlayerProvider(PlayerProvider);
	GetEvents()->OnAuthenticated.AddDynamic(this, &UFlockSubsystem::HandleCommandsAuthenticated);

	// Assets are game-version scoped rather than player scoped, so nothing here is cleared on logout — the
	// snapshot store's PruneOtherVersions above already parks a previous version's index. The binary cache
	// is its own directory under the persistent download path, deliberately not the snapshot store: that
	// one is JSON-payload only and unbudgeted, which is the wrong shape for a hundred megabytes of images.
	AssetProvider = MakeShared<FFlockAssetProvider>(HttpClient.ToSharedRef(), RetryPolicy, LoggerRef,
		AuthSession.ToSharedRef(), GetVersionedApiUrl(), SnapshotStore, Config.GameVersionId,
		FlockCreateHttpAssetDownloader(LoggerRef),
		MakeShared<FFlockAssetCache>(Settings->AssetCacheDirectory, Settings->AssetCacheMaxSizeMB, LoggerRef));
	AssetProvider->Configure(Settings->bEnableAssetCache, static_cast<float>(Settings->AssetDownloadTimeoutSeconds),
		Settings->AssetDownloadRetryCount, Settings->AssetMaxConcurrentDownloads);

	// Supply the reachability probe every snapshot-backed provider has always had a seam for and never a
	// production value for — left null, IsServerReachable() answered "reachable" unconditionally, so the
	// branch that serves cache *without* a call could only ever fire in a test. One shared latch on the
	// HTTP client, since a provider cannot classify a transport failure and every provider sends through it.
	//
	// Done here, after the last provider is built and before anything can fetch. The command provider is
	// included for AddGameFunds, which must refuse rather than queue when the server is unreachable.
	{
		const TSharedRef<FFlockHttpClient> ClientRef = HttpClient.ToSharedRef();
		auto Probe = [ClientRef]() { return !ClientRef->IsLikelyOffline(); };
		ConfigProvider->SetReachabilityProbe(Probe);
		GameProvider->SetReachabilityProbe(Probe);
		PlayerProvider->SetReachabilityProbe(Probe);
		ShopProvider->SetReachabilityProbe(Probe);
		CommandProvider->SetReachabilityProbe(Probe);
		AssetProvider->SetReachabilityProbe(Probe);
	}

	return true;
}

void UFlockSubsystem::HandleAnalyticsAuthenticated(const FFlockAuthInfo& Info)
{
	if (AnalyticsProvider.IsValid() && GetDefault<UFlockConfig>()->bAnalyticsAutoStartSession)
	{
		AnalyticsProvider->StartSession(Info.PlayerId);
	}
}

void UFlockSubsystem::HandleCommandsAuthenticated(const FFlockAuthInfo& Info)
{
	if (CommandProvider.IsValid())
	{
		CommandProvider->FlushPendingWrites();
	}
}

void UFlockSubsystem::HandleAnalyticsLoggedOut()
{
	if (AnalyticsProvider.IsValid())
	{
		AnalyticsProvider->HandleLoggedOut();
	}
}

void UFlockSubsystem::LogAnalyticsEvent(const FString& Message, const TMap<FString, FString>& ExtraData)
{
	if (AnalyticsProvider.IsValid())
	{
		AnalyticsProvider->LogEvent(Message, ExtraData);
	}
}

void UFlockSubsystem::LogAnalyticsError(const FString& Message, const FFlockLogDetails& Details)
{
	if (AnalyticsProvider.IsValid())
	{
		AnalyticsProvider->LogError(Message, Details);
	}
}

void UFlockSubsystem::LogAnalyticsException(const FString& Message, const FString& StackTrace,
	const FFlockLogDetails& Details)
{
	if (AnalyticsProvider.IsValid())
	{
		AnalyticsProvider->LogException(Message, StackTrace, Details);
	}
}

void UFlockSubsystem::RecordAnalyticsScreenView(const FString& ScreenName)
{
	if (AnalyticsProvider.IsValid())
	{
		AnalyticsProvider->RecordScreenView(ScreenName);
	}
}

void UFlockSubsystem::SetAnalyticsConsent(bool bGranted)
{
	if (AnalyticsProvider.IsValid())
	{
		AnalyticsProvider->SetConsent(bGranted);
	}
}

bool UFlockSubsystem::HasAnalyticsConsent() const
{
	return AnalyticsProvider.IsValid() && AnalyticsProvider->HasConsent();
}

bool UFlockSubsystem::HasActiveAnalyticsSession() const
{
	return AnalyticsProvider.IsValid() && AnalyticsProvider->HasActiveSession();
}

FString UFlockSubsystem::GetAnalyticsSessionId() const
{
	return AnalyticsProvider.IsValid() ? AnalyticsProvider->GetCurrentSessionId() : FString();
}

FFlockSessionSnapshot UFlockSubsystem::GetAnalyticsSnapshot() const
{
	return AnalyticsProvider.IsValid() ? AnalyticsProvider->GetCurrentSnapshot() : FFlockSessionSnapshot();
}

void UFlockSubsystem::EraseLocalAnalyticsData()
{
	if (AnalyticsProvider.IsValid())
	{
		AnalyticsProvider->EraseLocalData();
	}
}

void UFlockSubsystem::ShutdownSdk()
{
	if (!bInitialized)
	{
		return;
	}

	// Shut the provider down while it is still alive — its destructor deliberately will not end a
	// session, because that would dispatch HTTP from a destructor.
	if (AnalyticsProvider.IsValid())
	{
		AnalyticsProvider->Shutdown();
		AnalyticsProvider.Reset();
	}
	// Same reason: stop the pump while the provider is alive rather than leaving it to the destructor.
	// Anything still queued stays on disk and replays next run.
	if (CommandProvider.IsValid())
	{
		CommandProvider->Shutdown();
		CommandProvider.Reset();
	}
	if (Events != nullptr)
	{
		Events->OnAuthenticated.RemoveDynamic(this, &UFlockSubsystem::HandleAnalyticsAuthenticated);
		Events->OnLoggedOut.RemoveDynamic(this, &UFlockSubsystem::HandleAnalyticsLoggedOut);
		Events->OnAuthenticated.RemoveDynamic(this, &UFlockSubsystem::HandleCommandsAuthenticated);
	}

	ConfigProvider.Reset();
	GameProvider.Reset();
	ShopProvider.Reset();
	PlayerProvider.Reset();
	AssetProvider.Reset();
	SnapshotStore.Reset();
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

int32 UFlockSubsystem::GetPendingCommandCount() const
{
	return CommandProvider.IsValid() ? CommandProvider->GetPendingWriteCount() : 0;
}

bool UFlockSubsystem::IsLikelyOffline() const
{
	return HttpClient.IsValid() && HttpClient->IsLikelyOffline();
}

void UFlockSubsystem::Logout()
{
	if (!AuthProvider.IsValid())
	{
		GetLogger().LogWarning(TEXT("Logout called before the SDK was initialized; nothing to do."));
		return;
	}
	// Before the tokens go: closing the session needs a bearer, and OnLoggedOut — which is where the
	// analytics side would otherwise hear about this — only fires once they have been cleared. The
	// end is spooled either way, so a failure here costs delivery time, not the record.
	if (AnalyticsProvider.IsValid())
	{
		AnalyticsProvider->EndSession(EFlockSessionEndReason::Logout);
	}
	// Drop the signed-out player's cached data so a later sign-in never reads a stale row. Templates and
	// their snapshot are game-version-scoped, not player-scoped, so they are left intact.
	if (PlayerProvider.IsValid())
	{
		PlayerProvider->ClearPlayerDataCache();
	}
	AuthProvider->Logout();
}
