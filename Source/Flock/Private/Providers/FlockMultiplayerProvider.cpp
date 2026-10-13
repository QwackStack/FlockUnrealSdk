// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Providers/FlockMultiplayerProvider.h"
#include "Http/FlockEndpoints.h"

FFlockMultiplayerProvider::FFlockMultiplayerProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
	const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession, const FString& InVersionedApiUrl)
	: FFlockProviderBase(InClient, InPolicy, InLogger)
	, Session(InSession)
	, VersionedApiUrl(InVersionedApiUrl)
	, RepeatingCalls([InSession]() { return InSession->GetSignInNumber(); }, InLogger)
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
	RepeatingCalls.StopAll();
	// A read on its way is answered into a provider about to go, so whoever waits on it is told now.
	if (QueueNames.IsValid())
	{
		QueueNames->GiveUpEveryWait();
	}
}

bool FFlockMultiplayerProvider::Tick(float DeltaSeconds)
{
	RepeatingCalls.SendThoseDue();
	return true;
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
