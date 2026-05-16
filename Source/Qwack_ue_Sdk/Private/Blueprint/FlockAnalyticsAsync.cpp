#include "Qwack_ue_Sdk/Blueprint/FlockAnalyticsAsync.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

#include "Qwack_ue_Sdk/Blueprint/FlockBlueprintLibrary.h"
#include "Qwack_ue_Sdk/GameAPI/QwackFlockSubsystem.h"

namespace
{
	UGameInstance* GameInstanceFromContext(const UObject* Ctx)
	{
		if (!Ctx || !GEngine)
		{
			return nullptr;
		}
		if (const UWorld* W = GEngine->GetWorldFromContextObject(Ctx, EGetWorldErrorMode::LogAndReturnNull))
		{
			return W->GetGameInstance();
		}
		return nullptr;
	}

	const TCHAR* UnavailableMsg = TEXT("Flock subsystem unavailable (no active game instance)");

	FFlockGenericResponse MakeGenericUnavailable()
	{
		FFlockGenericResponse R;
		R.Meta.bSuccess = false;
		R.Meta.ErrorMessage = UnavailableMsg;
		return R;
	}
}

// ===================================================================== Start session

UFlockStartSessionAsync* UFlockStartSessionAsync::FlockStartSession(const UObject* WorldContextObject, const FFlockSessionStartRequest& Request)
{
	UFlockStartSessionAsync* Node = NewObject<UFlockStartSessionAsync>();
	Node->WorldContext = WorldContextObject;
	Node->Req = Request;
	Node->RegisterWithGameInstance(GameInstanceFromContext(WorldContextObject));
	return Node;
}

void UFlockStartSessionAsync::Activate()
{
	UQwackFlockSubsystem* S = UFlockBlueprintLibrary::GetFlockSubsystem(WorldContext.Get());
	if (!S)
	{
		FFlockSessionStartResponse R;
		R.Meta.bSuccess = false;
		R.Meta.ErrorMessage = UnavailableMsg;
		OnFailed.Broadcast(R);
		SetReadyToDestroy();
		return;
	}
	UQwackFlockSubsystem::FFlockOnSessionStart Cb;
	Cb.BindDynamic(this, &UFlockStartSessionAsync::HandleResponse);
	S->StartSession(Req, Cb);
}

void UFlockStartSessionAsync::HandleResponse(const FFlockSessionStartResponse& Response)
{
	if (Response.Meta.bSuccess)
	{
		OnSuccess.Broadcast(Response);
	}
	else
	{
		OnFailed.Broadcast(Response);
	}
	SetReadyToDestroy();
}

// ===================================================================== End session

UFlockEndSessionAsync* UFlockEndSessionAsync::FlockEndSession(const UObject* WorldContextObject, const FString& InSessionId, const FFlockSessionEndRequest& Request)
{
	UFlockEndSessionAsync* Node = NewObject<UFlockEndSessionAsync>();
	Node->WorldContext = WorldContextObject;
	Node->SessionId = InSessionId;
	Node->Req = Request;
	Node->RegisterWithGameInstance(GameInstanceFromContext(WorldContextObject));
	return Node;
}

void UFlockEndSessionAsync::Activate()
{
	UQwackFlockSubsystem* S = UFlockBlueprintLibrary::GetFlockSubsystem(WorldContext.Get());
	if (!S)
	{
		OnFailed.Broadcast(MakeGenericUnavailable());
		SetReadyToDestroy();
		return;
	}
	UQwackFlockSubsystem::FFlockOnGenericResponse Cb;
	Cb.BindDynamic(this, &UFlockEndSessionAsync::HandleResponse);
	S->EndSession(SessionId, Req, Cb);
}

void UFlockEndSessionAsync::HandleResponse(const FFlockGenericResponse& Response)
{
	if (Response.Meta.bSuccess)
	{
		OnSuccess.Broadcast(Response);
	}
	else
	{
		OnFailed.Broadcast(Response);
	}
	SetReadyToDestroy();
}

// ===================================================================== Track event

UFlockTrackEventAsync* UFlockTrackEventAsync::FlockTrackEvent(const UObject* WorldContextObject, const FFlockAnalyticsEventRequest& Request)
{
	UFlockTrackEventAsync* Node = NewObject<UFlockTrackEventAsync>();
	Node->WorldContext = WorldContextObject;
	Node->Req = Request;
	Node->RegisterWithGameInstance(GameInstanceFromContext(WorldContextObject));
	return Node;
}

void UFlockTrackEventAsync::Activate()
{
	UQwackFlockSubsystem* S = UFlockBlueprintLibrary::GetFlockSubsystem(WorldContext.Get());
	if (!S)
	{
		OnFailed.Broadcast(MakeGenericUnavailable());
		SetReadyToDestroy();
		return;
	}
	UQwackFlockSubsystem::FFlockOnGenericResponse Cb;
	Cb.BindDynamic(this, &UFlockTrackEventAsync::HandleResponse);
	S->TrackEvent(Req, Cb);
}

void UFlockTrackEventAsync::HandleResponse(const FFlockGenericResponse& Response)
{
	if (Response.Meta.bSuccess)
	{
		OnSuccess.Broadcast(Response);
	}
	else
	{
		OnFailed.Broadcast(Response);
	}
	SetReadyToDestroy();
}

// ===================================================================== Track events (batch)

UFlockTrackEventsAsync* UFlockTrackEventsAsync::FlockTrackEvents(const UObject* WorldContextObject, const FFlockAnalyticsEventsRequest& Request)
{
	UFlockTrackEventsAsync* Node = NewObject<UFlockTrackEventsAsync>();
	Node->WorldContext = WorldContextObject;
	Node->Req = Request;
	Node->RegisterWithGameInstance(GameInstanceFromContext(WorldContextObject));
	return Node;
}

void UFlockTrackEventsAsync::Activate()
{
	UQwackFlockSubsystem* S = UFlockBlueprintLibrary::GetFlockSubsystem(WorldContext.Get());
	if (!S)
	{
		OnFailed.Broadcast(MakeGenericUnavailable());
		SetReadyToDestroy();
		return;
	}
	UQwackFlockSubsystem::FFlockOnGenericResponse Cb;
	Cb.BindDynamic(this, &UFlockTrackEventsAsync::HandleResponse);
	S->TrackEvents(Req, Cb);
}

void UFlockTrackEventsAsync::HandleResponse(const FFlockGenericResponse& Response)
{
	if (Response.Meta.bSuccess)
	{
		OnSuccess.Broadcast(Response);
	}
	else
	{
		OnFailed.Broadcast(Response);
	}
	SetReadyToDestroy();
}

// ===================================================================== Record transaction

UFlockRecordTransactionAsync* UFlockRecordTransactionAsync::FlockRecordTransaction(const UObject* WorldContextObject, const FFlockTransactionRequest& Request)
{
	UFlockRecordTransactionAsync* Node = NewObject<UFlockRecordTransactionAsync>();
	Node->WorldContext = WorldContextObject;
	Node->Req = Request;
	Node->RegisterWithGameInstance(GameInstanceFromContext(WorldContextObject));
	return Node;
}

void UFlockRecordTransactionAsync::Activate()
{
	UQwackFlockSubsystem* S = UFlockBlueprintLibrary::GetFlockSubsystem(WorldContext.Get());
	if (!S)
	{
		OnFailed.Broadcast(MakeGenericUnavailable());
		SetReadyToDestroy();
		return;
	}
	UQwackFlockSubsystem::FFlockOnGenericResponse Cb;
	Cb.BindDynamic(this, &UFlockRecordTransactionAsync::HandleResponse);
	S->RecordTransaction(Req, Cb);
}

void UFlockRecordTransactionAsync::HandleResponse(const FFlockGenericResponse& Response)
{
	if (Response.Meta.bSuccess)
	{
		OnSuccess.Broadcast(Response);
	}
	else
	{
		OnFailed.Broadcast(Response);
	}
	SetReadyToDestroy();
}
