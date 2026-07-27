// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Models/FlockAnalyticsModels.h"
#include "FlockLibrary.generated.h"

class UFlockEvents;

/**
 * Blueprint convenience nodes that resolve the Flock subsystem from the calling graph's world context, so
 * a graph can log an event, read auth state, and so on without first getting the Flock Subsystem and wiring
 * it into a Target pin. Each is a plain (non-async) node — the calls are fire-and-forget or a simple state
 * read, so there is no outcome pin to wait on — the way UGameplayStatics exposes subsystem-backed helpers.
 *
 * These are a thin facade over UFlockSubsystem: every node is a safe no-op / default when the SDK cannot be
 * resolved or is not yet initialized, exactly like the subsystem methods they forward to. One-time setup
 * (Initialize / Shutdown) stays on the subsystem, where you already have it in hand.
 */
UCLASS()
class FLOCK_API UFlockLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// ── Analytics (fire-and-forget + state) ──

	/** Records a diagnostic message (spooled, delivered on the next flush). Ignored without consent. */
	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics", meta = (WorldContext = "WorldContextObject",
		AutoCreateRefTerm = "ExtraData", DisplayName = "Flock Log Event"))
	static void LogEvent(const UObject* WorldContextObject, const FString& Message, const TMap<FString, FString>& ExtraData);

	/** Records a recoverable logic fault. Leave Details at its default if you have nothing to add. */
	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics", meta = (WorldContext = "WorldContextObject",
		AutoCreateRefTerm = "Details", DisplayName = "Flock Log Error"))
	static void LogError(const UObject* WorldContextObject, const FString& Message, const FFlockLogDetails& Details);

	/** Records an exception you report yourself. Leave Stack Trace empty to have the callstack captured for you. */
	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics", meta = (WorldContext = "WorldContextObject",
		AutoCreateRefTerm = "Details", DisplayName = "Flock Log Exception"))
	static void LogException(const UObject* WorldContextObject, const FString& Message, const FString& StackTrace,
		const FFlockLogDetails& Details);

	/** Counts a screen/menu view against the current session. */
	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics", meta = (WorldContext = "WorldContextObject",
		DisplayName = "Flock Record Screen View"))
	static void RecordScreenView(const UObject* WorldContextObject, const FString& ScreenName);

	/** Grants or withdraws analytics consent, persisted across runs. */
	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics", meta = (WorldContext = "WorldContextObject",
		DisplayName = "Flock Set Analytics Consent"))
	static void SetAnalyticsConsent(const UObject* WorldContextObject, bool bGranted);

	UFUNCTION(BlueprintPure, Category = "Flock|Analytics", meta = (WorldContext = "WorldContextObject",
		DisplayName = "Flock Has Analytics Consent"))
	static bool HasAnalyticsConsent(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Flock|Analytics", meta = (WorldContext = "WorldContextObject",
		DisplayName = "Flock Has Active Analytics Session"))
	static bool HasActiveAnalyticsSession(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Flock|Analytics", meta = (WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Analytics Session Id"))
	static FString GetAnalyticsSessionId(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Flock|Analytics", meta = (WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Analytics Snapshot"))
	static FFlockSessionSnapshot GetAnalyticsSnapshot(const UObject* WorldContextObject);

	/** Drops the local spool, the consent decision, and any crash tombstone. */
	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics", meta = (WorldContext = "WorldContextObject",
		DisplayName = "Flock Erase Local Analytics Data"))
	static void EraseLocalAnalyticsData(const UObject* WorldContextObject);

	// ── Auth state ──

	UFUNCTION(BlueprintPure, Category = "Flock|Auth", meta = (WorldContext = "WorldContextObject",
		DisplayName = "Flock Is Authenticated"))
	static bool IsAuthenticated(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Flock|Auth", meta = (WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Player Id"))
	static FString GetPlayerId(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Flock|Auth", meta = (WorldContext = "WorldContextObject",
		DisplayName = "Flock Is Restoring Session"))
	static bool IsRestoringSession(const UObject* WorldContextObject);

	/** Logs the current player out locally; safe when signed out. */
	UFUNCTION(BlueprintCallable, Category = "Flock|Auth", meta = (WorldContext = "WorldContextObject",
		DisplayName = "Flock Logout"))
	static void Logout(const UObject* WorldContextObject);

	// ── Lifecycle / info ──

	UFUNCTION(BlueprintPure, Category = "Flock", meta = (WorldContext = "WorldContextObject",
		DisplayName = "Flock Is Initialized"))
	static bool IsInitialized(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Flock", meta = (WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Initialization Error"))
	static FString GetInitializationError(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Flock", meta = (WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Game Id"))
	static FString GetGameId(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Flock", meta = (WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Game Version Id"))
	static FString GetGameVersionId(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Flock", meta = (WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Api Url"))
	static FString GetApiUrl(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Flock", meta = (WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Versioned Api Url"))
	static FString GetVersionedApiUrl(const UObject* WorldContextObject);

	/** The SDK event hub (lifecycle/auth/session/consent); bind its events without grabbing the subsystem. */
	UFUNCTION(BlueprintPure, Category = "Flock", meta = (WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Events"))
	static UFlockEvents* GetEvents(const UObject* WorldContextObject);
};
