// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

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

	/**
	 * Records a diagnostic message (spooled, delivered on the next flush). Ignored without consent. Not for
	 * gameplay — use Flock Track Event.
	 *
	 * Build Extra Data with the Flock Metadata nodes, which turn a number or a flag into the string the
	 * diagnostics wire stores.
	 *
	 * Surface: log_event — read on Diagnostics → Events.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Diagnostics", meta = (WorldContext = "WorldContextObject",
		AutoCreateRefTerm = "ExtraData", DisplayName = "Flock Log Diagnostic Event"))
	static void LogDiagnosticEvent(const UObject* WorldContextObject, const FString& Message,
		const TMap<FString, FString>& ExtraData);

	/**
	 * Records a recoverable logic fault. Leave Details at its default if you have nothing to add.
	 *
	 * Surface: log_event — read on Diagnostics → Errors.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Diagnostics", meta = (WorldContext = "WorldContextObject",
		AutoCreateRefTerm = "Details", DisplayName = "Flock Log Diagnostic Error"))
	static void LogDiagnosticError(const UObject* WorldContextObject, const FString& Message, const FFlockLogDetails& Details);

	/**
	 * Records an exception you report yourself. Leave Stack Trace empty to have the callstack captured for you.
	 *
	 * Surface: log_event — read on Diagnostics → Errors.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Diagnostics", meta = (WorldContext = "WorldContextObject",
		AutoCreateRefTerm = "Details", DisplayName = "Flock Log Diagnostic Exception"))
	static void LogDiagnosticException(const UObject* WorldContextObject, const FString& Message, const FString& StackTrace,
		const FFlockLogDetails& Details);

	/**
	 * The former name of Flock Log Diagnostic Event, kept so existing graphs keep working. It sat beside the
	 * analytics nodes under one category and read like the way to record gameplay, which it never was.
	 *
	 * Surface: log_event — read on Diagnostics → Events.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Diagnostics", meta = (WorldContext = "WorldContextObject",
		AutoCreateRefTerm = "ExtraData", DisplayName = "Flock Log Event", DeprecatedFunction,
		DeprecationMessage = "Renamed to Flock Log Diagnostic Event, which is the surface it writes to. Flock Track Event is the one the Game Metrics dashboards read."))
	static void LogEvent(const UObject* WorldContextObject, const FString& Message, const TMap<FString, FString>& ExtraData);

	/**
	 * The former name of Flock Log Diagnostic Error, kept so existing graphs keep working.
	 *
	 * Surface: log_event — read on Diagnostics → Errors.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Diagnostics", meta = (WorldContext = "WorldContextObject",
		AutoCreateRefTerm = "Details", DisplayName = "Flock Log Error", DeprecatedFunction,
		DeprecationMessage = "Renamed to Flock Log Diagnostic Error, which is the surface it writes to."))
	static void LogError(const UObject* WorldContextObject, const FString& Message, const FFlockLogDetails& Details);

	/**
	 * The former name of Flock Log Diagnostic Exception, kept so existing graphs keep working.
	 *
	 * Surface: log_event — read on Diagnostics → Errors.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Diagnostics", meta = (WorldContext = "WorldContextObject",
		AutoCreateRefTerm = "Details", DisplayName = "Flock Log Exception", DeprecatedFunction,
		DeprecationMessage = "Renamed to Flock Log Diagnostic Exception, which is the surface it writes to."))
	static void LogException(const UObject* WorldContextObject, const FString& Message, const FString& StackTrace,
		const FFlockLogDetails& Details);

	/**
	 * Counts a screen/menu view against the current session.
	 *
	 * Surface: analytics — read on Dashboards → Game Metrics.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics", meta = (WorldContext = "WorldContextObject",
		DisplayName = "Flock Record Screen View"))
	static void RecordScreenView(const UObject* WorldContextObject, const FString& ScreenName);

	/**
	 * Records a gameplay event for the Game Metrics dashboards — not Flock Log Diagnostic Event, which writes an
	 * engineering entry. Build Properties with the Flock Event Property nodes, which keep a number a number.
	 * Returns false when refused (analytics off, no consent, an empty name, or the reserved session_started).
	 *
	 * Surface: analytics — read on Dashboards → Game Metrics.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Analytics", meta = (WorldContext = "WorldContextObject",
		AutoCreateRefTerm = "Properties", DisplayName = "Flock Track Event"))
	static bool TrackEvent(const UObject* WorldContextObject, const FString& EventName, const FFlockCommandData& Properties,
		const FString& EventCategory);

	/** What automatic exception capture can see in this build (Shipping and Test lose error log lines and ensures). */
	UFUNCTION(BlueprintPure, Category = "Flock|Diagnostics", meta = (WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Exception Capture Coverage"))
	static FFlockExceptionCaptureCoverage GetExceptionCaptureCoverage(const UObject* WorldContextObject);

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

	// ── Commands ──

	/**
	 * Offline commands waiting to be replayed for the signed-in player — for a "syncing…" indicator. Money
	 * commands are never queued, so this never counts one. Zero before initialization.
	 */
	UFUNCTION(BlueprintPure, Category = "Flock|Commands", meta = (WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Pending Command Count"))
	static int32 GetPendingCommandCount(const UObject* WorldContextObject);

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
