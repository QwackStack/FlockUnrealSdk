// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/FlockJoinChecks.h"
#include "Flock.h"
#include "FlockSubsystem.h"
#include "Multiplayer/FlockJoinChecksOnHost.h"
#include "Providers/FlockMultiplayerProvider.h"

void FlockJoinChecks::CheckJoiningPlayer(AGameModeBase* GameMode, const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId,
	const AGameModeBase::FOnPreLoginCompleteDelegate& OnComplete, FFlockApproveJoiningPlayer ApproveJoiningPlayer)
{
	FFlockJoiningRequest Request;
	Request.Options = Options;
	Request.Address = Address;
	Request.UniqueId = UniqueId;
	const TWeakObjectPtr<AGameModeBase> WeakGameMode = GameMode;
	Request.RunEngineChecks = [WeakGameMode, Options, Address, UniqueId]()
	{
		FString Refusal;
		if (AGameModeBase* Asking = WeakGameMode.Get())
		{
			// The engine's own checks (a full game among them), and its PreLogin event, as its own PreLoginAsync runs them.
			Asking->PreLogin(Options, Address, UniqueId, Refusal);
		}
		return Refusal;
	};
	Request.Answer = [OnComplete](const FString& Refusal) { OnComplete.ExecuteIfBound(Refusal); };
	Request.IsAskerAlive = [WeakGameMode]() { return WeakGameMode.IsValid(); };
	Request.ApproveJoiningPlayer = MoveTemp(ApproveJoiningPlayer);

	if (GameMode == nullptr)
	{
		UE_LOG(LogFlock, Error, TEXT("[Flock SDK] FlockJoinChecks::CheckJoiningPlayer was given no game mode, so the joining player was turned away. Pass the game mode whose PreLoginAsync is running (this)."));
		Request.Answer(FlockHostRefusals::WordsFor(FlockTravelToSessionRefusedReasons::JoinCheckMissing));
		return;
	}
	const UFlockSubsystem* Sdk = UFlockSubsystem::Get(GameMode);
	FFlockMultiplayerProvider* Multiplayer = Sdk != nullptr ? Sdk->GetMultiplayerProvider() : nullptr;
	if (Multiplayer == nullptr)
	{
		// Flock is not running in this game, so it hosts nothing through Travel To Session: the engine's own PreLoginAsync.
		Request.Answer(Request.RunEngineChecks());
		return;
	}
	Multiplayer->GetJoinChecks().Check(MoveTemp(Request));
}
