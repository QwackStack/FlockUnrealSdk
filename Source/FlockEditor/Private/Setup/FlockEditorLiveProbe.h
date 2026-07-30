// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Models/FlockAuthModels.h"
#include "Models/FlockAnalyticsModels.h"
#include "UObject/Object.h"
#include "UObject/WeakObjectPtr.h"
#include "FlockEditorLiveProbe.generated.h"

class UFlockEvents;

/**
 * Bridges the SDK's event hub to the (non-UObject) Flock panel.
 *
 * This class exists because every delegate on UFlockEvents is a
 * DECLARE_DYNAMIC_MULTICAST_DELEGATE — dynamic delegates bind only to UFUNCTIONs on a UObject, so they
 * cannot take a lambda or a Slate widget method. A small forwarding UObject is not a workaround; it is
 * the only way to observe the hub at all.
 *
 * The panel holds this in a TStrongObjectPtr. Without a strong reference it would be collected out from
 * under the hub mid-session.
 */
UCLASS()
class UFlockEditorLiveProbe : public UObject
{
	GENERATED_BODY()

public:
	/** Fires on every hub event, with a short human-readable line for the panel's activity list. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnLiveEvent, const FString& /*Line*/);
	FOnLiveEvent OnLiveEvent;

	/** Binds all thirteen hub delegates. Safe to call repeatedly; rebinds cleanly. */
	void Bind(UFlockEvents* Events);

	/** Mandatory on EndPIE — the panel outlives the session that raised these events. */
	void Unbind();

private:
	UFUNCTION() void HandleInitialized();
	UFUNCTION() void HandleInitializationFailed(const FString& Error);
	UFUNCTION() void HandleShutdown();
	UFUNCTION() void HandleAuthenticated(const FFlockAuthInfo& Info);
	UFUNCTION() void HandleTokenRefreshed();
	UFUNCTION() void HandleAuthExpired();
	UFUNCTION() void HandleLoggedOut();
	UFUNCTION() void HandleSessionRestored(bool bRestored);
	UFUNCTION() void HandleSessionStarted(const FString& SessionId);
	UFUNCTION() void HandleSessionEnded(const FFlockSessionEndedArgs& Args);
	UFUNCTION() void HandleSessionPaused();
	UFUNCTION() void HandleSessionResumed();
	UFUNCTION() void HandleConsentChanged(bool bGranted);

	void Emit(const FString& Line);

	UPROPERTY(Transient)
	TWeakObjectPtr<UFlockEvents> Bound;
};
