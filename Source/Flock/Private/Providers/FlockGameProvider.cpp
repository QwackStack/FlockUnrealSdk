// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Providers/FlockGameProvider.h"

#include "Http/FlockEndpoints.h"

const TCHAR* const FFlockGameProvider::SnapshotCategory = TEXT("game");

FFlockGameProvider::FFlockGameProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
	const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession,
	const FString& InVersionedApiUrl, const TSharedPtr<FFlockSnapshotStore>& InSnapshotStore,
	const FString& InGameVersionId)
	: FFlockProviderBase(InClient, InPolicy, InLogger)
	, Session(InSession)
	, VersionedApiUrl(InVersionedApiUrl)
{
	SetSnapshotStore(InSnapshotStore, InGameVersionId);
}

void FFlockGameProvider::GetGame(TFunction<void(TFlockResult<FFlockGameSchema>)> OnComplete)
{
	if (bGameFetched)
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockGameSchema>::Ok(Game));
		}
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::Game);
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockGameProvider> WeakSelf = AsShared();

	FetchWithSnapshot<FFlockGameSchema>(SnapshotCategory, TEXT("game"),
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockGameSchema>)> OnAttempt)
		{
			return ClientRef->Get<FFlockGameSchema>(Url, Headers, MoveTemp(OnAttempt));
		},
		TEXT("Fetch game"),
		[WeakSelf, OnComplete](TFlockResult<FFlockGameSchema> Result)
		{
			if (const TSharedPtr<FFlockGameProvider> Self = WeakSelf.Pin())
			{
				if (Result.bSuccess)
				{
					Self->Game = Result.Value;
					Self->bGameFetched = true;
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		});
}

void FFlockGameProvider::GetGameVersion(TFunction<void(TFlockResult<FFlockGameVersionSchema>)> OnComplete)
{
	if (bVersionFetched)
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockGameVersionSchema>::Ok(GameVersion));
		}
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::GameVersion);
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockGameProvider> WeakSelf = AsShared();

	FetchWithSnapshot<FFlockGameVersionSchema>(SnapshotCategory, TEXT("game_version"),
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockGameVersionSchema>)> OnAttempt)
		{
			return ClientRef->Get<FFlockGameVersionSchema>(Url, Headers, MoveTemp(OnAttempt));
		},
		TEXT("Fetch game version"),
		[WeakSelf, OnComplete](TFlockResult<FFlockGameVersionSchema> Result)
		{
			if (const TSharedPtr<FFlockGameProvider> Self = WeakSelf.Pin())
			{
				if (Result.bSuccess)
				{
					Self->GameVersion = Result.Value;
					Self->bVersionFetched = true;
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		});
}

void FFlockGameProvider::GetGameVersionByName(const FString& Name, TFunction<void(TFlockResult<FFlockGameVersionSchema>)> OnComplete)
{
	if (!RequireNotEmpty(Name, TEXT("Game Version Name"), OnComplete))
	{
		return;
	}
	if (const FFlockGameVersionSchema* Cached = VersionsByName.Find(Name))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockGameVersionSchema>::Ok(*Cached));
		}
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::GameVersionByName(Name));
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockGameProvider> WeakSelf = AsShared();

	// BootstrapScope, keyed by API URL + name: this runs before the version id is known, so it can't be
	// scoped under it, and the API URL keeps dev and prod backends from sharing an entry.
	const FString Key = FString::Printf(TEXT("%s|%s"), *VersionedApiUrl, *Name);
	FetchAtScope<FFlockGameVersionSchema>(FFlockSnapshotStore::BootstrapScope, Key,
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockGameVersionSchema>)> OnAttempt)
		{
			return ClientRef->Get<FFlockGameVersionSchema>(Url, Headers, MoveTemp(OnAttempt));
		},
		TEXT("Fetch game version by name"),
		[WeakSelf, Name, OnComplete](TFlockResult<FFlockGameVersionSchema> Result)
		{
			if (const TSharedPtr<FFlockGameProvider> Self = WeakSelf.Pin())
			{
				if (Result.bSuccess)
				{
					Self->VersionsByName.Add(Name, Result.Value);
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		});
}

void FFlockGameProvider::ClearCache()
{
	bGameFetched = false;
	Game = FFlockGameSchema();
	bVersionFetched = false;
	GameVersion = FFlockGameVersionSchema();
	VersionsByName.Reset();
	DeleteSnapshotCategory(SnapshotCategory);
}
