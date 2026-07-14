// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Notification-first PIE setup guard. When the game would auto-initialize the SDK but can't (missing
 * credentials or an unresolved Game Version), it warns via the Message Log and an editor toast on
 * entering Play — never silently entering a broken session, but non-blocking by default. Mirrors the
 * intent of the Unity SDK's FlockPlayModeGuard, adapted to UE. Registered by the FlockEditor module.
 */
class FFlockPlayModeGuard
{
public:
	static void Register();
	static void Unregister();

private:
	static void OnBeginPIE(const bool bIsSimulating);

	static FDelegateHandle BeginPIEHandle;
};
