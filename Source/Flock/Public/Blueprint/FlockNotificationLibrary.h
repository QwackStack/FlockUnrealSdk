// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Models/FlockNotificationModels.h"
#include "FlockNotificationLibrary.generated.h"

/**
 * Pure Blueprint accessors over the notification models.
 *
 * These answer questions that need no network, so they need no async node. Each one delegates straight to
 * the struct it reads, so Blueprint and C++ cannot drift — pinned by Library.CppParity.
 */
UCLASS()
class FLOCK_API UFlockNotificationLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * True once the server has stamped a read time.
	 *
	 * Read state through this rather than testing Read At yourself: it is a nullable wire field, and an
	 * absent one arrives as an empty string.
	 */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Is Read"), Category = "Flock|Notifications")
	static bool IsNotificationRead(const FFlockNotification& Notification) { return Notification.IsRead(); }

	/** Still waiting to fire: neither delivered nor canceled. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Is Pending"), Category = "Flock|Notifications")
	static bool IsScheduledPending(const FFlockScheduledNotification& Scheduled) { return Scheduled.IsPending(); }

	/** Already delivered. Read from the timestamp, not the loose status string. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Is Delivered"), Category = "Flock|Notifications")
	static bool IsScheduledDelivered(const FFlockScheduledNotification& Scheduled) { return Scheduled.IsDelivered(); }

	/** Canceled before it fired. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Is Canceled"), Category = "Flock|Notifications")
	static bool IsScheduledCanceled(const FFlockScheduledNotification& Scheduled) { return Scheduled.IsCanceled(); }

	/**
	 * The platform this build would register a push token under, and whether the push backend supports it
	 * at all. False on desktop, console and in the editor.
	 *
	 * Branch on this to hide an "enable notifications" toggle where push cannot work, rather than letting
	 * Register Device Token fail and having to explain the error.
	 */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Get Current Device Platform"), Category = "Flock|Notifications")
	static bool GetCurrentDevicePlatform(EFlockDevicePlatform& Platform);

	/**
	 * The wire spelling of a device platform ("android", "ios", "web") — for a log line or a debug readout.
	 * Not needed to call Register Device Token, which takes the enum.
	 */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Device Platform To String"), Category = "Flock|Notifications")
	static FString DevicePlatformToString(EFlockDevicePlatform Platform) { return FlockDevicePlatformToWire(Platform); }

	/** The wire spelling of a delivery channel ("in_app", "email", "push"). */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Notification Channel To String"), Category = "Flock|Notifications")
	static FString NotificationChannelToString(EFlockNotificationChannel Channel) { return FlockNotificationChannelToWire(Channel); }
};
