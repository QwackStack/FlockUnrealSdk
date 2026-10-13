// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Providers/FlockMultiplayerProvider.h"
#include "Http/FlockEndpoints.h"
#include "Multiplayer/FlockMultiplayerSessionRequests.h"
#include "Multiplayer/FlockMultiplayerSessions.h"
#include "Multiplayer/FlockParties.h"
#include "Multiplayer/FlockPartyRequests.h"

FFlockMultiplayerProvider::FFlockMultiplayerProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
	const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession, const FString& InVersionedApiUrl,
	double InPartyRefreshSeconds)
	: FFlockProviderBase(InClient, InPolicy, InLogger)
	, Session(InSession)
	, VersionedApiUrl(InVersionedApiUrl)
	, RepeatingCalls(MakeShared<FFlockRepeatingCalls>([InSession]() { return InSession->GetSignInNumber(); }, InLogger))
	, Parties(MakeShared<FFlockParties>(
		MakeShared<FFlockPartyRequests>(InClient, InPolicy, InLogger, InSession, InVersionedApiUrl),
		RepeatingCalls, InSession, InLogger, InPartyRefreshSeconds))
	, Sessions(MakeShared<FFlockMultiplayerSessions>(
		MakeShared<FFlockMultiplayerSessionRequests>(InClient, InPolicy, InLogger, InSession, InVersionedApiUrl),
		RepeatingCalls, InSession, InLogger))
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
	Sessions->StopForShutdown();
	RepeatingCalls->StopAll();
	// A read on its way is answered into a provider about to go, so whoever waits on it is told now.
	if (QueueNames.IsValid())
	{
		QueueNames->GiveUpEveryWait();
	}
}

bool FFlockMultiplayerProvider::Tick(float DeltaSeconds)
{
	Parties->EndIfSignInEnded();
	Sessions->EndIfSignInEnded();
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

bool FFlockMultiplayerProvider::LeaveSessionForQuit()
{
	return Sessions->LeaveForQuit();
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
	if (!QueueNames.IsValid())
	{
		const TWeakPtr<FFlockMultiplayerProvider> WeakSelf = AsShared();
		QueueNames = MakeShared<FFlockMatchmakingQueueNames>(
			[WeakSelf](TFunction<void(TFlockResult<TArray<FFlockMatchmakingQueue>>)> OnRead) -> FFlockRequestHandle
			{
				if (const TSharedPtr<FFlockMultiplayerProvider> Self = WeakSelf.Pin())
				{
					return Self->ReadQueues(MoveTemp(OnRead));
				}
				OnRead(TFlockResult<TArray<FFlockMatchmakingQueue>>::Fail(FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Request cancelled"))));
				return FFlockRequestHandle();
			},
			Logger);
	}
	return QueueNames->FindId(QueueName, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockMultiplayerProvider::ReadQueues(TFunction<void(TFlockResult<TArray<FFlockMatchmakingQueue>>)> OnComplete)
{
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = FString::Printf(TEXT("%s/%s?limit=%d"), *VersionedApiUrl, FlockEndpoints::MatchmakingQueues,
		FFlockMatchmakingQueueNames::MostQueuesListed);
	// The API key alone: the list is the game's, so no player's token goes with it and a 401 is never a lapsed sign-in.
	const TMap<FString, FString> Headers = Session->GetBaseHeaders();
	return Execute<TArray<FFlockMatchmakingQueue>>(
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<TArray<FFlockMatchmakingQueue>>)> OnResult)
		{
			return ClientRef->GetList<FFlockMatchmakingQueue>(Url, Headers, MoveTemp(OnResult));
		},
		MoveTemp(OnComplete), TEXT("Read matchmaking queues"), /*bIdempotent*/ true, /*MaxRetriesOverride*/ -1,
		/*bAllowAuthRetry*/ false);
}
