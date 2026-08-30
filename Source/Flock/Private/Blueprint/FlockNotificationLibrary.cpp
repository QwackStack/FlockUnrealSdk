// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockNotificationLibrary.h"

#include "FlockSubsystem.h"
#include "Providers/FlockNotificationProvider.h"

#include "Providers/FlockNotificationProvider.h"

bool UFlockNotificationLibrary::GetCurrentDevicePlatform(EFlockDevicePlatform& Platform)
{
	// Delegates to the provider's static rather than re-reading the platform name, so the Blueprint answer
	// and the one Register Device Token acts on can never disagree.
	return FFlockNotificationProvider::GetCurrentDevicePlatform(Platform);
}

TArray<FFlockPendingSchedule> UFlockNotificationLibrary::GetPendingSchedules(UObject* WorldContextObject)
{
	// Safe empty answer when the SDK is not up: a settings screen asking "what have I got pending" before
	// initialization should render an empty list, not fail.
	const UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
	const FFlockNotificationProvider* Provider = Sdk ? Sdk->GetNotificationProvider() : nullptr;
	return Provider ? Provider->GetPendingSchedules() : TArray<FFlockPendingSchedule>();
}
