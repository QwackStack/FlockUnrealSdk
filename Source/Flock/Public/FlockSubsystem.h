// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Auth/FlockAuthSession.h"
#include "Auth/FlockTokenStore.h"
#include "FlockInitConfig.h"
#include "FlockLogger.h"
#include "Http/FlockHttpAdapter.h"
#include "Http/FlockHttpClient.h"
#include "Providers/FlockAuthProvider.h"
#include "FlockSubsystem.generated.h"

class UFlockEvents;

/**
 * The global Flock SDK accessor.
 *
 * A GameInstanceSubsystem is created automatically when the game starts (PIE and packaged) and lives
 * for the whole game session. Fetch it with UFlockSubsystem::Get(WorldContext) or the standard
 * GetGameInstance()->GetSubsystem<UFlockSubsystem>().
 *
 * Initialization is synchronous and needs no network: the Game Version ID is resolved and baked at
 * edit time (Tools > Flock > Resolve Game Version), and runtime init uses it directly.
 *
 * The Authentication provider is wired (with automatic session restore after init); the remaining
 * feature providers (Config / Game / Player / Commands / Shop / Asset / Analytics) land in later
 * releases.
 */
UCLASS()
class FLOCK_API UFlockSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/**
	 * API version segment appended to the API URL for all SDK HTTP calls. Single source of truth —
	 * bump here when the backend cuts a new major API version.
	 */
	static const FString ApiVersion;

	/**
	 * This plugin's SDK version. Keep in sync with Flock.uplugin's VersionName and CHANGELOG.md —
	 * bump all three together. Reported to the backend when an SDK-version request header is added.
	 */
	static const FString SdkVersion;

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
	 * C++ only (IFlockLogger is not a UObject).
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

	/**
	 * The SDK event hub (lifecycle/auth/session/consent). Bind its events or use its
	 * CallOrRegister entry points; always valid.
	 */
	UFUNCTION(BlueprintPure, Category = "Flock")
	UFlockEvents* GetEvents();

	// ── Authentication ──

	/**
	 * The authentication provider — every login/register/account call lives here. Null before
	 * initialization and after shutdown. C++ API; Blueprint uses the Flock auth async nodes.
	 */
	FFlockAuthProvider* GetAuthProvider() const { return AuthProvider.Get(); }

	/** True when a player is signed in. */
	UFUNCTION(BlueprintPure, Category = "Flock|Auth")
	bool IsAuthenticated() const;

	/** The signed-in player's id from the access-token claims; empty when signed out. */
	UFUNCTION(BlueprintPure, Category = "Flock|Auth")
	FString GetPlayerId() const;

	/** True while a persisted session is being restored (kicked automatically after init). */
	UFUNCTION(BlueprintPure, Category = "Flock|Auth")
	bool IsRestoringSession() const;

	/** Logs the current player out locally; safe when signed out. Server-side revocation is separate (RevokeToken). */
	UFUNCTION(BlueprintCallable, Category = "Flock|Auth")
	void Logout();

	// ── Test seams (call before initialization; unused by games) ──

	/** Routes SDK HTTP through the given adapter instead of the engine HTTP module. */
	void SetHttpAdapterForTesting(const TSharedPtr<IFlockHttpAdapter>& InAdapter) { TestHttpAdapter = InAdapter; }

	/** Persists tokens through the given store instead of the encrypted file store. */
	void SetTokenStoreForTesting(const TSharedPtr<IFlockTokenStore>& InStore) { TestTokenStore = InStore; }

private:
	/** Applies the baked-version gate and adopts the config. Returns false with OutError on failure. */
	bool TryInitialize(const FFlockInitConfig& Config, FString& OutError);

	/** The active logger, lazily defaulted from the project's Enable Debug Logs setting when unset. */
	IFlockLogger& GetLogger();

	bool bInitialized = false;
	FString InitializationError;
	FFlockInitConfig ActiveConfig;
	TSharedPtr<IFlockLogger> Logger;

	/** Backing store for GetEvents(); created on first use so events can be bound before init. */
	UPROPERTY(Transient)
	TObjectPtr<UFlockEvents> Events;

	// Auth stack, built per-initialization and dropped on shutdown. Everything is shared-ptr
	// backed; in-flight callbacks hold what they need, so teardown mid-request stays safe.
	TSharedPtr<FFlockHttpClient> HttpClient;
	TSharedPtr<IFlockTokenStore> TokenStore;
	TSharedPtr<FFlockAuthSession> AuthSession;
	TUniquePtr<FFlockAuthProvider> AuthProvider;

	TSharedPtr<IFlockHttpAdapter> TestHttpAdapter;
	TSharedPtr<IFlockTokenStore> TestTokenStore;
};
