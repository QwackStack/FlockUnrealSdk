// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/FlockTravelToSession.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/PendingNetGame.h"
#include "Engine/World.h"
#include "IPAddress.h"
#include "Kismet/GameplayStatics.h"
#include "SocketTypes.h"

namespace
{
	/**
	 * The engine's side of a travel, read from the game instance's world context: the world it is in, a level it is opening or a
	 * host it is connecting to, and the world's net driver. Measured (UE 5.5, two game processes): a level opened with ?listen
	 * listens about a second later on every address; a second listen server on one machine takes the next free port by itself;
	 * a connection is open and its player let in about 1.7 s after the travel; a host nobody answers fails as a timeout.
	 */
	class FFlockEngineTravel : public IFlockGameTravel, public TSharedFromThis<FFlockEngineTravel>
	{
	public:
		explicit FFlockEngineTravel(UGameInstance* InGameInstance)
			: GameInstance(InGameInstance)
		{
		}

		virtual ~FFlockEngineTravel() override
		{
			if (GEngine != nullptr)
			{
				GEngine->OnNetworkFailure().Remove(NetworkFailureHandle);
				GEngine->OnTravelFailure().Remove(TravelFailureHandle);
			}
		}

		virtual FFlockGameNetState Read() const override
		{
			FFlockGameNetState State;
			const FWorldContext* Context = WorldContext();
			UWorld* World = Context != nullptr ? Context->World() : nullptr;
			if (World == nullptr)
			{
				return State;
			}
			State.bHasWorld = true;
			State.bInAnotherWorld = World != Marked.Get();
			// While a connection is being made the world it leaves already reads as a client, so the change comes first.
			State.bChangingLevel = !Context->TravelURL.IsEmpty() || Context->PendingNetGame != nullptr || !World->NextURL.IsEmpty();
			State.NetMode = World->GetNetMode();
			UNetDriver* Driver = World->GetNetDriver();
			if (State.NetMode == NM_ListenServer && Driver != nullptr)
			{
				// The port the net driver bound, which the engine moves on by itself when the one asked for is taken. Only an IP
				// address can be published: a net driver on Steam's sockets listens on a Steam id, which no address reaches.
				const TSharedPtr<const FInternetAddr> Local = Driver->GetLocalAddr();
				const bool bOverIp = Local.IsValid() && (Local->GetProtocolType() == FNetworkProtocolTypes::IPv4 || Local->GetProtocolType() == FNetworkProtocolTypes::IPv6);
				State.ListeningPort = bOverIp ? Local->GetPort() : 0;
			}
			if (State.NetMode == NM_Client && Driver != nullptr && Driver->ServerConnection != nullptr)
			{
				State.bLetInByHost = Driver->ServerConnection->GetConnectionState() == USOCK_Open && Driver->ServerConnection->PlayerController != nullptr;
			}
			return State;
		}

		virtual FString CurrentLevel() const override
		{
			const FWorldContext* Context = WorldContext();
			const UWorld* World = Context != nullptr ? Context->World() : nullptr;
			return World != nullptr ? UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()) : FString();
		}

		virtual void MarkWorld() override
		{
			const FWorldContext* Context = WorldContext();
			Marked = Context != nullptr ? Context->World() : nullptr;
		}

		virtual void OpenListening(const FString& Level) override
		{
			const FWorldContext* Context = WorldContext();
			if (UWorld* World = Context != nullptr ? Context->World() : nullptr)
			{
				UGameplayStatics::OpenLevel(World, FName(*Level), /*bAbsolute*/ true, TEXT("listen"));
			}
		}

		virtual void TravelTo(const FString& Address, int32 Port) override
		{
			const FWorldContext* Context = WorldContext();
			if (UWorld* World = Context != nullptr ? Context->World() : nullptr)
			{
				GEngine->SetClientTravel(World, *FString::Printf(TEXT("%s:%d"), *Address, Port), TRAVEL_Absolute);
			}
		}

		virtual void StopListening() override
		{
			const FWorldContext* Context = WorldContext();
			if (UWorld* World = Context != nullptr ? Context->World() : nullptr)
			{
				GEngine->ShutdownWorldNetDriver(World);
			}
		}

		virtual void StopConnecting() override
		{
			FWorldContext* Context = WorldContext();
			if (Context == nullptr)
			{
				return;
			}
			// A travel not yet taken up is dropped; one under way gives its connection up, and the game stays where it is.
			Context->TravelURL.Empty();
			if (Context->PendingNetGame != nullptr && Context->World() != nullptr)
			{
				GEngine->CancelPending(Context->World());
			}
		}

		virtual void WatchFailures(TFunction<void(const FString& Why)> OnFailure) override
		{
			if (GEngine == nullptr)
			{
				return;
			}
			const TWeakPtr<FFlockEngineTravel> WeakSelf = AsShared();
			// A connection being made fails with no world: it is this game's when its driver is the one the context is connecting with.
			NetworkFailureHandle = GEngine->OnNetworkFailure().AddLambda(
				[WeakSelf, OnFailure](UWorld* World, UNetDriver* Driver, ENetworkFailure::Type Type, const FString& Error)
				{
					const TSharedPtr<FFlockEngineTravel> Self = WeakSelf.Pin();
					const FWorldContext* Context = Self.IsValid() ? Self->WorldContext() : nullptr;
					if (Context == nullptr)
					{
						return;
					}
					const bool bConnecting = Driver != nullptr && Context->PendingNetGame != nullptr && Context->PendingNetGame->NetDriver == Driver;
					const bool bOwnWorld = World != nullptr && World == Context->World();
					if (bConnecting || bOwnWorld)
					{
						OnFailure(FString::Printf(TEXT("%s: %s"), ENetworkFailure::ToString(Type), *Error));
					}
				});
			// A travel failure with no world cannot be told apart from another game instance's (Play In Editor with several clients),
			// so only this game's world's count; a level that failed to open without one still shows in the world that stays.
			TravelFailureHandle = GEngine->OnTravelFailure().AddLambda(
				[WeakSelf, OnFailure](UWorld* World, ETravelFailure::Type Type, const FString& Error)
				{
					const TSharedPtr<FFlockEngineTravel> Self = WeakSelf.Pin();
					const FWorldContext* Context = Self.IsValid() ? Self->WorldContext() : nullptr;
					if (Context != nullptr && World != nullptr && World == Context->World())
					{
						OnFailure(FString::Printf(TEXT("%s: %s"), ETravelFailure::ToString(Type), *Error));
					}
				});
		}

	private:
		FWorldContext* WorldContext() const
		{
			UGameInstance* Instance = GameInstance.Get();
			return Instance != nullptr ? Instance->GetWorldContext() : nullptr;
		}

		TWeakObjectPtr<UGameInstance> GameInstance;
		TWeakObjectPtr<UWorld> Marked;
		FDelegateHandle NetworkFailureHandle;
		FDelegateHandle TravelFailureHandle;
	};
}

FFlockMakeGameTravel FlockEngineTravel()
{
	return [](UObject* WorldContextObject, FString& OutRefusal) -> TSharedPtr<IFlockGameTravel>
	{
		UWorld* World = GEngine != nullptr && WorldContextObject != nullptr
			? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
		UGameInstance* Instance = World != nullptr ? World->GetGameInstance() : nullptr;
		if (World == nullptr || !World->IsGameWorld() || Instance == nullptr || Instance->GetWorldContext() == nullptr)
		{
			OutRefusal = TEXT("Travel To Session needs the game's world: pass an object in it, such as an actor or the game instance.");
			return nullptr;
		}
		return MakeShared<FFlockEngineTravel>(Instance);
	};
}
