// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockMultiplayerAsyncActions.h"
#include "Blueprint/FlockBlueprintCallOrigin.h"
#include "Http/FlockErrorHints.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "FlockSubsystem.h"
#include "Multiplayer/FlockMultiplayerSession.h"
#include "Multiplayer/FlockParty.h"
#include "Providers/FlockMultiplayerProvider.h"

namespace
{
	FFlockMultiplayerProvider* FindMultiplayer(const UObject* WorldContextObject)
	{
		UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
		return Sdk ? Sdk->GetMultiplayerProvider() : nullptr;
	}

	FFlockError FlockNotRunning()
	{
		return FFlockError::MakeWithFix(EFlockErrorType::Validation, TEXT("Flock is not initialized, so its multiplayer nodes cannot run"),
			FFlockErrorHints::InitializeFlockFirst());
	}

	FFlockError NoPartyGiven()
	{
		return FFlockError::MakeWithFix(EFlockErrorType::Validation, TEXT("No party was given"), FFlockErrorHints::PassAHeldParty());
	}

	FFlockError NoSessionGiven()
	{
		return FFlockError::MakeWithFix(EFlockErrorType::Validation, TEXT("No session was given"), FFlockErrorHints::PassAHeldSession());
	}

	TOptional<int32> SetUnlessZero(int32 Value)
	{
		return Value != 0 ? TOptional<int32>(Value) : TOptional<int32>();
	}

	/**
	 * The game instance a node registers with, so it keeps the node until a pin has fired: the graph that made it holds it only
	 * in a variable a second call of the same node replaces, and a search or a travel outlives that by minutes. Null when the
	 * world context leads to none, which registering ignores.
	 */
	UGameInstance* GameInstanceOf(const UObject* WorldContextObject)
	{
		if (WorldContextObject == nullptr)
		{
			return nullptr;
		}
		const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
		if (UGameInstance* Instance = World ? World->GetGameInstance() : nullptr)
		{
			return Instance;
		}
		if (const UGameInstance* Instance = Cast<UGameInstance>(WorldContextObject))
		{
			return const_cast<UGameInstance*>(Instance);
		}
		return WorldContextObject->GetTypedOuter<UGameInstance>();
	}
}

// ────────────────────────────── Parties ──────────────────────────────

UFlockPartyAction* UFlockPartyAction::CreateParty(UObject* WorldContextObject, int32 MaxSize, const FFlockCommandData& Settings)
{
	UFlockPartyAction* Action = NewObject<UFlockPartyAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Call = EFlockPartyCall::Create;
	Action->MaxSize = MaxSize;
	Action->Settings = Settings;
	return Action;
}

UFlockPartyAction* UFlockPartyAction::JoinParty(UObject* WorldContextObject, const FString& InviteCode)
{
	UFlockPartyAction* Action = NewObject<UFlockPartyAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Call = EFlockPartyCall::Join;
	Action->InviteCode = InviteCode;
	return Action;
}

UFlockPartyAction* UFlockPartyAction::LeaveParty(UObject* WorldContextObject, UFlockParty* Party)
{
	UFlockPartyAction* Action = NewObject<UFlockPartyAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Call = EFlockPartyCall::Leave;
	Action->Party = Party;
	return Action;
}

UFlockPartyAction* UFlockPartyAction::KickFromParty(UObject* WorldContextObject, UFlockParty* Party, const FString& PlayerId)
{
	UFlockPartyAction* Action = NewObject<UFlockPartyAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Call = EFlockPartyCall::Kick;
	Action->Party = Party;
	Action->OtherPlayerId = PlayerId;
	return Action;
}

UFlockPartyAction* UFlockPartyAction::MakePartyLeader(UObject* WorldContextObject, UFlockParty* Party, const FString& PlayerId)
{
	UFlockPartyAction* Action = NewObject<UFlockPartyAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Call = EFlockPartyCall::MakeLeader;
	Action->Party = Party;
	Action->OtherPlayerId = PlayerId;
	return Action;
}

UFlockPartyAction* UFlockPartyAction::UpdateParty(UObject* WorldContextObject, UFlockParty* Party, int32 NewMaxSize, bool bReplaceSettings,
	const FFlockCommandData& NewSettings)
{
	UFlockPartyAction* Action = NewObject<UFlockPartyAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Call = EFlockPartyCall::Update;
	Action->Party = Party;
	Action->MaxSize = NewMaxSize;
	Action->bReplaceSettings = bReplaceSettings;
	Action->Settings = NewSettings;
	return Action;
}

UFlockPartyAction* UFlockPartyAction::DisbandParty(UObject* WorldContextObject, UFlockParty* Party)
{
	UFlockPartyAction* Action = NewObject<UFlockPartyAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Call = EFlockPartyCall::Disband;
	Action->Party = Party;
	return Action;
}

void UFlockPartyAction::Activate()
{
	const bool bFindsFlock = Call == EFlockPartyCall::Create || Call == EFlockPartyCall::Join;
	Start(bFindsFlock ? FindMultiplayer(WorldContextObject.Get()) : nullptr);
}

void UFlockPartyAction::ActivateWithProviderForTesting(FFlockMultiplayerProvider* Provider)
{
	Start(Provider);
}

void UFlockPartyAction::Start(FFlockMultiplayerProvider* Provider)
{
	RegisterWithGameInstance(GameInstanceOf(WorldContextObject.Get()));
	const TWeakObjectPtr<UFlockPartyAction> WeakThis(this);
	FFlockOnParty Done = [WeakThis](TFlockResult<UFlockParty*> Result)
	{
		if (UFlockPartyAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	};
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));

	if (Call == EFlockPartyCall::Create || Call == EFlockPartyCall::Join)
	{
		if (Provider == nullptr)
		{
			Complete(TFlockResult<UFlockParty*>::Fail(FlockNotRunning()));
		}
		else if (Call == EFlockPartyCall::Create)
		{
			Provider->CreateParty(SetUnlessZero(MaxSize), Settings, MoveTemp(Done));
		}
		else
		{
			Provider->JoinParty(InviteCode, MoveTemp(Done));
		}
		return;
	}
	if (Party == nullptr)
	{
		Complete(TFlockResult<UFlockParty*>::Fail(NoPartyGiven()));
		return;
	}
	switch (Call)
	{
	case EFlockPartyCall::Leave:
		Party->Leave(MoveTemp(Done));
		break;
	case EFlockPartyCall::Kick:
		Party->Kick(OtherPlayerId, MoveTemp(Done));
		break;
	case EFlockPartyCall::MakeLeader:
		Party->MakeLeader(OtherPlayerId, MoveTemp(Done));
		break;
	case EFlockPartyCall::Update:
		Party->Update(SetUnlessZero(MaxSize), bReplaceSettings ? TOptional<FFlockCommandData>(Settings) : TOptional<FFlockCommandData>(), MoveTemp(Done));
		break;
	case EFlockPartyCall::Disband:
		Party->Disband(MoveTemp(Done));
		break;
	default:
		break;
	}
}

void UFlockPartyAction::Complete(const TFlockResult<UFlockParty*>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(nullptr, Result.Error);
	}
	SetReadyToDestroy();
}

UFlockGetMyPartyAction* UFlockGetMyPartyAction::GetMyParty(UObject* WorldContextObject)
{
	UFlockGetMyPartyAction* Action = NewObject<UFlockGetMyPartyAction>();
	Action->WorldContextObject = WorldContextObject;
	return Action;
}

void UFlockGetMyPartyAction::Activate()
{
	Start(FindMultiplayer(WorldContextObject.Get()));
}

void UFlockGetMyPartyAction::ActivateWithProviderForTesting(FFlockMultiplayerProvider* Provider)
{
	Start(Provider);
}

void UFlockGetMyPartyAction::Start(FFlockMultiplayerProvider* Provider)
{
	RegisterWithGameInstance(GameInstanceOf(WorldContextObject.Get()));
	if (Provider == nullptr)
	{
		Complete(TFlockResult<UFlockParty*>::Fail(FlockNotRunning()));
		return;
	}
	const TWeakObjectPtr<UFlockGetMyPartyAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
	Provider->GetMyParty([WeakThis](TFlockResult<UFlockParty*> Result)
	{
		if (UFlockGetMyPartyAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockGetMyPartyAction::Complete(const TFlockResult<UFlockParty*>& Result)
{
	if (!Result.bSuccess)
	{
		OnFailure.Broadcast(nullptr, Result.Error);
	}
	else if (Result.Value != nullptr)
	{
		OnParty.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnNoParty.Broadcast(nullptr, FFlockError());
	}
	SetReadyToDestroy();
}

// ────────────────────────────── Sessions ──────────────────────────────

UFlockSessionAction* UFlockSessionAction::HostSession(UObject* WorldContextObject, int32 MaxPlayers, const FFlockCommandData& Data, bool bSameVersionOnly)
{
	UFlockSessionAction* Action = NewObject<UFlockSessionAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Call = EFlockSessionCall::Host;
	Action->MaxPlayers = MaxPlayers;
	Action->Data = Data;
	Action->bSameVersionOnly = bSameVersionOnly;
	return Action;
}

UFlockSessionAction* UFlockSessionAction::JoinSession(UObject* WorldContextObject, const FString& JoinCode)
{
	UFlockSessionAction* Action = NewObject<UFlockSessionAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Call = EFlockSessionCall::Join;
	Action->JoinCode = JoinCode;
	return Action;
}

UFlockSessionAction* UFlockSessionAction::GetSession(UObject* WorldContextObject, const FString& SessionId)
{
	UFlockSessionAction* Action = NewObject<UFlockSessionAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Call = EFlockSessionCall::Get;
	Action->SessionId = SessionId;
	return Action;
}

UFlockSessionAction* UFlockSessionAction::LeaveSession(UObject* WorldContextObject, UFlockMultiplayerSession* Session)
{
	UFlockSessionAction* Action = NewObject<UFlockSessionAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Call = EFlockSessionCall::Leave;
	Action->Session = Session;
	return Action;
}

UFlockSessionAction* UFlockSessionAction::EndSession(UObject* WorldContextObject, UFlockMultiplayerSession* Session)
{
	UFlockSessionAction* Action = NewObject<UFlockSessionAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Call = EFlockSessionCall::End;
	Action->Session = Session;
	return Action;
}

UFlockSessionAction* UFlockSessionAction::MakeSessionHost(UObject* WorldContextObject, UFlockMultiplayerSession* Session, const FString& PlayerId)
{
	UFlockSessionAction* Action = NewObject<UFlockSessionAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Call = EFlockSessionCall::MakeHost;
	Action->Session = Session;
	Action->OtherPlayerId = PlayerId;
	return Action;
}

UFlockSessionAction* UFlockSessionAction::PublishConnection(UObject* WorldContextObject, UFlockMultiplayerSession* Session, const FString& Mode,
	const FFlockCommandData& Values)
{
	UFlockSessionAction* Action = NewObject<UFlockSessionAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Call = EFlockSessionCall::PublishConnection;
	Action->Session = Session;
	Action->Mode = Mode;
	Action->Values = Values;
	return Action;
}

UFlockSessionAction* UFlockSessionAction::PublishDirectConnection(UObject* WorldContextObject, UFlockMultiplayerSession* Session, int32 Port)
{
	UFlockSessionAction* Action = NewObject<UFlockSessionAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Call = EFlockSessionCall::PublishDirectConnection;
	Action->Session = Session;
	Action->Port = Port;
	return Action;
}

void UFlockSessionAction::Activate()
{
	const bool bFindsFlock = Call == EFlockSessionCall::Host || Call == EFlockSessionCall::Join || Call == EFlockSessionCall::Get;
	Start(bFindsFlock ? FindMultiplayer(WorldContextObject.Get()) : nullptr);
}

void UFlockSessionAction::ActivateWithProviderForTesting(FFlockMultiplayerProvider* Provider)
{
	Start(Provider);
}

void UFlockSessionAction::Start(FFlockMultiplayerProvider* Provider)
{
	RegisterWithGameInstance(GameInstanceOf(WorldContextObject.Get()));
	const TWeakObjectPtr<UFlockSessionAction> WeakThis(this);
	FFlockOnMultiplayerSession Done = [WeakThis](TFlockResult<UFlockMultiplayerSession*> Result)
	{
		if (UFlockSessionAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	};
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));

	if (Call == EFlockSessionCall::Host || Call == EFlockSessionCall::Join || Call == EFlockSessionCall::Get)
	{
		if (Provider == nullptr)
		{
			Complete(TFlockResult<UFlockMultiplayerSession*>::Fail(FlockNotRunning()));
		}
		else if (Call == EFlockSessionCall::Host)
		{
			Provider->HostSession(SetUnlessZero(MaxPlayers), Data, bSameVersionOnly, MoveTemp(Done));
		}
		else if (Call == EFlockSessionCall::Join)
		{
			Provider->JoinSession(JoinCode, MoveTemp(Done));
		}
		else
		{
			Provider->GetSession(SessionId, MoveTemp(Done));
		}
		return;
	}
	if (Session == nullptr)
	{
		Complete(TFlockResult<UFlockMultiplayerSession*>::Fail(NoSessionGiven()));
		return;
	}
	switch (Call)
	{
	case EFlockSessionCall::Leave:
		Session->Leave(MoveTemp(Done));
		break;
	case EFlockSessionCall::End:
		Session->End(MoveTemp(Done));
		break;
	case EFlockSessionCall::MakeHost:
		Session->MakeHost(OtherPlayerId, MoveTemp(Done));
		break;
	case EFlockSessionCall::PublishConnection:
		Session->PublishConnection(Mode, Values, MoveTemp(Done));
		break;
	case EFlockSessionCall::PublishDirectConnection:
		Session->PublishDirectConnection(Port, MoveTemp(Done));
		break;
	default:
		break;
	}
}

void UFlockSessionAction::Complete(const TFlockResult<UFlockMultiplayerSession*>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(nullptr, Result.Error);
	}
	SetReadyToDestroy();
}

UFlockGetMySessionAction* UFlockGetMySessionAction::GetMySession(UObject* WorldContextObject)
{
	UFlockGetMySessionAction* Action = NewObject<UFlockGetMySessionAction>();
	Action->WorldContextObject = WorldContextObject;
	return Action;
}

void UFlockGetMySessionAction::Activate()
{
	Start(FindMultiplayer(WorldContextObject.Get()));
}

void UFlockGetMySessionAction::ActivateWithProviderForTesting(FFlockMultiplayerProvider* Provider)
{
	Start(Provider);
}

void UFlockGetMySessionAction::Start(FFlockMultiplayerProvider* Provider)
{
	RegisterWithGameInstance(GameInstanceOf(WorldContextObject.Get()));
	if (Provider == nullptr)
	{
		Complete(TFlockResult<UFlockMultiplayerSession*>::Fail(FlockNotRunning()));
		return;
	}
	const TWeakObjectPtr<UFlockGetMySessionAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
	Provider->GetMySession([WeakThis](TFlockResult<UFlockMultiplayerSession*> Result)
	{
		if (UFlockGetMySessionAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockGetMySessionAction::Complete(const TFlockResult<UFlockMultiplayerSession*>& Result)
{
	if (!Result.bSuccess)
	{
		OnFailure.Broadcast(nullptr, Result.Error);
	}
	else if (Result.Value != nullptr)
	{
		OnSession.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnNoSession.Broadcast(nullptr, FFlockError());
	}
	SetReadyToDestroy();
}

// ────────────────────────────── Find Match ──────────────────────────────

UFlockFindMatchAction* UFlockFindMatchAction::FindMatch(UObject* WorldContextObject, const FString& QueueName, const FFlockMatchmakingOptions& Options)
{
	UFlockFindMatchAction* Action = NewObject<UFlockFindMatchAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->QueueName = QueueName;
	Action->Options = Options;
	return Action;
}

UFlockFindMatchAction* UFlockFindMatchAction::PartyFindMatch(UObject* WorldContextObject, UFlockParty* Party, const FString& QueueName,
	const FFlockMatchmakingOptions& Options)
{
	UFlockFindMatchAction* Action = NewObject<UFlockFindMatchAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Party = Party;
	Action->bAsParty = true;
	Action->QueueName = QueueName;
	Action->Options = Options;
	return Action;
}

void UFlockFindMatchAction::Activate()
{
	Start(bAsParty ? nullptr : FindMultiplayer(WorldContextObject.Get()));
}

void UFlockFindMatchAction::ActivateWithProviderForTesting(FFlockMultiplayerProvider* Provider)
{
	Start(Provider);
}

void UFlockFindMatchAction::Start(FFlockMultiplayerProvider* Provider)
{
	RegisterWithGameInstance(GameInstanceOf(WorldContextObject.Get()));
	if (bAsParty ? Party == nullptr : Provider == nullptr)
	{
		Complete(TFlockResult<FFlockMatchmakingResult>::Fail(bAsParty ? NoPartyGiven() : FlockNotRunning()));
		return;
	}
	const TWeakObjectPtr<UFlockFindMatchAction> WeakThis(this);
	FFlockOnMatchmakingResult Done = [WeakThis](TFlockResult<FFlockMatchmakingResult> Result)
	{
		if (UFlockFindMatchAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	};
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
	const FFlockRequestHandle Handle = bAsParty ? Party->FindMatch(QueueName, Options, MoveTemp(Done)) : Provider->FindMatch(QueueName, Options, MoveTemp(Done));
	if (!bDone)
	{
		Searching = Handle;
	}
}

void UFlockFindMatchAction::Cancel()
{
	if (bDone)
	{
		return;
	}
	// Done before the cancel, so the search's own answer to it (Cancelled) fires no pin: the game asked for it.
	bDone = true;
	FFlockRequestHandle Handle = MoveTemp(Searching);
	Searching = FFlockRequestHandle();
	Handle.Cancel();
	Super::Cancel();
}

void UFlockFindMatchAction::Complete(const TFlockResult<FFlockMatchmakingResult>& Result)
{
	if (bDone)
	{
		return;
	}
	bDone = true;
	Searching = FFlockRequestHandle();
	if (!Result.bSuccess)
	{
		OnFailure.Broadcast(FFlockMatchmakingResult(), Result.Error);
	}
	else if (Result.Value.IsMatched())
	{
		OnMatched.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnNoMatch.Broadcast(Result.Value, FFlockError());
	}
	SetReadyToDestroy();
}

// ────────────────────────────── Join tokens and addresses ──────────────────────────────

UFlockRequestJoinTokenAction* UFlockRequestJoinTokenAction::RequestJoinToken(UObject* WorldContextObject, UFlockMultiplayerSession* Session)
{
	UFlockRequestJoinTokenAction* Action = NewObject<UFlockRequestJoinTokenAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Session = Session;
	return Action;
}

void UFlockRequestJoinTokenAction::Activate()
{
	RegisterWithGameInstance(GameInstanceOf(WorldContextObject.Get()));
	if (Session == nullptr)
	{
		Complete(TFlockResult<FFlockMultiplayerSessionJoinToken>::Fail(NoSessionGiven()));
		return;
	}
	const TWeakObjectPtr<UFlockRequestJoinTokenAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
	Session->RequestJoinToken([WeakThis](TFlockResult<FFlockMultiplayerSessionJoinToken> Result)
	{
		if (UFlockRequestJoinTokenAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockRequestJoinTokenAction::Complete(const TFlockResult<FFlockMultiplayerSessionJoinToken>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockMultiplayerSessionJoinToken(), Result.Error);
	}
	SetReadyToDestroy();
}

UFlockVerifyJoinTokenAction* UFlockVerifyJoinTokenAction::VerifyJoinToken(UObject* WorldContextObject, UFlockMultiplayerSession* Session, const FString& Token)
{
	UFlockVerifyJoinTokenAction* Action = NewObject<UFlockVerifyJoinTokenAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Session = Session;
	Action->Token = Token;
	return Action;
}

void UFlockVerifyJoinTokenAction::Activate()
{
	RegisterWithGameInstance(GameInstanceOf(WorldContextObject.Get()));
	if (Session == nullptr)
	{
		Complete(TFlockResult<FString>::Fail(NoSessionGiven()));
		return;
	}
	const TWeakObjectPtr<UFlockVerifyJoinTokenAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
	Session->VerifyJoinToken(Token, [WeakThis](TFlockResult<FString> Result)
	{
		if (UFlockVerifyJoinTokenAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockVerifyJoinTokenAction::Complete(const TFlockResult<FString>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FString(), Result.Error);
	}
	SetReadyToDestroy();
}

UFlockFindDirectAddressAction* UFlockFindDirectAddressAction::FindDirectAddress(UObject* WorldContextObject, UFlockMultiplayerSession* Session)
{
	UFlockFindDirectAddressAction* Action = NewObject<UFlockFindDirectAddressAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Session = Session;
	return Action;
}

void UFlockFindDirectAddressAction::Activate()
{
	RegisterWithGameInstance(GameInstanceOf(WorldContextObject.Get()));
	if (Session == nullptr)
	{
		Complete(TFlockResult<FFlockDirectAddress>::Fail(NoSessionGiven()));
		return;
	}
	const TWeakObjectPtr<UFlockFindDirectAddressAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
	Session->FindDirectAddress([WeakThis](TFlockResult<FFlockDirectAddress> Result)
	{
		if (UFlockFindDirectAddressAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockFindDirectAddressAction::Complete(const TFlockResult<FFlockDirectAddress>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockDirectAddress(), Result.Error);
	}
	SetReadyToDestroy();
}

// ────────────────────────────── Wait For Connection ──────────────────────────────

UFlockWaitForConnectionAction* UFlockWaitForConnectionAction::WaitForConnection(UObject* WorldContextObject, UFlockMultiplayerSession* Session,
	float TimeoutSeconds)
{
	UFlockWaitForConnectionAction* Action = NewObject<UFlockWaitForConnectionAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Session = Session;
	Action->TimeoutSeconds = TimeoutSeconds;
	return Action;
}

void UFlockWaitForConnectionAction::Activate()
{
	RegisterWithGameInstance(GameInstanceOf(WorldContextObject.Get()));
	if (Session == nullptr)
	{
		Complete(TFlockResult<UFlockMultiplayerSession*>::Fail(NoSessionGiven()));
		return;
	}
	const TWeakObjectPtr<UFlockWaitForConnectionAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
	const FFlockRequestHandle Handle = Session->WaitForConnection(TimeoutSeconds, [WeakThis](TFlockResult<UFlockMultiplayerSession*> Result)
	{
		if (UFlockWaitForConnectionAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
	if (!bDone)
	{
		Waiting = Handle;
	}
}

void UFlockWaitForConnectionAction::Cancel()
{
	if (bDone)
	{
		return;
	}
	// Done before the cancel, so the wait's own answer to it (Cancelled) fires no pin: the game asked for it.
	bDone = true;
	FFlockRequestHandle Handle = MoveTemp(Waiting);
	Waiting = FFlockRequestHandle();
	Handle.Cancel();
	Super::Cancel();
}

void UFlockWaitForConnectionAction::Complete(const TFlockResult<UFlockMultiplayerSession*>& Result)
{
	if (bDone)
	{
		return;
	}
	bDone = true;
	Waiting = FFlockRequestHandle();
	if (!Result.bSuccess)
	{
		OnFailure.Broadcast(nullptr, Result.Error);
	}
	// Ended first: a session that has ended cannot be joined, whatever its last reading said.
	else if (Result.Value != nullptr && Result.Value->HasEnded())
	{
		OnSessionEnded.Broadcast(Result.Value, FFlockError());
	}
	else if (Result.Value != nullptr && Result.Value->HasConnection())
	{
		OnConnection.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnTimedOut.Broadcast(Result.Value, FFlockError());
	}
	SetReadyToDestroy();
}

// ────────────────────────────── Travel To Session ──────────────────────────────

UFlockTravelToSessionAction* UFlockTravelToSessionAction::TravelToSession(UObject* WorldContextObject, UFlockMultiplayerSession* Session,
	const FFlockTravelToSessionOptions& Options)
{
	UFlockTravelToSessionAction* Action = NewObject<UFlockTravelToSessionAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Session = Session;
	Action->Options = Options;
	return Action;
}

bool UFlockTravelToSessionAction::IsGoneAfterTheLevelChanges(const UObject* Caller)
{
	return Caller != nullptr && (Caller->IsA<UWorld>() || Caller->IsA<ULevel>() || Caller->GetTypedOuter<UWorld>() != nullptr);
}

void UFlockTravelToSessionAction::Activate()
{
	Start(FindMultiplayer(WorldContextObject.Get()));
}

void UFlockTravelToSessionAction::ActivateWithProviderForTesting(FFlockMultiplayerProvider* Provider)
{
	Start(Provider);
}

void UFlockTravelToSessionAction::Start(FFlockMultiplayerProvider* Provider)
{
	RegisterWithGameInstance(GameInstanceOf(WorldContextObject.Get()));
	if (Session == nullptr)
	{
		Complete(TFlockResult<FFlockTravelToSessionResult>::Fail(NoSessionGiven()));
		return;
	}
	if (Provider != nullptr && IsGoneAfterTheLevelChanges(WorldContextObject.Get()))
	{
		const FString Caller = WorldContextObject.Get()->GetName();
		Provider->GetLogger().LogWarning(FString::Printf(TEXT("Travel To Session was called from %s, which belongs to the current level: its "
			"pins fire once the level has changed, when %s is gone, so nothing there sees how the travel ended. Call Flock Travel To "
			"Session from the Game Instance Blueprint, where the engine's own NetworkError and TravelError events are."), *Caller, *Caller));
	}
	const TWeakObjectPtr<UFlockTravelToSessionAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
	const FFlockRequestHandle Handle = Session->TravelToSession(WorldContextObject.Get(), Options, [WeakThis](TFlockResult<FFlockTravelToSessionResult> Result)
	{
		if (UFlockTravelToSessionAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
	if (!bDone)
	{
		Traveling = Handle;
	}
}

void UFlockTravelToSessionAction::Cancel()
{
	if (bDone)
	{
		return;
	}
	// Done before the cancel, so the travel's own answer to it (Cancelled) fires no pin: the game asked for it.
	bDone = true;
	FFlockRequestHandle Handle = MoveTemp(Traveling);
	Traveling = FFlockRequestHandle();
	Handle.Cancel();
	Super::Cancel();
}

void UFlockTravelToSessionAction::Complete(const TFlockResult<FFlockTravelToSessionResult>& Result)
{
	if (bDone)
	{
		return;
	}
	bDone = true;
	Traveling = FFlockRequestHandle();
	if (!Result.bSuccess)
	{
		OnFailure.Broadcast(FFlockTravelToSessionResult(), Result.Error);
	}
	else if (Result.Value.Outcome.Equals(FlockTravelToSessionOutcomes::StartedAsHost, ESearchCase::CaseSensitive)
		|| Result.Value.Outcome.Equals(FlockTravelToSessionOutcomes::Connected, ESearchCase::CaseSensitive))
	{
		OnJoined.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnNotJoined.Broadcast(Result.Value, FFlockError());
	}
	SetReadyToDestroy();
}
