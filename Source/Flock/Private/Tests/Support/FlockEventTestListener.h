// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FlockEventModels.h"
#include "Http/FlockError.h"
#include "Models/FlockAuthModels.h"
#include "FlockEventTestListener.generated.h"

/**
 * Bind target for the event automation tests: dynamic delegates need UFUNCTION handlers on a UObject,
 * so plain lambdas can't subscribe. Not guarded by WITH_AUTOMATION_TESTS on purpose — UHT generates
 * its registration unconditionally.
 */
UCLASS()
class UFlockEventTestListener : public UObject
{
	GENERATED_BODY()

public:
	int32 InitializedCount = 0;
	int32 FailedCount = 0;
	FString LastError;
	int32 ShutdownCount = 0;
	int32 AuthenticatedCount = 0;
	FFlockAuthInfo LastAuthInfo;
	int32 ConsentCount = 0;
	bool bLastConsent = false;
	int32 SessionEndedCount = 0;
	FFlockSessionEndedArgs LastSessionEnded;
	int32 TokenRefreshedCount = 0;
	int32 AuthExpiredCount = 0;
	int32 LoggedOutCount = 0;
	int32 SessionRestoredCount = 0;
	bool bLastSessionRestored = false;

	UFUNCTION()
	void HandleInitialized() { ++InitializedCount; }

	UFUNCTION()
	void HandleInitializationFailed(const FString& Error) { ++FailedCount; LastError = Error; }

	UFUNCTION()
	void HandleShutdown() { ++ShutdownCount; }

	UFUNCTION()
	void HandleAuthenticated(const FFlockAuthInfo& Info) { ++AuthenticatedCount; LastAuthInfo = Info; }

	UFUNCTION()
	void HandleConsentChanged(bool bGranted) { ++ConsentCount; bLastConsent = bGranted; }

	UFUNCTION()
	void HandleSessionEnded(const FFlockSessionEndedArgs& Args) { ++SessionEndedCount; LastSessionEnded = Args; }

	UFUNCTION()
	void HandleTokenRefreshed() { ++TokenRefreshedCount; }

	UFUNCTION()
	void HandleAuthExpired() { ++AuthExpiredCount; }

	UFUNCTION()
	void HandleLoggedOut() { ++LoggedOutCount; }

	UFUNCTION()
	void HandleSessionRestored(bool bRestored) { ++SessionRestoredCount; bLastSessionRestored = bRestored; }
};

/** Bind target for the auth async-node pin tests (dynamic delegates need UFUNCTION handlers). */
UCLASS()
class UFlockAuthNodeTestListener : public UObject
{
	GENERATED_BODY()

public:
	int32 LoginPinCount = 0;
	int32 RegisterPinCount = 0;
	int32 RestorePinCount = 0;
	int32 AccountPinCount = 0;
	int32 NamePinCount = 0;
	FFlockError LastError;

	UFUNCTION()
	void HandleLoginPin(const FFlockPlayerLoginResponse& Response, const FFlockError& Error) { ++LoginPinCount; LastError = Error; }

	UFUNCTION()
	void HandleRegisterPin(const FFlockRegisterResult& Result, const FFlockError& Error) { ++RegisterPinCount; LastError = Error; }

	UFUNCTION()
	void HandleRestorePin() { ++RestorePinCount; }

	UFUNCTION()
	void HandleAccountPin(const FFlockError& Error) { ++AccountPinCount; LastError = Error; }

	UFUNCTION()
	void HandleNamePin(bool bAvailable, const FFlockError& Error) { ++NamePinCount; LastError = Error; }
};
