// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

namespace FlockDevelopmentCommands
{
	/**
	 * The device id `Flock.LoginWithDevice` signs in with: the one given, as it is, or empty without one, which the device calls
	 * read as this install's own (ThisDeviceId).
	 */
	FString DeviceIdToSignInWith(const TArray<FString>& Arguments);
}

#endif // !UE_BUILD_SHIPPING
