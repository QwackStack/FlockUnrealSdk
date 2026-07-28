// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Providers/FlockCommandProvider.h"

#include "Http/FlockEndpoints.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Providers/FlockPlayerProvider.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

const TCHAR* const FFlockCommandProvider::SnapshotCategory = TEXT("command");
const TCHAR* const FFlockCommandProvider::PendingWritesKey = TEXT("pending_writes");

namespace
{
	FString SerializeObject(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	/** Command bodies all lead with the row they target, so every builder starts here. */
	TSharedRef<FJsonObject> MakeCommandBody(const FString& PlayerDataId)
	{
		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("player_data_id"), PlayerDataId);
		return Body;
	}
}

FFlockCommandProvider::FFlockCommandProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
	const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession,
	const FString& InVersionedApiUrl, const TSharedPtr<FFlockSnapshotStore>& InSnapshotStore,
	const FString& InGameVersionId)
	: FFlockProviderBase(InClient, InPolicy, InLogger)
	, Session(InSession)
	, VersionedApiUrl(InVersionedApiUrl)
{
	SetSnapshotStore(InSnapshotStore, InGameVersionId);
	SetAuthSession(InSession);
}

FFlockCommandProvider::~FFlockCommandProvider()
{
	Shutdown();
}

void FFlockCommandProvider::Initialize()
{
	if (Pump.IsRunning())
	{
		return;
	}

	bWasReachable = IsServerReachable();

	const TWeakPtr<FFlockCommandProvider> WeakSelf = AsShared();
	Pump.OnTick.AddLambda([WeakSelf](float Delta)
	{
		if (const TSharedPtr<FFlockCommandProvider> Self = WeakSelf.Pin())
		{
			Self->HandleTick(Delta);
		}
	});
	Pump.OnBackgroundChanged.AddLambda([WeakSelf](bool bBackgrounded)
	{
		if (const TSharedPtr<FFlockCommandProvider> Self = WeakSelf.Pin())
		{
			Self->HandleBackgroundChanged(bBackgrounded);
		}
	});
	Pump.Start();
}

void FFlockCommandProvider::Shutdown()
{
	// No AsShared() here — this also runs from the destructor, where pinning a weak self is invalid.
	Pump.Stop();
	Pump.OnTick.Clear();
	Pump.OnBackgroundChanged.Clear();
}

// ───────────────────────────────────  Commands  ────────────────────────────────

void FFlockCommandProvider::UpdatePlayerData(const FString& PlayerDataId, const FFlockCommandData& Data,
	TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete)
{
	if (!RequireNotEmpty(PlayerDataId, TEXT("Player Data ID"), OnComplete))
	{
		return;
	}

	// Flat {field: value} object — the backend 422s on the DataField array form.
	TSharedRef<FJsonObject> Body = MakeCommandBody(PlayerDataId);
	Body->SetObjectField(TEXT("data"), Data.ToJsonObject());

	SendQueueable(FlockEndpoints::CommandUpdatePlayerData, SerializeObject(Body), TEXT("Update player data"),
		PlayerDataId, Data, MoveTemp(OnComplete));
}

void FFlockCommandProvider::UpdatePlayerDataField(const FString& PlayerDataId, const FString& Key,
	const FFlockCommandValue& Value, TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete)
{
	if (!RequireNotEmpty(PlayerDataId, TEXT("Player Data ID"), OnComplete)
		|| !RequireNotEmpty(Key, TEXT("Key"), OnComplete))
	{
		return;
	}

	TSharedRef<FJsonObject> Body = MakeCommandBody(PlayerDataId);
	Body->SetStringField(TEXT("key"), Key);
	Body->SetField(TEXT("value"), Value.ToJsonValue());

	// The optimistic overlay is the same single field, expressed as a one-entry bag.
	const FFlockCommandData Optimistic = FFlockCommandData().Set(Key, Value);

	SendQueueable(FlockEndpoints::CommandUpdatePlayerDataKey, SerializeObject(Body), TEXT("Update player data field"),
		PlayerDataId, Optimistic, MoveTemp(OnComplete));
}

void FFlockCommandProvider::UnlockAchievement(const FString& AchievementName,
	TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete)
{
	if (!RequireNotEmpty(AchievementName, TEXT("Achievement Name"), OnComplete))
	{
		return;
	}

	const TSharedPtr<FFlockPlayerProvider> Players = PlayerProvider.Pin();
	if (!Players.IsValid())
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockPlayerData>::Fail(FFlockError::Make(EFlockErrorType::Validation,
				TEXT("Player provider unavailable; cannot resolve the achievements row."))));
		}
		return;
	}

	const FString Name = AchievementName;
	const TWeakPtr<FFlockCommandProvider> WeakSelf = AsShared();
	Players->GetMyDataByTag(TEXT("achievement"),
		[WeakSelf, Name, OnComplete](TFlockResult<FFlockPlayerData> RowResult)
		{
			const TSharedPtr<FFlockCommandProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (!RowResult.bSuccess)
			{
				if (OnComplete)
				{
					OnComplete(RowResult);
				}
				return;
			}
			if (RowResult.Value.Id.IsEmpty())
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<FFlockPlayerData>::Fail(FFlockError::Make(EFlockErrorType::Validation,
						TEXT("No achievements data found for the current player."))));
				}
				return;
			}

			TSharedRef<FJsonObject> Body = MakeCommandBody(RowResult.Value.Id);
			Body->SetStringField(TEXT("achievement_name"), Name);

			// The server decides what an unlock writes into the row, so there is nothing to overlay
			// optimistically — the queued entry replays and the authoritative row lands then.
			Self->SendQueueable(FlockEndpoints::CommandUnlockAchievement, SerializeObject(Body),
				TEXT("Unlock achievement"), RowResult.Value.Id, FFlockCommandData(), OnComplete);
		});
}

void FFlockCommandProvider::AddGameFunds(const FString& Currency, int32 Amount,
	TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete)
{
	if (!RequireNotEmpty(Currency, TEXT("Currency"), OnComplete))
	{
		return;
	}

	const TSharedPtr<FFlockPlayerProvider> Players = PlayerProvider.Pin();
	if (!Players.IsValid())
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockPlayerData>::Fail(FFlockError::Make(EFlockErrorType::Validation,
				TEXT("Player provider unavailable; cannot resolve the currency template."))));
		}
		return;
	}

	const FString CurrencyCopy = Currency;
	const TWeakPtr<FFlockCommandProvider> WeakSelf = AsShared();
	Players->GetTemplateByTag(TEXT("currency"),
		[WeakSelf, CurrencyCopy, Amount, OnComplete](TFlockResult<FFlockPlayerTemplateSchema> TemplateResult)
		{
			const TSharedPtr<FFlockCommandProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (!TemplateResult.bSuccess)
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<FFlockPlayerData>::Fail(TemplateResult.Error));
				}
				return;
			}
			Self->AddGameFunds(CurrencyCopy, Amount, TemplateResult.Value.Id, OnComplete);
		});
}

void FFlockCommandProvider::AddGameFunds(const FString& Currency, int32 Amount, const FString& CurrencyTemplateId,
	TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete)
{
	if (!RequireNotEmpty(Currency, TEXT("Currency"), OnComplete)
		|| !RequireNotEmpty(CurrencyTemplateId, TEXT("Currency Template ID"), OnComplete))
	{
		return;
	}

	const TSharedPtr<FFlockPlayerProvider> Players = PlayerProvider.Pin();
	if (!Players.IsValid())
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockPlayerData>::Fail(FFlockError::Make(EFlockErrorType::Validation,
				TEXT("Player provider unavailable; cannot resolve the currency wallet."))));
		}
		return;
	}

	const FString CurrencyCopy = Currency;
	const TWeakPtr<FFlockCommandProvider> WeakSelf = AsShared();
	Players->GetMyDataByTemplate(CurrencyTemplateId,
		[WeakSelf, CurrencyCopy, Amount, OnComplete](TFlockResult<FFlockPlayerData> WalletResult)
		{
			const TSharedPtr<FFlockCommandProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (!WalletResult.bSuccess)
			{
				if (OnComplete)
				{
					OnComplete(WalletResult);
				}
				return;
			}
			if (WalletResult.Value.Id.IsEmpty())
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<FFlockPlayerData>::Fail(FFlockError::Make(EFlockErrorType::Validation,
						TEXT("No currency wallet found for the current player."))));
				}
				return;
			}

			// Money is never queued: a grant that replays later is a grant the player may have already been
			// given, and the SDK has no way to tell. Fail loudly instead.
			if (!Self->IsServerReachable())
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<FFlockPlayerData>::Fail(FFlockError::Make(EFlockErrorType::Connection,
						TEXT("Add game funds requires a network connection; money grants are not queued offline."))));
				}
				return;
			}

			TSharedRef<FJsonObject> Body = MakeCommandBody(WalletResult.Value.Id);
			Body->SetStringField(TEXT("currency"), CurrencyCopy);
			Body->SetNumberField(TEXT("amount"), Amount);

			// bIdempotent=false: an ambiguous failure may mean the credit already landed, so the retry
			// handler must surface it rather than re-post and double-credit.
			Self->PostCommand(FlockEndpoints::CommandAddGameFunds, SerializeObject(Body), TEXT("Add game funds"),
				/*bIdempotent*/ false, OnComplete);
		});
}

// ────────────────────────────────  Offline queue  ──────────────────────────────

void FFlockCommandProvider::FlushPendingWrites(TFunction<void(TFlockResult<int32>)> OnComplete)
{
	// The guard lives here rather than only in the auto-flush trigger, so a manual flush and an automatic
	// one can't run the queue at once and post the same command twice. Game-thread only, so a plain bool
	// is enough.
	if (bFlushInFlight)
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<int32>::Ok(0));
		}
		return;
	}

	EnsureQueueLoaded();
	if (PendingWrites.Num() == 0 || !IsServerReachable())
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<int32>::Ok(0));
		}
		return;
	}

	bFlushInFlight = true;
	FlushNext(MakeShared<int32>(0), MoveTemp(OnComplete));
}

void FFlockCommandProvider::FlushNext(const TSharedRef<int32>& Delivered, TFunction<void(TFlockResult<int32>)> OnComplete)
{
	if (PendingWrites.Num() == 0)
	{
		FinishFlush(*Delivered, MoveTemp(OnComplete));
		return;
	}

	const FFlockPendingCommand Head = PendingWrites[0];
	const TWeakPtr<FFlockCommandProvider> WeakSelf = AsShared();

	PostCommand(Head.Path, Head.PayloadJson, Head.Context, /*bIdempotent*/ true,
		[WeakSelf, Head, Delivered, OnComplete](TFlockResult<FFlockPlayerData> Result)
		{
			const TSharedPtr<FFlockCommandProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}

			if (Result.bSuccess)
			{
				Self->PendingWrites.RemoveAt(0);
				Self->PersistQueue();
				*Delivered += 1;
				Self->FlushNext(Delivered, OnComplete);
				return;
			}

			if (!IsPermanentFailure(Result.Error))
			{
				// Transient or auth: keep it queued and stop here, so ordering survives to the next flush.
				Self->Logger->LogWarning(FString::Printf(
					TEXT("Pending-write flush halted at '%s', will retry next flush: %s"),
					*Head.Context, *Result.Error.Message));
				Self->FinishFlush(*Delivered, OnComplete);
				return;
			}

			// Authoritatively rejected — it will never succeed, so drop it rather than let it block the
			// queue, and drop the optimistic row it wrote since the server never accepted it.
			Self->Logger->LogError(FString::Printf(TEXT("Dropping rejected queued write '%s' (HTTP %d): %s"),
				*Head.Context, Result.Error.StatusCode, *Result.Error.Message));
			Self->PendingWrites.RemoveAt(0);
			Self->PersistQueue();
			Self->EvictOptimisticRow(Head.PlayerDataId);
			Self->FlushNext(Delivered, OnComplete);
		});
}

void FFlockCommandProvider::FinishFlush(int32 Delivered, TFunction<void(TFlockResult<int32>)> OnComplete)
{
	bFlushInFlight = false;
	if (OnComplete)
	{
		OnComplete(TFlockResult<int32>::Ok(Delivered));
	}
}

int32 FFlockCommandProvider::GetPendingWriteCount()
{
	EnsureQueueLoaded();
	return PendingWrites.Num();
}

void FFlockCommandProvider::ClearPendingWrites()
{
	EnsureQueueLoaded();
	PendingWrites.Reset();
	PersistQueue();
}

void FFlockCommandProvider::TriggerFlush()
{
	// Replays are player-scoped, so there is nothing safe to send while signed out.
	if (!Session->IsAuthenticated())
	{
		return;
	}
	const TSharedRef<IFlockLogger> Log = Logger;
	FlushPendingWrites([Log](TFlockResult<int32> Result)
	{
		if (Result.bSuccess && Result.Value > 0)
		{
			Log->LogInfo(FString::Printf(TEXT("Replayed %d queued command(s)."), Result.Value));
		}
	});
}

void FFlockCommandProvider::HandleTick(float DeltaSeconds)
{
	// Flush the instant connectivity returns, so queued writes don't wait for a focus change. Only ever
	// observable with a reachability probe set — the default probe always reports reachable, so this edge
	// never fires on its own.
	const bool bReachable = IsServerReachable();
	if (bReachable && !bWasReachable)
	{
		TriggerFlush();
	}
	bWasReachable = bReachable;
}

void FFlockCommandProvider::HandleBackgroundChanged(bool bBackgrounded)
{
	if (!bBackgrounded)
	{
		TriggerFlush();
	}
}

// ───────────────────────────────────  Internals  ───────────────────────────────

void FFlockCommandProvider::PostCommand(const FString& Path, const FString& PayloadJson, const FString& Context,
	bool bIdempotent, TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete)
{
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(Path);
	const TWeakPtr<FFlockCommandProvider> WeakSelf = AsShared();

	// Bare response: the command routes answer with the player-data row at the root, not under `result`.
	// Headers are fetched inside the operation so a post-refresh replay carries the new bearer.
	Execute<FFlockPlayerData>(
		[ClientRef, Url, PayloadJson, WeakSelf](TFunction<void(TFlockResult<FFlockPlayerData>)> OnAttempt)
		{
			TMap<FString, FString> Headers;
			if (const TSharedPtr<FFlockCommandProvider> Self = WeakSelf.Pin())
			{
				Headers = Self->HeadersNow();
			}
			return ClientRef->PostJsonRaw<FFlockPlayerData>(Url, Headers, PayloadJson, MoveTemp(OnAttempt));
		},
		[WeakSelf, OnComplete](TFlockResult<FFlockPlayerData> Result)
		{
			if (const TSharedPtr<FFlockCommandProvider> Self = WeakSelf.Pin())
			{
				if (Result.bSuccess)
				{
					Self->ApplyToPlayerCache(Result.Value);
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		},
		Context, bIdempotent);
}

void FFlockCommandProvider::SendQueueable(const FString& Path, const FString& PayloadJson, const FString& Context,
	const FString& PlayerDataId, const FFlockCommandData& OptimisticFields,
	TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete)
{
	if (!IsServerReachable())
	{
		const FFlockPlayerData Optimistic = EnqueueOffline(Path, PayloadJson, Context, PlayerDataId, OptimisticFields);
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockPlayerData>::Ok(Optimistic));
		}
		return;
	}

	PostCommand(Path, PayloadJson, Context, /*bIdempotent*/ true, MoveTemp(OnComplete));
}

FFlockPlayerData FFlockCommandProvider::EnqueueOffline(const FString& Path, const FString& PayloadJson,
	const FString& Context, const FString& PlayerDataId, const FFlockCommandData& OptimisticFields)
{
	EnsureQueueLoaded();

	FFlockPendingCommand Pending;
	Pending.Path = Path;
	Pending.PayloadJson = PayloadJson;
	Pending.Context = Context;
	Pending.PlayerDataId = PlayerDataId;
	PendingWrites.Add(Pending);
	PersistQueue();

	Logger->LogWarning(FString::Printf(TEXT("%s: offline — queued for sync on reconnect"), *Context));

	// Overlay the change onto the cached row so a read-after-write is consistent. An uncached row yields an
	// empty record; the authoritative values land when the queue replays.
	FFlockPlayerData Row;
	const TSharedPtr<FFlockPlayerProvider> Players = PlayerProvider.Pin();
	if (!Players.IsValid() || !Players->TryGetCachedRow(PlayerDataId, Row))
	{
		return FFlockPlayerData();
	}
	if (!OptimisticFields.IsEmpty())
	{
		Row.Data.OverlayFields(OptimisticFields.ToJsonObject());
		Players->ApplyServerPlayerData(Row);
	}
	return Row;
}

void FFlockCommandProvider::EnsureQueueLoaded()
{
	// Reload whenever the signed-in player changes (logout included, which loads the empty queue for no
	// player). The previous player's in-memory writes are dropped so they can't replay under a new login;
	// their persisted copy stays under their own scope and replays when they sign back in.
	const FString PlayerId = Session->GetPlayerId();
	if (bQueueLoaded && QueuePlayerId == PlayerId)
	{
		return;
	}
	bQueueLoaded = true;
	QueuePlayerId = PlayerId;
	PendingWrites.Reset();

	const TSharedPtr<FFlockSnapshotStore> Store = GetSnapshotStore();
	FString Payload;
	if (Store.IsValid() && Store->TryRead(GetQueueScope(), PendingWritesKey, Payload))
	{
		TArray<FFlockPendingCommand> Saved;
		if (FFlockJsonUtils::ArrayFromPlainJson(Payload, Saved))
		{
			PendingWrites = MoveTemp(Saved);
		}
	}
}

void FFlockCommandProvider::PersistQueue()
{
	const TSharedPtr<FFlockSnapshotStore> Store = GetSnapshotStore();
	if (!Store.IsValid())
	{
		return;
	}
	FString Payload;
	if (FFlockJsonUtils::ArrayToPlainJson(PendingWrites, Payload))
	{
		Store->Write(GetQueueScope(), PendingWritesKey, Payload);
	}
}

FString FFlockCommandProvider::GetQueueScope() const
{
	// QueuePlayerId, not the live player id, so a persist always writes back to the scope it loaded from.
	return FString::Printf(TEXT("%s/%s"), *GetSnapshotScope(SnapshotCategory), *QueuePlayerId);
}

void FFlockCommandProvider::ApplyToPlayerCache(const FFlockPlayerData& Row)
{
	if (const TSharedPtr<FFlockPlayerProvider> Players = PlayerProvider.Pin())
	{
		Players->ApplyServerPlayerData(Row);
	}
}

void FFlockCommandProvider::EvictOptimisticRow(const FString& PlayerDataId)
{
	if (PlayerDataId.IsEmpty())
	{
		return;
	}
	for (const FFlockPendingCommand& Pending : PendingWrites)
	{
		if (Pending.PlayerDataId == PlayerDataId)
		{
			return; // another queued write still overlays this row — its optimistic value must survive.
		}
	}
	if (const TSharedPtr<FFlockPlayerProvider> Players = PlayerProvider.Pin())
	{
		Players->EvictPlayerCacheByRow(PlayerDataId);
	}
}

bool FFlockCommandProvider::IsPermanentFailure(const FFlockError& Error)
{
	// Auth is recoverable by signing in again, so an auth failure keeps the write queued rather than
	// discarding a change the player made.
	if (Error.Type == EFlockErrorType::Auth)
	{
		return false;
	}
	return FFlockError::IsPermanentStatus(Error.StatusCode);
}
