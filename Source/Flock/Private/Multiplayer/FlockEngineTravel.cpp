// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/FlockTravelToSession.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/PendingNetGame.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "IPAddress.h"
#include "Kismet/GameplayStatics.h"
#include "Multiplayer/FlockJoinChecks.h"
#include "Multiplayer/FlockUdpSockets.h"
#include "SocketTypes.h"
#include "UObject/ObjectKey.h"

namespace
{
	/** "address:port" as a travel names a host: an IPv4 address inside IPv6 (a dual-stack socket's reading) written as IPv4. */
	FString HostAndPort(const FString& Host, int32 Port)
	{
		const FString Mapped = TEXT("::ffff:");
		const FString Address = Host.StartsWith(Mapped, ESearchCase::IgnoreCase) ? Host.Mid(Mapped.Len()) : Host;
		return FString::Printf(TEXT("%s:%d"), *Address, Port);
	}

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
			// A seamless travel sets none of those while it moves through its transition level.
			State.bChangingLevel = !Context->TravelURL.IsEmpty() || Context->PendingNetGame != nullptr || !World->NextURL.IsEmpty() || World->IsInSeamlessTravel();
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
				const TSharedPtr<const FInternetAddr> Remote = Driver->ServerConnection->GetRemoteAddr();
				if (Remote.IsValid())
				{
					State.Destination = HostAndPort(Remote->ToString(false), Remote->GetPort());
				}
			}
			// A connection being made: where it goes, and whether the host's engine answered its hello with a challenge, which comes
			// before the host's own check of the player (read in UPendingNetGame::NotifyControlMessage).
			if (const UPendingNetGame* Pending = Context->PendingNetGame)
			{
				State.bConnecting = true;
				State.Destination = HostAndPort(Pending->URL.Host, Pending->URL.Port);
				const UNetConnection* Connecting = Pending->NetDriver != nullptr ? Pending->NetDriver->ServerConnection : nullptr;
				State.bHostAnswered = Connecting != nullptr && !Connecting->Challenge.IsEmpty();
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

		virtual void TravelTo(const FString& Url) override
		{
			const FWorldContext* Context = WorldContext();
			if (UWorld* World = Context != nullptr ? Context->World() : nullptr)
			{
				GEngine->SetClientTravel(World, *Url, TRAVEL_Absolute);
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

		virtual void WatchFailures(TFunction<void(const FString& Kind, const FString& Words)> OnFailure) override
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
						OnFailure(ENetworkFailure::ToString(Type), Error);
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
						OnFailure(ETravelFailure::ToString(Type), Error);
					}
				});
		}

		virtual uint64 FindJoiningConnection(const FString& Options, const FString& Address) override
		{
			UNetDriver* Driver = ListeningDriver();
			if (Driver == nullptr)
			{
				return 0;
			}
			ForgetConnectionsGone();
			// The engine asks the game mode while it reads the player's login, so the newest connection with this request is the one asking.
			for (int32 Index = Driver->ClientConnections.Num() - 1; Index >= 0; --Index)
			{
				UNetConnection* Connection = Driver->ClientConnections[Index];
				if (Connection == nullptr || Connection->PlayerController != nullptr || Connection->GetConnectionState() == USOCK_Closed
					|| NumberOf(Connection) != 0 || !OptionsOf(Connection->RequestURL).Equals(Options, ESearchCase::CaseSensitive)
					|| !Connection->LowLevelGetRemoteAddress().Equals(Address, ESearchCase::CaseSensitive))
				{
					continue;
				}
				const uint64 Number = ++LastNumber;
				Connections.Add(Number, Connection);
				NumberByConnection.Add(FObjectKey(Connection), Number);
				return Number;
			}
			return 0;
		}

		virtual bool IsStillHere(uint64 Number) const override
		{
			const TWeakObjectPtr<UNetConnection>* Found = Connections.Find(Number);
			const UNetConnection* Connection = Found != nullptr ? Found->Get() : nullptr;
			const UNetDriver* Driver = ListeningDriver();
			return Connection != nullptr && Driver != nullptr && Connection->GetConnectionState() != USOCK_Closed && Driver->ClientConnections.Contains(Connection);
		}

		virtual uint64 ConnectionOf(const APlayerController* Controller) const override
		{
			return Controller != nullptr ? NumberOf(Controller->NetConnection) : 0;
		}

		virtual bool IsOwnPlayer(const APlayerController* Controller) const override
		{
			return Controller != nullptr && Controller->IsLocalController() && GameInstance.IsValid() && Controller->GetGameInstance() == GameInstance.Get();
		}

		virtual bool IsThisGame(const AGameModeBase* GameMode) const override
		{
			return GameMode != nullptr && GameInstance.IsValid() && GameMode->GetGameInstance() == GameInstance.Get();
		}

		virtual int32 LoopbackPortOf(uint64 Number) const override
		{
			const TWeakObjectPtr<UNetConnection>* Found = Connections.Find(Number);
			return LoopbackPortOfConnection(Found != nullptr ? Found->Get() : nullptr);
		}

		virtual bool IsAnyConnectionFromLoopbackPort(int32 Port) const override
		{
			UNetDriver* Driver = ListeningDriver();
			if (Driver == nullptr || Port <= 0)
			{
				return false;
			}
			for (UNetConnection* Connection : Driver->ClientConnections)
			{
				if (Connection != nullptr && Connection->GetConnectionState() != USOCK_Closed && LoopbackPortOfConnection(Connection) == Port)
				{
					return true;
				}
			}
			return false;
		}

		virtual double SecondsSinceHeardFrom(uint64 Number) const override
		{
			const TWeakObjectPtr<UNetConnection>* Found = Connections.Find(Number);
			const UNetConnection* Connection = Found != nullptr ? Found->Get() : nullptr;
			if (Connection == nullptr || Connection->GetConnectionState() == USOCK_Closed)
			{
				return TNumericLimits<double>::Max();
			}
			return FPlatformTime::Seconds() - Connection->LastReceiveRealtime;
		}

		virtual void CloseConnection(uint64 Number) override
		{
			const TWeakObjectPtr<UNetConnection>* Found = Connections.Find(Number);
			if (UNetConnection* Connection = Found != nullptr ? Found->Get() : nullptr)
			{
				Connection->Close();
			}
		}

		virtual bool PutJoinTokenOnTheConnectionBeingMade(const FString& Token) override
		{
			FWorldContext* Context = WorldContext();
			UPendingNetGame* Pending = Context != nullptr ? Context->PendingNetGame : nullptr;
			if (Pending == nullptr)
			{
				return false;
			}
			// The login goes out with the URL's options once the host's engine has sent its challenge (UPendingNetGame::NotifyControlMessage).
			const UNetConnection* Connecting = Pending->NetDriver != nullptr ? Pending->NetDriver->ServerConnection : nullptr;
			if (Connecting != nullptr && !Connecting->Challenge.IsEmpty())
			{
				return false;
			}
			// In place of the option of that key, whatever its letter case, as the host's check reads it.
			Pending->URL.AddOption(*FString::Printf(TEXT("%s=%s"), FlockJoinChecks::JoinTokenOption, *Token));
			return true;
		}

	private:
		/** The port of 127.0.0.1 a connection's packets come from; 0 for any other address. */
		static int32 LoopbackPortOfConnection(UNetConnection* Connection)
		{
			const TSharedPtr<const FInternetAddr> Remote = Connection != nullptr ? Connection->GetRemoteAddr() : nullptr;
			return Remote.IsValid() && FFlockUdpSockets::IsLoopback(*Remote) ? Remote->GetPort() : 0;
		}

		/** The options of a request URL as the engine hands them to PreLoginAsync: from its first '?'. */
		static FString OptionsOf(const FString& RequestUrl)
		{
			int32 Start = INDEX_NONE;
			return RequestUrl.FindChar(TEXT('?'), Start) ? RequestUrl.Mid(Start) : FString();
		}

		UNetDriver* ListeningDriver() const
		{
			const FWorldContext* Context = WorldContext();
			UWorld* World = Context != nullptr ? Context->World() : nullptr;
			return World != nullptr && World->GetNetMode() == NM_ListenServer ? World->GetNetDriver() : nullptr;
		}

		uint64 NumberOf(const UNetConnection* Connection) const
		{
			const uint64* Found = Connection != nullptr ? NumberByConnection.Find(FObjectKey(Connection)) : nullptr;
			return Found != nullptr ? *Found : 0;
		}

		void ForgetConnectionsGone()
		{
			for (auto It = Connections.CreateIterator(); It; ++It)
			{
				if (!It.Value().IsValid())
				{
					It.RemoveCurrent();
				}
			}
			// An object key is never reused, so a key of a connection gone matches nothing; it is only dropped.
			for (auto It = NumberByConnection.CreateIterator(); It; ++It)
			{
				if (!Connections.Contains(It.Value()))
				{
					It.RemoveCurrent();
				}
			}
		}

		FWorldContext* WorldContext() const
		{
			UGameInstance* Instance = GameInstance.Get();
			return Instance != nullptr ? Instance->GetWorldContext() : nullptr;
		}

		TWeakObjectPtr<UGameInstance> GameInstance;
		TWeakObjectPtr<UWorld> Marked;
		/** Each connection a join check asked about, by the number it was handed out with; numbers are never used twice. */
		TMap<uint64, TWeakObjectPtr<UNetConnection>> Connections;
		/** The same, the other way round. */
		TMap<FObjectKey, uint64> NumberByConnection;
		uint64 LastNumber = 0;
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
			OutRefusal = TEXT("Travel To Session needs the game's world, and the object given is not in one");
			return nullptr;
		}
		return MakeShared<FFlockEngineTravel>(Instance);
	};
}
