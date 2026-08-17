// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Models/FlockNotificationModels.h"
#include "UObject/Object.h"
#include "FlockSelfTestListener.generated.h"

/**
 * Bind target for the self-test's notification-event steps.
 *
 * The events hub's delegates are `DECLARE_DYNAMIC_MULTICAST_DELEGATE`, and a dynamic delegate binds only
 * to a `UFUNCTION` on a `UObject` — a lambda cannot subscribe. The self-test is otherwise plain functions
 * in an anonymous namespace, so observing the events at all requires this one small object.
 *
 * **Not guarded by `!UE_BUILD_SHIPPING`**, unlike `FlockSelfTest.cpp` itself: UHT generates the class
 * registration unconditionally, so a guard here would leave the generated code referring to a type that
 * does not exist in a shipping build. Same reasoning as the automation tests' listener. Nothing
 * references it in a shipping build, so it costs a class registration and nothing else.
 */
UCLASS()
class UFlockSelfTestListener : public UObject
{
	GENERATED_BODY()

public:
	/** How many times the server reported an unread count during the sweep. */
	int32 UnreadCountEvents = 0;

	/** The most recent reported count; -1 until the first event. */
	int32 LastUnreadCount = -1;

	/** Ids of every notification announced as newly received, in raise order (oldest first). */
	TArray<FString> ReceivedIds;

	UFUNCTION()
	void HandleUnreadCountChanged(int32 UnreadCount)
	{
		++UnreadCountEvents;
		LastUnreadCount = UnreadCount;
	}

	UFUNCTION()
	void HandleNotificationReceived(const FFlockNotification& Notification)
	{
		ReceivedIds.Add(Notification.Id);
	}
};
