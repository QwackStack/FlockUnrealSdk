// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Providers/FlockMultiplayerProvider.h"
#include "Multiplayer/FlockMatchmaking.h"
#include "Multiplayer/FlockMatchmakingRequests.h"
#include "Multiplayer/FlockMultiplayerSessionRequests.h"
#include "Multiplayer/FlockMultiplayerSessions.h"
#include "Multiplayer/FlockParties.h"
#include "Multiplayer/FlockPartyRequests.h"

FFlockMultiplayerProvider::FFlockMultiplayerProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
	const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession, const FString& InVersionedApiUrl,
	double InPartyRefreshSeconds)
	: FFlockProviderBase(InClient, InPolicy, InLogger)
	, Session(InSession)
	, RepeatingCalls(MakeShared<FFlockRepeatingCalls>([InSession]() { return InSession->GetSignInNumber(); }, InLogger))
	, MatchmakingRequests(MakeShared<FFlockMatchmakingRequests>(InClient, InPolicy, InLogger, InSession, InVersionedApiUrl))
	, QueueNames(MakeShared<FFlockMatchmakingQueueNames>(
		[Requests = MatchmakingRequests](TFunction<void(TFlockResult<TArray<FFlockMatchmakingQueue>>)> OnRead)
		{
			return Requests->ReadQueues(MoveTemp(OnRead));
		},
		InLogger))
	, Sessions(MakeShared<FFlockMultiplayerSessions>(
		MakeShared<FFlockMultiplayerSessionRequests>(InClient, InPolicy, InLogger, InSession, InVersionedApiUrl),
		RepeatingCalls, InSession, InLogger))
	, Matchmaking(MakeShared<FFlockMatchmaking>(MatchmakingRequests, RepeatingCalls, Sessions, InSession, InLogger, QueueNames))
	, Parties(MakeShared<FFlockParties>(
		MakeShared<FFlockPartyRequests>(InClient, InPolicy, InLogger, InSession, InVersionedApiUrl),
		RepeatingCalls, Matchmaking, InSession, InLogger, InPartyRefreshSeconds))
{
	SetAuthSession(InSession);
}

FFlockMultiplayerProvider::~FFlockMultiplayerProvider()
{
	Shutdown();
}

void FFlockMultiplayerProvider::Start()
{
	if (TickerHandle.IsValid())
	{
		return;
	}
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &FFlockMultiplayerProvider::Tick));
}

void FFlockMultiplayerProvider::Shutdown()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	Parties->StopForShutdown();
	Matchmaking->StopForShutdown();
	Sessions->StopForShutdown();
	RepeatingCalls->StopAll();
	// A read on its way is answered into a provider about to go, so whoever waits on it is told now.
	QueueNames->GiveUpEveryWait();
}

bool FFlockMultiplayerProvider::Tick(float DeltaSeconds)
{
	Parties->EndIfSignInEnded();
	Matchmaking->EndIfSignInEnded();
	Sessions->EndIfSignInEnded();
	Sessions->MoveConnectingOn();
	RepeatingCalls->SendThoseDue();
	return true;
}

FFlockRequestHandle FFlockMultiplayerProvider::HostSession(TOptional<int32> MaxPlayers, const FFlockCommandData& Data, bool bSameVersionOnly,
	FFlockOnMultiplayerSession OnComplete)
{
	return Sessions->Host(MaxPlayers, Data, bSameVersionOnly, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockMultiplayerProvider::JoinSession(const FString& JoinCode, FFlockOnMultiplayerSession OnComplete)
{
	return Sessions->Join(JoinCode, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockMultiplayerProvider::GetSession(const FString& SessionId, FFlockOnMultiplayerSession OnComplete)
{
	return Sessions->Get(SessionId, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockMultiplayerProvider::GetMySession(FFlockOnMultiplayerSession OnComplete)
{
	return Sessions->GetMine(MoveTemp(OnComplete));
}

bool FFlockMultiplayerProvider::LeaveForQuit()
{
	RepeatingCalls->StopAll();
	Matchmaking->CancelForQuit();
	return Sessions->LeaveForQuit();
}

FFlockRequestHandle FFlockMultiplayerProvider::FindMatch(const FString& QueueName, const FFlockMatchmakingOptions& Options,
	FFlockOnMatchmakingResult OnComplete)
{
	return Matchmaking->FindAlone(QueueName, Options, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockMultiplayerProvider::CreateParty(TOptional<int32> MaxSize, const FFlockCommandData& Settings, FFlockOnParty OnComplete)
{
	return Parties->Create(MaxSize, Settings, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockMultiplayerProvider::JoinParty(const FString& InviteCode, FFlockOnParty OnComplete)
{
	return Parties->Join(InviteCode, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockMultiplayerProvider::GetMyParty(FFlockOnParty OnComplete)
{
	return Parties->GetMine(MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockMultiplayerProvider::FindQueueId(const FString& QueueName, TFunction<void(TFlockResult<FString>)> OnComplete)
{
	if (!RequireNotEmpty<FString>(QueueName, TEXT("Queue Name"), OnComplete))
	{
		return FFlockRequestHandle();
	}
	return QueueNames->FindId(QueueName, MoveTemp(OnComplete));
}
