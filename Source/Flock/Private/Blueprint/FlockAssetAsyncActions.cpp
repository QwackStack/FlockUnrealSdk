// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockAssetAsyncActions.h"

#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Texture2D.h"
#include "FlockSubsystem.h"
#include "Providers/FlockAssetProvider.h"

namespace
{
	FString ResolveCallOrigin(const UObject* WorldContextObject)
	{
		if (const UBlueprintGeneratedClass* BlueprintClass =
			WorldContextObject ? Cast<UBlueprintGeneratedClass>(WorldContextObject->GetClass()) : nullptr)
		{
			FString AssetName = BlueprintClass->GetName();
			AssetName.RemoveFromEnd(TEXT("_C"));
			return FString::Printf(TEXT("Blueprint '%s'"), *AssetName);
		}
		return TEXT("Blueprint node");
	}

	FFlockAssetProvider* ResolveAssets(UObject* WorldContextObject, FFlockError& OutError)
	{
		UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
		FFlockAssetProvider* Provider = Sdk ? Sdk->GetAssetProvider() : nullptr;
		if (!Provider)
		{
			OutError = FFlockError::Make(EFlockErrorType::Validation,
				TEXT("Flock assets are not available. Initialize the SDK first."));
		}
		return Provider;
	}

	/** 0 until the server declares a content length; callers show an indeterminate bar until then. */
	float ProgressFraction(int64 Received, int64 Total)
	{
		return Total > 0 ? FMath::Clamp(static_cast<float>(Received) / static_cast<float>(Total), 0.f, 1.f) : 0.f;
	}
}

// ───────────────────────────── Download Texture ──────────────────────────────

UFlockDownloadAssetTextureAction* UFlockDownloadAssetTextureAction::DownloadAssetTexture(
	UObject* WorldContextObject, const FString& Asset)
{
	UFlockDownloadAssetTextureAction* Action = NewObject<UFlockDownloadAssetTextureAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Asset = Asset;
	return Action;
}

void UFlockDownloadAssetTextureAction::Activate()
{
	FFlockError Error;
	FFlockAssetProvider* Provider = ResolveAssets(WorldContextObject, Error);
	if (!Provider)
	{
		OnFailure.Broadcast(nullptr, 0.f, Error);
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UFlockDownloadAssetTextureAction> WeakThis(this);
	FFlockAssetProgress Progress;
	Progress.BindLambda([WeakThis](int64 Received, int64 Total)
	{
		if (UFlockDownloadAssetTextureAction* Self = WeakThis.Get())
		{
			Self->OnProgress.Broadcast(nullptr, ProgressFraction(Received, Total), FFlockError());
		}
	});

	FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->DownloadTexture(Asset, Progress, [WeakThis](TFlockResult<UTexture2D*> Result)
	{
		UFlockDownloadAssetTextureAction* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}
		if (Result.bSuccess)
		{
			// Rooted in a UPROPERTY before the broadcast, so the graph cannot receive a collected texture.
			Self->Downloaded = Result.Value;
			Self->OnSuccess.Broadcast(Result.Value, 1.f, FFlockError());
		}
		else
		{
			Self->OnFailure.Broadcast(nullptr, 0.f, Result.Error);
		}
		Self->SetReadyToDestroy();
	});
}

// ─────────────────────────────── Download Text ───────────────────────────────

UFlockDownloadAssetTextAction* UFlockDownloadAssetTextAction::DownloadAssetText(
	UObject* WorldContextObject, const FString& Asset)
{
	UFlockDownloadAssetTextAction* Action = NewObject<UFlockDownloadAssetTextAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Asset = Asset;
	return Action;
}

void UFlockDownloadAssetTextAction::Activate()
{
	FFlockError Error;
	FFlockAssetProvider* Provider = ResolveAssets(WorldContextObject, Error);
	if (!Provider)
	{
		OnFailure.Broadcast(FString(), 0.f, Error);
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UFlockDownloadAssetTextAction> WeakThis(this);
	FFlockAssetProgress Progress;
	Progress.BindLambda([WeakThis](int64 Received, int64 Total)
	{
		if (UFlockDownloadAssetTextAction* Self = WeakThis.Get())
		{
			Self->OnProgress.Broadcast(FString(), ProgressFraction(Received, Total), FFlockError());
		}
	});

	FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->DownloadText(Asset, Progress, [WeakThis](TFlockResult<FString> Result)
	{
		UFlockDownloadAssetTextAction* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}
		if (Result.bSuccess)
		{
			Self->OnSuccess.Broadcast(Result.Value, 1.f, FFlockError());
		}
		else
		{
			Self->OnFailure.Broadcast(FString(), 0.f, Result.Error);
		}
		Self->SetReadyToDestroy();
	});
}

// ─────────────────────────────── Download Bytes ──────────────────────────────

UFlockDownloadAssetBytesAction* UFlockDownloadAssetBytesAction::DownloadAssetBytes(
	UObject* WorldContextObject, const FString& Asset)
{
	UFlockDownloadAssetBytesAction* Action = NewObject<UFlockDownloadAssetBytesAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Asset = Asset;
	return Action;
}

void UFlockDownloadAssetBytesAction::Activate()
{
	FFlockError Error;
	FFlockAssetProvider* Provider = ResolveAssets(WorldContextObject, Error);
	if (!Provider)
	{
		OnFailure.Broadcast(TArray<uint8>(), 0.f, Error);
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UFlockDownloadAssetBytesAction> WeakThis(this);
	FFlockAssetProgress Progress;
	Progress.BindLambda([WeakThis](int64 Received, int64 Total)
	{
		if (UFlockDownloadAssetBytesAction* Self = WeakThis.Get())
		{
			Self->OnProgress.Broadcast(TArray<uint8>(), ProgressFraction(Received, Total), FFlockError());
		}
	});

	FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->DownloadBytes(Asset, Progress, [WeakThis](TFlockResult<TArray<uint8>> Result)
	{
		UFlockDownloadAssetBytesAction* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}
		if (Result.bSuccess)
		{
			Self->OnSuccess.Broadcast(Result.Value, 1.f, FFlockError());
		}
		else
		{
			Self->OnFailure.Broadcast(TArray<uint8>(), 0.f, Result.Error);
		}
		Self->SetReadyToDestroy();
	});
}

// ─────────────────────────────── Download File ───────────────────────────────

UFlockDownloadAssetFileAction* UFlockDownloadAssetFileAction::DownloadAssetFile(
	UObject* WorldContextObject, const FString& Asset)
{
	UFlockDownloadAssetFileAction* Action = NewObject<UFlockDownloadAssetFileAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Asset = Asset;
	return Action;
}

void UFlockDownloadAssetFileAction::Activate()
{
	FFlockError Error;
	FFlockAssetProvider* Provider = ResolveAssets(WorldContextObject, Error);
	if (!Provider)
	{
		OnFailure.Broadcast(FString(), 0.f, Error);
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UFlockDownloadAssetFileAction> WeakThis(this);
	FFlockAssetProgress Progress;
	Progress.BindLambda([WeakThis](int64 Received, int64 Total)
	{
		if (UFlockDownloadAssetFileAction* Self = WeakThis.Get())
		{
			Self->OnProgress.Broadcast(FString(), ProgressFraction(Received, Total), FFlockError());
		}
	});

	FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->DownloadFile(Asset, Progress, [WeakThis](TFlockResult<FString> Result)
	{
		UFlockDownloadAssetFileAction* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}
		if (Result.bSuccess)
		{
			Self->OnSuccess.Broadcast(Result.Value, 1.f, FFlockError());
		}
		else
		{
			Self->OnFailure.Broadcast(FString(), 0.f, Result.Error);
		}
		Self->SetReadyToDestroy();
	});
}

// ──────────────────────────────── Get Assets ─────────────────────────────────

UFlockGetAssetsAction* UFlockGetAssetsAction::GetAssets(UObject* WorldContextObject)
{
	UFlockGetAssetsAction* Action = NewObject<UFlockGetAssetsAction>();
	Action->WorldContextObject = WorldContextObject;
	return Action;
}

void UFlockGetAssetsAction::Activate()
{
	FFlockError Error;
	FFlockAssetProvider* Provider = ResolveAssets(WorldContextObject, Error);
	if (!Provider)
	{
		OnFailure.Broadcast(TArray<FFlockAsset>(), Error);
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UFlockGetAssetsAction> WeakThis(this);
	FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->GetAll([WeakThis](TFlockResult<TArray<FFlockAsset>> Result)
	{
		UFlockGetAssetsAction* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}
		if (Result.bSuccess)
		{
			Self->OnSuccess.Broadcast(Result.Value, FFlockError());
		}
		else
		{
			Self->OnFailure.Broadcast(TArray<FFlockAsset>(), Result.Error);
		}
		Self->SetReadyToDestroy();
	});
}

// ───────────────────────────────── Get Asset ─────────────────────────────────

UFlockGetAssetAction* UFlockGetAssetAction::GetAsset(UObject* WorldContextObject, const FString& Asset)
{
	UFlockGetAssetAction* Action = NewObject<UFlockGetAssetAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Asset = Asset;
	return Action;
}

void UFlockGetAssetAction::Activate()
{
	FFlockError Error;
	FFlockAssetProvider* Provider = ResolveAssets(WorldContextObject, Error);
	if (!Provider)
	{
		OnFailure.Broadcast(FFlockAsset(), Error);
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UFlockGetAssetAction> WeakThis(this);
	FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->GetByIdOrName(Asset, [WeakThis](TFlockResult<FFlockAsset> Result)
	{
		UFlockGetAssetAction* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}
		if (Result.bSuccess)
		{
			Self->OnSuccess.Broadcast(Result.Value, FFlockError());
		}
		else
		{
			Self->OnFailure.Broadcast(FFlockAsset(), Result.Error);
		}
		Self->SetReadyToDestroy();
	});
}

// ─────────────────────────────── Preload Assets ──────────────────────────────

UFlockPreloadAssetsAction* UFlockPreloadAssetsAction::PreloadAssets(UObject* WorldContextObject,
	const TArray<FFlockAsset>& Assets)
{
	UFlockPreloadAssetsAction* Action = NewObject<UFlockPreloadAssetsAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Assets = Assets;
	return Action;
}

UFlockPreloadAssetsAction* UFlockPreloadAssetsAction::PreloadAllAssets(UObject* WorldContextObject)
{
	UFlockPreloadAssetsAction* Action = NewObject<UFlockPreloadAssetsAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->bAll = true;
	return Action;
}

void UFlockPreloadAssetsAction::Activate()
{
	FFlockError Error;
	FFlockAssetProvider* Provider = ResolveAssets(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<int32>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockPreloadAssetsAction> WeakThis(this);
	auto OnProgressFn = [WeakThis](float Fraction)
	{
		if (UFlockPreloadAssetsAction* Self = WeakThis.Get())
		{
			Self->OnProgress.Broadcast(0, Self->Assets.Num(), Fraction, FFlockError());
		}
	};
	auto OnDone = [WeakThis](TFlockResult<int32> Result)
	{
		if (UFlockPreloadAssetsAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	};

	FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	if (bAll)
	{
		// Remember what the batch turned out to be, so the Requested pin reports the real count.
		Provider->PreloadWhere([WeakThis](const FFlockAsset& Asset)
		{
			if (UFlockPreloadAssetsAction* Self = WeakThis.Get())
			{
				Self->Assets.Add(Asset);
			}
			return true;
		}, OnProgressFn, OnDone);
		return;
	}
	Provider->Preload(Assets, OnProgressFn, OnDone);
}

void UFlockPreloadAssetsAction::Complete(const TFlockResult<int32>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, Assets.Num(), 1.f, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(0, Assets.Num(), 0.f, Result.Error);
	}
	SetReadyToDestroy();
}
