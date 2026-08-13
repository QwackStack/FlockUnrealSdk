// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockError.h"
#include "Http/FlockResult.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Models/FlockCommandModels.h"
#include "Models/FlockNotificationModels.h"
#include "FlockNotificationAsyncActions.generated.h"

/**
 * Blueprint async nodes for the notification inbox. Each resolves the SDK from its world context on
 * Activate and fires exactly one pin; when the SDK is unavailable the failure pin fires with a Validation
 * error.
 *
 * Every node here needs a signed-in player and fails with an Auth error without one — an inbox belongs to
 * a player, so there is nothing sensible to return when nobody is signed in.
 *
 * These are the in-app half of notifications and work on their own. Receiving a message as a *push* banner
 * additionally requires the game to register a device token, which is a separate call.
 */

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockNotificationPagePin, const FFlockNotificationPage&, Notifications, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockUnreadCountPin, int32, UnreadCount, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockNotificationSummaryPin, const FFlockNotificationSummary&, Summary, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockNotificationPin, const FFlockNotification&, Notification, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockMarkAllReadPin, const FFlockMarkAllReadResult&, Result, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockNotificationTemplatesPin, const TArray<FFlockNotificationTemplate>&, Templates, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockNotificationTemplatePin, const FFlockNotificationTemplate&, Template, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockScheduledNotificationPin, const FFlockScheduledNotification&, Scheduled, const FFlockError&, Error);

/** Fetches a page of the signed-in player's inbox, newest first. */
UCLASS()
class FLOCK_API UFlockGetNotificationsAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockNotificationPagePin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockNotificationPagePin OnFailure;

	/** Unread Only filters to messages the player has not opened. Page is 1-based. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Notifications", AdvancedDisplay = "Page,Limit"), Category = "Flock|Notifications")
	static UFlockGetNotificationsAction* GetNotifications(UObject* WorldContextObject, bool bUnreadOnly = false,
		int32 Page = 1, int32 Limit = 50);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockNotificationPage>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	bool bUnreadOnly = false;
	int32 Page = 1;
	int32 Limit = 50;
};

/** The unread message count — the cheapest call for a badge. */
UCLASS()
class FLOCK_API UFlockGetUnreadNotificationCountAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockUnreadCountPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockUnreadCountPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Unread Notification Count"), Category = "Flock|Notifications")
	static UFlockGetUnreadNotificationCountAction* GetUnreadCount(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<int32>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;
};

/** Unread count plus the most recent few messages, in one call — the bell-icon payload. */
UCLASS()
class FLOCK_API UFlockGetNotificationSummaryAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockNotificationSummaryPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockNotificationSummaryPin OnFailure;

	/** Limit caps the preview list, not the unread count. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Notification Summary", AdvancedDisplay = "Limit"), Category = "Flock|Notifications")
	static UFlockGetNotificationSummaryAction* GetSummary(UObject* WorldContextObject, int32 Limit = 10);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockNotificationSummary>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	int32 Limit = 10;
};

/** Marks one notification read and returns the updated row. Marking an already-read message succeeds. */
UCLASS()
class FLOCK_API UFlockMarkNotificationReadAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockNotificationPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockNotificationPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Mark Notification Read"), Category = "Flock|Notifications")
	static UFlockMarkNotificationReadAction* MarkRead(UObject* WorldContextObject, const FString& NotificationId);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockNotification>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString NotificationId;
};

/** Marks the whole inbox read. Updated is how many rows flipped; zero is a success, not a failure. */
UCLASS()
class FLOCK_API UFlockMarkAllNotificationsReadAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockMarkAllReadPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockMarkAllReadPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Mark All Notifications Read"), Category = "Flock|Notifications")
	static UFlockMarkAllNotificationsReadAction* MarkAllRead(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockMarkAllReadResult>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;
};

/** Every notification template this game can schedule against. Works signed out — the catalog is game-scoped. */
UCLASS()
class FLOCK_API UFlockGetNotificationTemplatesAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockNotificationTemplatesPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockNotificationTemplatesPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Notification Templates"), Category = "Flock|Notifications")
	static UFlockGetNotificationTemplatesAction* GetTemplates(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<TArray<FFlockNotificationTemplate>>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;
};

/** One template by the name shown on the dashboard. An unknown name fails as a Validation error. */
UCLASS()
class FLOCK_API UFlockGetNotificationTemplateByNameAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockNotificationTemplatePin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockNotificationTemplatePin OnFailure;

	/** An empty Locale takes the template's default. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Notification Template By Name", AdvancedDisplay = "Locale"), Category = "Flock|Notifications")
	static UFlockGetNotificationTemplateByNameAction* GetByName(UObject* WorldContextObject,
		const FString& TemplateName, const FString& Locale);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockNotificationTemplate>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString TemplateName;
	FString Locale;
};

/**
 * Asks the backend to deliver a templated notification later. Server-side, so it fires whether or not the
 * game is running.
 *
 * Addressed by template **name** — the thing a designer has from the dashboard. The id is resolved
 * internally and memoized. Build Variables with the Flock Command Data Set nodes; leaving Channels empty
 * lets the template's own defaults apply.
 */
UCLASS()
class FLOCK_API UFlockScheduleNotificationAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockScheduledNotificationPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockScheduledNotificationPin OnFailure;

	/** Deliver At is UTC. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Schedule Notification", AdvancedDisplay = "Variables,Channels"), Category = "Flock|Notifications")
	static UFlockScheduleNotificationAction* Schedule(UObject* WorldContextObject, const FString& TemplateName,
		FDateTime DeliverAtUtc, FFlockCommandData Variables, const TArray<EFlockNotificationChannel>& Channels);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockScheduledNotification>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString TemplateName;
	FDateTime DeliverAtUtc;
	FFlockCommandData Variables;
	TArray<EFlockNotificationChannel> Channels;
};

/** Cancels a scheduled notification and returns the canceled row. */
UCLASS()
class FLOCK_API UFlockCancelScheduledNotificationAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockScheduledNotificationPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockScheduledNotificationPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Cancel Scheduled Notification"), Category = "Flock|Notifications")
	static UFlockCancelScheduledNotificationAction* Cancel(UObject* WorldContextObject, const FString& ScheduledId);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockScheduledNotification>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString ScheduledId;
};
