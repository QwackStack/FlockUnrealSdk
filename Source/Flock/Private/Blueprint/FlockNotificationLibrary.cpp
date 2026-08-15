// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockNotificationLibrary.h"

#include "Providers/FlockNotificationProvider.h"

bool UFlockNotificationLibrary::GetCurrentDevicePlatform(EFlockDevicePlatform& Platform)
{
	// Delegates to the provider's static rather than re-reading the platform name, so the Blueprint answer
	// and the one Register Device Token acts on can never disagree.
	return FFlockNotificationProvider::GetCurrentDevicePlatform(Platform);
}
