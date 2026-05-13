#include "QwackConfigSubsystem.h"
#include "QwackSettings.h"

#include "Dom/JsonObject.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogFlockConfig, Log, All);

void UQwackConfigSubsystem::Deinitialize()
{
	// Drop any pending resolve callbacks — the request may still be in flight
	// but the lambda holds a TWeakObjectPtr so it will no-op on completion.
	PendingResolveCallbacks.Reset();
	Super::Deinitialize();
}

const UQwackSettings* UQwackConfigSubsystem::GetSettings() const
{
	return GetDefault<UQwackSettings>();
}

FString UQwackConfigSubsystem::GetApiUrl() const
{
	if (!ApiUrlOverride.IsEmpty()) return ApiUrlOverride;
	const UQwackSettings* S = GetSettings();
	return S ? S->ApiUrl : FString();
}

FString UQwackConfigSubsystem::GetApiKey() const
{
	if (!ApiKeyOverride.IsEmpty()) return ApiKeyOverride;
	const UQwackSettings* S = GetSettings();
	return S ? S->ApiKey : FString();
}

FString UQwackConfigSubsystem::GetGameId() const
{
	if (!GameIdOverride.IsEmpty()) return GameIdOverride;
	const UQwackSettings* S = GetSettings();
	return S ? S->GameId : FString();
}

FString UQwackConfigSubsystem::GetGameVersion() const
{
	if (!GameVersionOverride.IsEmpty()) return GameVersionOverride;
	const UQwackSettings* S = GetSettings();
	return S ? S->GameVersion : FString();
}

void UQwackConfigSubsystem::SetApiUrlOverride(const FString& InUrl)
{
	ApiUrlOverride = InUrl;
}

void UQwackConfigSubsystem::SetApiKeyOverride(const FString& InKey)
{
	ApiKeyOverride = InKey;
}

void UQwackConfigSubsystem::SetGameIdOverride(const FString& InGameId)
{
	GameIdOverride = InGameId;
}

void UQwackConfigSubsystem::SetGameVersionOverride(const FString& InVersion)
{
	if (GameVersionOverride == InVersion) return;
	GameVersionOverride = InVersion;
	// Name changed — cached UUID and the prior attempt are no longer valid.
	// Don't auto-refire; the next Send that needs the header will trigger
	// EnsureGameVersionResolved.
	ResolvedGameVersionId.Reset();
	bGameVersionResolveAttempted = false;
}

void UQwackConfigSubsystem::ClearOverrides()
{
	ApiUrlOverride.Reset();
	ApiKeyOverride.Reset();
	GameIdOverride.Reset();
	GameVersionOverride.Reset();
	ResolvedGameVersionId.Reset();
	bGameVersionResolveAttempted = false;
}

// =====================================================================
// Game version UUID resolution
// =====================================================================

void UQwackConfigSubsystem::EnsureGameVersionResolved()
{
	if (IsGameVersionResolved()) return;
	if (bGameVersionResolveInFlight) return;
	StartGameVersionResolve();
}

void UQwackConfigSubsystem::OnGameVersionResolved(TFunction<void(bool)> Callback)
{
	if (!Callback) return;
	if (IsGameVersionResolved()) { Callback(true); return; }
	PendingResolveCallbacks.Add(MoveTemp(Callback));
}

void UQwackConfigSubsystem::StartGameVersionResolve()
{
	const FString Name = GetGameVersion();
	const FString ApiKey = GetApiKey();
	FString Base = GetApiUrl();

	if (Name.IsEmpty() || ApiKey.IsEmpty() || Base.IsEmpty())
	{
		UE_LOG(LogFlockConfig, Warning,
			TEXT("Cannot resolve X-Game-Version-ID — missing %s%s%s"),
			Name.IsEmpty()   ? TEXT("GameVersion ") : TEXT(""),
			ApiKey.IsEmpty() ? TEXT("ApiKey ")      : TEXT(""),
			Base.IsEmpty()   ? TEXT("ApiUrl ")      : TEXT(""));
		FinishGameVersionResolve(false, FString());
		return;
	}

	if (Base.EndsWith(TEXT("/"))) Base.LeftChopInline(1);
	const FString Url = Base + TEXT("/v1/game_version/by-name/")
		+ FGenericPlatformHttp::UrlEncode(Name);

	bGameVersionResolveInFlight = true;

	const TSharedRef<IHttpRequest> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(Url);
	Req->SetVerb(TEXT("GET"));
	Req->SetHeader(TEXT("X-Flock-API-Key"), ApiKey);

	TWeakObjectPtr<UQwackConfigSubsystem> WeakThis(this);
	Req->OnProcessRequestComplete().BindLambda(
		[WeakThis](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
		{
			UQwackConfigSubsystem* Self = WeakThis.Get();
			if (!Self) return;

			const int32 Code = Resp.IsValid() ? Resp->GetResponseCode() : 0;
			const FString Body = Resp.IsValid() ? Resp->GetContentAsString() : FString();
			const bool bHttpOk = bOk && Code >= 200 && Code < 300;

			FString ResolvedId;
			if (bHttpOk)
			{
				TSharedPtr<FJsonObject> Root;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
				if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
				{
					const TSharedPtr<FJsonObject>* ResultObj = nullptr;
					if (Root->TryGetObjectField(TEXT("result"), ResultObj) && ResultObj && ResultObj->IsValid())
					{
						(*ResultObj)->TryGetStringField(TEXT("id"), ResolvedId);
					}
				}
			}

			const bool bSuccess = bHttpOk && !ResolvedId.IsEmpty();
			if (!bSuccess)
			{
				UE_LOG(LogFlockConfig, Warning,
					TEXT("X-Game-Version-ID resolve failed (code=%d, body=%s)"),
					Code, *Body.Left(160).Replace(TEXT("\n"), TEXT(" ")));
			}
			Self->FinishGameVersionResolve(bSuccess, ResolvedId);
		});
	Req->ProcessRequest();
}

void UQwackConfigSubsystem::FinishGameVersionResolve(bool bSuccess, const FString& InResolvedId)
{
	bGameVersionResolveInFlight = false;
	bGameVersionResolveAttempted = true;
	if (bSuccess)
	{
		ResolvedGameVersionId = InResolvedId;
		UE_LOG(LogFlockConfig, Log, TEXT("Resolved X-Game-Version-ID: %s"), *ResolvedGameVersionId);
	}

	// Drain — even on failure — so deferred sends don't hang forever. The Send
	// gate also checks WasGameVersionResolveAttempted, so failed-and-drained
	// callbacks won't loop back into another resolve attempt; they'll fall
	// through with no X-Game-Version-ID and let the server surface the real error.
	TArray<TFunction<void(bool)>> Drained = MoveTemp(PendingResolveCallbacks);
	PendingResolveCallbacks.Reset();
	for (auto& Cb : Drained)
	{
		Cb(bSuccess);
	}
}
