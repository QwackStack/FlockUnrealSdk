// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockNotificationAsyncActions.h"

#include "Engine/BlueprintGeneratedClass.h"
#include "FlockSubsystem.h"
#include "Providers/FlockNotificationProvider.h"

namespace
{
	FString ResolveCallOrigin(const UObject* WorldContextObject)
	{
		if (const UBlueprintGeneratedClass* BlueprintClass =
			WorldContextObject ? Cast<UBlueprintGeneratedClass>(WorldContextObject->GetClass()) : nullptr)
		{
			FString AssetName = BlueprintClass->GetName();
			AssetName.RemoveFromEnd(TEXT("_C"));
			return FString::Printf(TEXT("Blueprint '%s'"), *AssetName);
		}
		return TEXT("Blueprint node");
	}

	FFlockNotificationProvider* ResolveNotifications(UObject* WorldContextObject, FFlockError& OutError)
	{
		UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
		FFlockNotificationProvider* Provider = Sdk ? Sdk->GetNotificationProvider() : nullptr;
		if (!Provider)
		{
			OutError = FFlockError::Make(EFlockErrorType::Validation,
				TEXT("Flock notifications are not available. Initialize the SDK first."));
		}
		return Provider;
	}
}

// Get Notifications

UFlockGetNotificationsAction* UFlockGetNotificationsAction::GetNotifications(UObject* WorldContextObject,
	bool bUnreadOnly, int32 Page, int32 Limit)
{
	UFlockGetNotificationsAction* Action = NewObject<UFlockGetNotificationsAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->bUnreadOnly = bUnreadOnly;
	Action->Page = Page;
	Action->Limit = Limit;
	return Action;
}

void UFlockGetNotificationsAction::Activate()
{
	FFlockError Error;
	FFlockNotificationProvider* Provider = ResolveNotifications(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockNotificationPage>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetNotificationsAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->GetNotifications(bUnreadOnly, Page, Limit, [WeakThis](TFlockResult<FFlockNotificationPage> Result)
	{
		if (UFlockGetNotificationsAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockGetNotificationsAction::Complete(const TFlockResult<FFlockNotificationPage>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockNotificationPage(), Result.Error);
	}
	SetReadyToDestroy();
}

// Get Unread Count

UFlockGetUnreadNotificationCountAction* UFlockGetUnreadNotificationCountAction::GetUnreadCount(UObject* WorldContextObject)
{
	UFlockGetUnreadNotificationCountAction* Action = NewObject<UFlockGetUnreadNotificationCountAction>();
	Action->WorldContextObject = WorldContextObject;
	return Action;
}

void UFlockGetUnreadNotificationCountAction::Activate()
{
	FFlockError Error;
	FFlockNotificationProvider* Provider = ResolveNotifications(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<int32>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetUnreadNotificationCountAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->GetUnreadCount([WeakThis](TFlockResult<int32> Result)
	{
		if (UFlockGetUnreadNotificationCountAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockGetUnreadNotificationCountAction::Complete(const TFlockResult<int32>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(0, Result.Error);
	}
	SetReadyToDestroy();
}

// Get Summary

UFlockGetNotificationSummaryAction* UFlockGetNotificationSummaryAction::GetSummary(UObject* WorldContextObject, int32 Limit)
{
	UFlockGetNotificationSummaryAction* Action = NewObject<UFlockGetNotificationSummaryAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Limit = Limit;
	return Action;
}

void UFlockGetNotificationSummaryAction::Activate()
{
	FFlockError Error;
	FFlockNotificationProvider* Provider = ResolveNotifications(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockNotificationSummary>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetNotificationSummaryAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->GetSummary(Limit, [WeakThis](TFlockResult<FFlockNotificationSummary> Result)
	{
		if (UFlockGetNotificationSummaryAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockGetNotificationSummaryAction::Complete(const TFlockResult<FFlockNotificationSummary>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockNotificationSummary(), Result.Error);
	}
	SetReadyToDestroy();
}

// Mark Read

UFlockMarkNotificationReadAction* UFlockMarkNotificationReadAction::MarkRead(UObject* WorldContextObject,
	const FString& NotificationId)
{
	UFlockMarkNotificationReadAction* Action = NewObject<UFlockMarkNotificationReadAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->NotificationId = NotificationId;
	return Action;
}

void UFlockMarkNotificationReadAction::Activate()
{
	FFlockError Error;
	FFlockNotificationProvider* Provider = ResolveNotifications(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockNotification>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockMarkNotificationReadAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->MarkRead(NotificationId, [WeakThis](TFlockResult<FFlockNotification> Result)
	{
		if (UFlockMarkNotificationReadAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockMarkNotificationReadAction::Complete(const TFlockResult<FFlockNotification>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockNotification(), Result.Error);
	}
	SetReadyToDestroy();
}

// Mark All Read

UFlockMarkAllNotificationsReadAction* UFlockMarkAllNotificationsReadAction::MarkAllRead(UObject* WorldContextObject)
{
	UFlockMarkAllNotificationsReadAction* Action = NewObject<UFlockMarkAllNotificationsReadAction>();
	Action->WorldContextObject = WorldContextObject;
	return Action;
}

void UFlockMarkAllNotificationsReadAction::Activate()
{
	FFlockError Error;
	FFlockNotificationProvider* Provider = ResolveNotifications(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockMarkAllReadResult>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockMarkAllNotificationsReadAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->MarkAllRead([WeakThis](TFlockResult<FFlockMarkAllReadResult> Result)
	{
		if (UFlockMarkAllNotificationsReadAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockMarkAllNotificationsReadAction::Complete(const TFlockResult<FFlockMarkAllReadResult>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockMarkAllReadResult(), Result.Error);
	}
	SetReadyToDestroy();
}

// Get Templates

UFlockGetNotificationTemplatesAction* UFlockGetNotificationTemplatesAction::GetTemplates(UObject* WorldContextObject)
{
	UFlockGetNotificationTemplatesAction* Action = NewObject<UFlockGetNotificationTemplatesAction>();
	Action->WorldContextObject = WorldContextObject;
	return Action;
}

void UFlockGetNotificationTemplatesAction::Activate()
{
	FFlockError Error;
	FFlockNotificationProvider* Provider = ResolveNotifications(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<TArray<FFlockNotificationTemplate>>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetNotificationTemplatesAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->GetTemplates([WeakThis](TFlockResult<TArray<FFlockNotificationTemplate>> Result)
	{
		if (UFlockGetNotificationTemplatesAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockGetNotificationTemplatesAction::Complete(const TFlockResult<TArray<FFlockNotificationTemplate>>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(TArray<FFlockNotificationTemplate>(), Result.Error);
	}
	SetReadyToDestroy();
}

// Get Template By Name

UFlockGetNotificationTemplateByNameAction* UFlockGetNotificationTemplateByNameAction::GetByName(
	UObject* WorldContextObject, const FString& TemplateName, const FString& Locale)
{
	UFlockGetNotificationTemplateByNameAction* Action = NewObject<UFlockGetNotificationTemplateByNameAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->TemplateName = TemplateName;
	Action->Locale = Locale;
	return Action;
}

void UFlockGetNotificationTemplateByNameAction::Activate()
{
	FFlockError Error;
	FFlockNotificationProvider* Provider = ResolveNotifications(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockNotificationTemplate>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetNotificationTemplateByNameAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->GetTemplateByName(TemplateName, Locale, [WeakThis](TFlockResult<FFlockNotificationTemplate> Result)
	{
		if (UFlockGetNotificationTemplateByNameAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockGetNotificationTemplateByNameAction::Complete(const TFlockResult<FFlockNotificationTemplate>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockNotificationTemplate(), Result.Error);
	}
	SetReadyToDestroy();
}

// Schedule

UFlockScheduleNotificationAction* UFlockScheduleNotificationAction::Schedule(UObject* WorldContextObject,
	const FString& TemplateName, FDateTime DeliverAtUtc, FFlockCommandData Variables,
	const TArray<EFlockNotificationChannel>& Channels)
{
	UFlockScheduleNotificationAction* Action = NewObject<UFlockScheduleNotificationAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->TemplateName = TemplateName;
	Action->DeliverAtUtc = DeliverAtUtc;
	Action->Variables = MoveTemp(Variables);
	Action->Channels = Channels;
	return Action;
}

void UFlockScheduleNotificationAction::Activate()
{
	FFlockError Error;
	FFlockNotificationProvider* Provider = ResolveNotifications(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockScheduledNotification>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockScheduleNotificationAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->ScheduleByTemplateName(TemplateName, DeliverAtUtc, Variables, Channels,
		[WeakThis](TFlockResult<FFlockScheduledNotification> Result)
		{
			if (UFlockScheduleNotificationAction* Self = WeakThis.Get())
			{
				Self->Complete(Result);
			}
		});
}

void UFlockScheduleNotificationAction::Complete(const TFlockResult<FFlockScheduledNotification>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockScheduledNotification(), Result.Error);
	}
	SetReadyToDestroy();
}

// Cancel Scheduled

UFlockCancelScheduledNotificationAction* UFlockCancelScheduledNotificationAction::Cancel(UObject* WorldContextObject,
	const FString& ScheduledId)
{
	UFlockCancelScheduledNotificationAction* Action = NewObject<UFlockCancelScheduledNotificationAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->ScheduledId = ScheduledId;
	return Action;
}

void UFlockCancelScheduledNotificationAction::Activate()
{
	FFlockError Error;
	FFlockNotificationProvider* Provider = ResolveNotifications(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockScheduledNotification>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockCancelScheduledNotificationAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->CancelScheduled(ScheduledId, [WeakThis](TFlockResult<FFlockScheduledNotification> Result)
	{
		if (UFlockCancelScheduledNotificationAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockCancelScheduledNotificationAction::Complete(const TFlockResult<FFlockScheduledNotification>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockScheduledNotification(), Result.Error);
	}
	SetReadyToDestroy();
}

// Register Device Token

UFlockRegisterDeviceTokenAction* UFlockRegisterDeviceTokenAction::Register(UObject* WorldContextObject, const FString& Token)
{
	UFlockRegisterDeviceTokenAction* Action = NewObject<UFlockRegisterDeviceTokenAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Token = Token;
	return Action;
}

void UFlockRegisterDeviceTokenAction::Activate()
{
	FFlockError Error;
	FFlockNotificationProvider* Provider = ResolveNotifications(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockDeviceToken>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockRegisterDeviceTokenAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->RegisterDeviceToken(Token, [WeakThis](TFlockResult<FFlockDeviceToken> Result)
	{
		if (UFlockRegisterDeviceTokenAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockRegisterDeviceTokenAction::Complete(const TFlockResult<FFlockDeviceToken>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockDeviceToken(), Result.Error);
	}
	SetReadyToDestroy();
}

// Register Device Token For Platform

UFlockRegisterDeviceTokenForPlatformAction* UFlockRegisterDeviceTokenForPlatformAction::RegisterForPlatform(
	UObject* WorldContextObject, EFlockDevicePlatform Platform, const FString& Token)
{
	UFlockRegisterDeviceTokenForPlatformAction* Action = NewObject<UFlockRegisterDeviceTokenForPlatformAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Platform = Platform;
	Action->Token = Token;
	return Action;
}

void UFlockRegisterDeviceTokenForPlatformAction::Activate()
{
	FFlockError Error;
	FFlockNotificationProvider* Provider = ResolveNotifications(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockDeviceToken>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockRegisterDeviceTokenForPlatformAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->RegisterDeviceToken(Platform, Token, [WeakThis](TFlockResult<FFlockDeviceToken> Result)
	{
		if (UFlockRegisterDeviceTokenForPlatformAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockRegisterDeviceTokenForPlatformAction::Complete(const TFlockResult<FFlockDeviceToken>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockDeviceToken(), Result.Error);
	}
	SetReadyToDestroy();
}

// Unregister Device Token

UFlockUnregisterDeviceTokenAction* UFlockUnregisterDeviceTokenAction::Unregister(UObject* WorldContextObject, const FString& Token)
{
	UFlockUnregisterDeviceTokenAction* Action = NewObject<UFlockUnregisterDeviceTokenAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Token = Token;
	return Action;
}

void UFlockUnregisterDeviceTokenAction::Activate()
{
	FFlockError Error;
	FFlockNotificationProvider* Provider = ResolveNotifications(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockUnregisterDeviceTokenResult>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockUnregisterDeviceTokenAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->UnregisterDeviceToken(Token, [WeakThis](TFlockResult<FFlockUnregisterDeviceTokenResult> Result)
	{
		if (UFlockUnregisterDeviceTokenAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockUnregisterDeviceTokenAction::Complete(const TFlockResult<FFlockUnregisterDeviceTokenResult>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockUnregisterDeviceTokenResult(), Result.Error);
	}
	SetReadyToDestroy();
}

