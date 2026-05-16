#include "Qwack_ue_Sdk/Blueprint/FlockLogAsync.h"

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

	FFlockGenericResponse MakeGenericUnavailable()
	{
		FFlockGenericResponse R;
		R.Meta.bSuccess = false;
		R.Meta.ErrorMessage = TEXT("Flock subsystem unavailable (no active game instance)");
		return R;
	}
}

// ===================================================================== Log debug

UFlockLogDebugAsync* UFlockLogDebugAsync::FlockLogDebug(const UObject* WorldContextObject, const FString& InMessage, const FString& InExtraDataJson)
{
	UFlockLogDebugAsync* Node = NewObject<UFlockLogDebugAsync>();
	Node->WorldContext = WorldContextObject;
	Node->Message = InMessage;
	Node->ExtraDataJson = InExtraDataJson;
	Node->RegisterWithGameInstance(GameInstanceFromContext(WorldContextObject));
	return Node;
}

void UFlockLogDebugAsync::Activate()
{
	UQwackFlockSubsystem* S = UFlockBlueprintLibrary::GetFlockSubsystem(WorldContext.Get());
	if (!S)
	{
		OnFailed.Broadcast(MakeGenericUnavailable());
		SetReadyToDestroy();
		return;
	}
	UQwackFlockSubsystem::FFlockOnGenericResponse Cb;
	Cb.BindDynamic(this, &UFlockLogDebugAsync::HandleResponse);
	S->LogDebug(Message, ExtraDataJson, Cb);
}

void UFlockLogDebugAsync::HandleResponse(const FFlockGenericResponse& Response)
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

// ===================================================================== Log error

UFlockLogErrorAsync* UFlockLogErrorAsync::FlockLogError(const UObject* WorldContextObject, const FString& InMessage, const FString& InLogicalExpression, const FString& InExtraDataJson)
{
	UFlockLogErrorAsync* Node = NewObject<UFlockLogErrorAsync>();
	Node->WorldContext = WorldContextObject;
	Node->Message = InMessage;
	Node->LogicalExpression = InLogicalExpression;
	Node->ExtraDataJson = InExtraDataJson;
	Node->RegisterWithGameInstance(GameInstanceFromContext(WorldContextObject));
	return Node;
}

void UFlockLogErrorAsync::Activate()
{
	UQwackFlockSubsystem* S = UFlockBlueprintLibrary::GetFlockSubsystem(WorldContext.Get());
	if (!S)
	{
		OnFailed.Broadcast(MakeGenericUnavailable());
		SetReadyToDestroy();
		return;
	}
	UQwackFlockSubsystem::FFlockOnGenericResponse Cb;
	Cb.BindDynamic(this, &UFlockLogErrorAsync::HandleResponse);
	S->LogError(Message, LogicalExpression, ExtraDataJson, Cb);
}

void UFlockLogErrorAsync::HandleResponse(const FFlockGenericResponse& Response)
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

// ===================================================================== Log exception

UFlockLogExceptionAsync* UFlockLogExceptionAsync::FlockLogException(const UObject* WorldContextObject, const FString& InMessage, const FString& InErrorMessage, const FString& InErrorCode, const FString& InTraceback, const FString& InErrorDataJson, const FString& InExtraDataJson)
{
	UFlockLogExceptionAsync* Node = NewObject<UFlockLogExceptionAsync>();
	Node->WorldContext = WorldContextObject;
	Node->Message = InMessage;
	Node->ErrorMessage = InErrorMessage;
	Node->ErrorCode = InErrorCode;
	Node->Traceback = InTraceback;
	Node->ErrorDataJson = InErrorDataJson;
	Node->ExtraDataJson = InExtraDataJson;
	Node->RegisterWithGameInstance(GameInstanceFromContext(WorldContextObject));
	return Node;
}

void UFlockLogExceptionAsync::Activate()
{
	UQwackFlockSubsystem* S = UFlockBlueprintLibrary::GetFlockSubsystem(WorldContext.Get());
	if (!S)
	{
		OnFailed.Broadcast(MakeGenericUnavailable());
		SetReadyToDestroy();
		return;
	}
	UQwackFlockSubsystem::FFlockOnGenericResponse Cb;
	Cb.BindDynamic(this, &UFlockLogExceptionAsync::HandleResponse);
	S->LogException(Message, ErrorMessage, ErrorCode, Traceback, ErrorDataJson, ExtraDataJson, Cb);
}

void UFlockLogExceptionAsync::HandleResponse(const FFlockGenericResponse& Response)
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

// ===================================================================== Log event (struct)

UFlockLogEventAsync* UFlockLogEventAsync::FlockLogEvent(const UObject* WorldContextObject, const FFlockLogEventRequest& Request)
{
	UFlockLogEventAsync* Node = NewObject<UFlockLogEventAsync>();
	Node->WorldContext = WorldContextObject;
	Node->Req = Request;
	Node->RegisterWithGameInstance(GameInstanceFromContext(WorldContextObject));
	return Node;
}

void UFlockLogEventAsync::Activate()
{
	UQwackFlockSubsystem* S = UFlockBlueprintLibrary::GetFlockSubsystem(WorldContext.Get());
	if (!S)
	{
		OnFailed.Broadcast(MakeGenericUnavailable());
		SetReadyToDestroy();
		return;
	}
	UQwackFlockSubsystem::FFlockOnGenericResponse Cb;
	Cb.BindDynamic(this, &UFlockLogEventAsync::HandleResponse);
	S->LogEvent(Req, Cb);
}

void UFlockLogEventAsync::HandleResponse(const FFlockGenericResponse& Response)
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

// ===================================================================== Log events (batch)

UFlockLogEventsAsync* UFlockLogEventsAsync::FlockLogEvents(const UObject* WorldContextObject, const FFlockLogEventsRequest& Request)
{
	UFlockLogEventsAsync* Node = NewObject<UFlockLogEventsAsync>();
	Node->WorldContext = WorldContextObject;
	Node->Req = Request;
	Node->RegisterWithGameInstance(GameInstanceFromContext(WorldContextObject));
	return Node;
}

void UFlockLogEventsAsync::Activate()
{
	UQwackFlockSubsystem* S = UFlockBlueprintLibrary::GetFlockSubsystem(WorldContext.Get());
	if (!S)
	{
		OnFailed.Broadcast(MakeGenericUnavailable());
		SetReadyToDestroy();
		return;
	}
	UQwackFlockSubsystem::FFlockOnGenericResponse Cb;
	Cb.BindDynamic(this, &UFlockLogEventsAsync::HandleResponse);
	S->LogEvents(Req, Cb);
}

void UFlockLogEventsAsync::HandleResponse(const FFlockGenericResponse& Response)
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
