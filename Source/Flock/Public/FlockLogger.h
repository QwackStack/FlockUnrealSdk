// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Sink for Flock SDK breadcrumbs and errors. Implement this and inject it via
 * UFlockSubsystem::SetLogger() to route SDK logs into your own telemetry or an on-screen debugger.
 */
class FLOCK_API IFlockLogger
{
public:
	virtual ~IFlockLogger() = default;

	virtual void LogDebug(const FString& Message) = 0;
	virtual void LogInfo(const FString& Message) = 0;
	virtual void LogWarning(const FString& Message) = 0;
	virtual void LogError(const FString& Message) = 0;
};

/**
 * The call site the SDK is dispatching for right now — "C++", or "Blueprint 'bpTest'".
 *
 * Ambient rather than threaded through every signature because it exists **only** to decorate log lines:
 * carrying it from a Blueprint node down through the provider, the retry handler and into the HTTP client
 * would put a debugging aid in the argument list of every call in the SDK.
 *
 * Game-thread only, and readers take a **copy at dispatch time** — the HTTP client stamps it onto the
 * request line and carries that copy into the completion, so a response landing after some other call has
 * started cannot be attributed to the wrong caller.
 *
 * Providers keep their own copy too (FFlockProviderBase::SetCallOrigin writes both), so a provider's log
 * lines and the HTTP lines underneath them always agree.
 */
struct FLOCK_API FFlockCallOrigin
{
	static const FString& Get();
	static void Set(const FString& Origin);
};

/**
 * Default logger: routes to the LogFlock category with a "[Flock SDK]" prefix. When not verbose
 * (Enable Debug Logs off) Debug/Info are suppressed, but Warning/Error still surface — a UE-native
 * adjustment so startup failures are never silently swallowed.
 */
class FLOCK_API FFlockUnrealLogger : public IFlockLogger
{
public:
	explicit FFlockUnrealLogger(bool bInVerbose)
		: bVerbose(bInVerbose)
	{
	}

	virtual void LogDebug(const FString& Message) override;
	virtual void LogInfo(const FString& Message) override;
	virtual void LogWarning(const FString& Message) override;
	virtual void LogError(const FString& Message) override;

private:
	bool bVerbose;
};

/** Silences all SDK logging. Inject via SetLogger() to fully opt out. */
class FLOCK_API FFlockNullLogger : public IFlockLogger
{
public:
	virtual void LogDebug(const FString& Message) override {}
	virtual void LogInfo(const FString& Message) override {}
	virtual void LogWarning(const FString& Message) override {}
	virtual void LogError(const FString& Message) override {}
};
