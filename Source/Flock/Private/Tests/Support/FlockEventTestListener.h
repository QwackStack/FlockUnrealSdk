// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FlockEventModels.h"
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
};
