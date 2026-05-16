#include "Qwack_ue_Sdk/Blueprint/FlockAuthAsync.h"

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

	FFlockPlayerAuthResponse MakeAuthUnavailable()
	{
		FFlockPlayerAuthResponse R;
		R.Meta.bSuccess = false;
		R.Meta.ErrorMessage = TEXT("Flock subsystem unavailable (no active game instance)");
		return R;
	}
}

// ===================================================================== Register

UFlockRegisterPlayerAsync* UFlockRegisterPlayerAsync::FlockRegisterPlayer(const UObject* WorldContextObject, const FFlockPlayerRegisterRequest& Request)
{
	UFlockRegisterPlayerAsync* Node = NewObject<UFlockRegisterPlayerAsync>();
	Node->WorldContext = WorldContextObject;
	Node->Req = Request;
	Node->RegisterWithGameInstance(GameInstanceFromContext(WorldContextObject));
	return Node;
}

void UFlockRegisterPlayerAsync::Activate()
{
	UQwackFlockSubsystem* S = UFlockBlueprintLibrary::GetFlockSubsystem(WorldContext.Get());
	if (!S)
	{
		OnFailed.Broadcast(MakeAuthUnavailable());
		SetReadyToDestroy();
		return;
	}
	UQwackFlockSubsystem::FFlockOnAuthResponse Cb;
	Cb.BindDynamic(this, &UFlockRegisterPlayerAsync::HandleResponse);
	S->RegisterPlayer(Req, Cb);
}

void UFlockRegisterPlayerAsync::HandleResponse(const FFlockPlayerAuthResponse& Response)
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

// ===================================================================== Login

UFlockLoginPlayerAsync* UFlockLoginPlayerAsync::FlockLoginPlayer(const UObject* WorldContextObject, const FFlockPlayerLoginRequest& Request)
{
	UFlockLoginPlayerAsync* Node = NewObject<UFlockLoginPlayerAsync>();
	Node->WorldContext = WorldContextObject;
	Node->Req = Request;
	Node->RegisterWithGameInstance(GameInstanceFromContext(WorldContextObject));
	return Node;
}

void UFlockLoginPlayerAsync::Activate()
{
	UQwackFlockSubsystem* S = UFlockBlueprintLibrary::GetFlockSubsystem(WorldContext.Get());
	if (!S)
	{
		OnFailed.Broadcast(MakeAuthUnavailable());
		SetReadyToDestroy();
		return;
	}
	UQwackFlockSubsystem::FFlockOnAuthResponse Cb;
	Cb.BindDynamic(this, &UFlockLoginPlayerAsync::HandleResponse);
	S->LoginPlayer(Req, Cb);
}

void UFlockLoginPlayerAsync::HandleResponse(const FFlockPlayerAuthResponse& Response)
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

// ===================================================================== Refresh

UFlockRefreshTokenAsync* UFlockRefreshTokenAsync::FlockRefreshToken(const UObject* WorldContextObject, const FFlockPlayerRefreshRequest& Request)
{
	UFlockRefreshTokenAsync* Node = NewObject<UFlockRefreshTokenAsync>();
	Node->WorldContext = WorldContextObject;
	Node->Req = Request;
	Node->RegisterWithGameInstance(GameInstanceFromContext(WorldContextObject));
	return Node;
}

void UFlockRefreshTokenAsync::Activate()
{
	UQwackFlockSubsystem* S = UFlockBlueprintLibrary::GetFlockSubsystem(WorldContext.Get());
	if (!S)
	{
		OnFailed.Broadcast(MakeAuthUnavailable());
		SetReadyToDestroy();
		return;
	}
	UQwackFlockSubsystem::FFlockOnAuthResponse Cb;
	Cb.BindDynamic(this, &UFlockRefreshTokenAsync::HandleResponse);
	S->RefreshToken(Req, Cb);
}

void UFlockRefreshTokenAsync::HandleResponse(const FFlockPlayerAuthResponse& Response)
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

// ===================================================================== Auth test

UFlockAuthTestAsync* UFlockAuthTestAsync::FlockAuthTest(const UObject* WorldContextObject)
{
	UFlockAuthTestAsync* Node = NewObject<UFlockAuthTestAsync>();
	Node->WorldContext = WorldContextObject;
	Node->RegisterWithGameInstance(GameInstanceFromContext(WorldContextObject));
	return Node;
}

void UFlockAuthTestAsync::Activate()
{
	UQwackFlockSubsystem* S = UFlockBlueprintLibrary::GetFlockSubsystem(WorldContext.Get());
	if (!S)
	{
		FFlockAuthTestResponse R;
		R.Meta.bSuccess = false;
		R.Meta.ErrorMessage = TEXT("Flock subsystem unavailable (no active game instance)");
		OnFailed.Broadcast(R);
		SetReadyToDestroy();
		return;
	}
	UQwackFlockSubsystem::FFlockOnAuthTestResponse Cb;
	Cb.BindDynamic(this, &UFlockAuthTestAsync::HandleResponse);
	S->AuthTest(Cb);
}

void UFlockAuthTestAsync::HandleResponse(const FFlockAuthTestResponse& Response)
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
