#include "QwackFlockGameSubsystem.h"

#include "Engine/GameInstance.h"
#include "Qwack_ue_Sdk/Auth/QwackAuthSubsystem.h"
#include "Qwack_ue_Sdk/Config/QwackConfigSubsystem.h"
#include "Qwack_ue_Sdk/Endpoints/QwackGameEndpoints.h"
#include "Qwack_ue_Sdk/HTTPClient/HTTPResponse.h"
#include "Qwack_ue_Sdk/HTTPClient/SHTTPClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogFlockSdk, Log, All);

void UQwackFlockGameSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	HttpClient = NewObject<USHTTPClient>(this);
}

void UQwackFlockGameSubsystem::Deinitialize()
{
	HttpClient = nullptr;
	Super::Deinitialize();
}

FString UQwackFlockGameSubsystem::BuildUrl(const FString& Path) const
{
	FString Base;
	if (const UGameInstance* GI = GetGameInstance())
	{
		if (const UQwackConfigSubsystem* Config = GI->GetSubsystem<UQwackConfigSubsystem>())
		{
			Base = Config->GetApiUrl();
		}
	}
	if (Base.IsEmpty())
	{
		UE_LOG(LogFlockSdk, Warning,
			TEXT("ApiUrl is empty — set it in Project Settings → Plugins → Flock or via UQwackConfigSubsystem::SetApiUrlOverride"));
		return FString();
	}
	if (Base.EndsWith(TEXT("/"))) Base.LeftChopInline(1);
	return Base + Path;
}

TMap<FString, FString> UQwackFlockGameSubsystem::MakeHeaders(bool bIncludeAuth) const
{
	TMap<FString, FString> Headers;
	const UGameInstance* GI = GetGameInstance();
	if (!GI) return Headers;

	if (const UQwackConfigSubsystem* Config = GI->GetSubsystem<UQwackConfigSubsystem>())
	{
		const FString ApiKey = Config->GetApiKey();
		if (!ApiKey.IsEmpty())
		{
			Headers.Add(TEXT("X-Flock-API-Key"), ApiKey);
		}
		// X-Game-Version-ID must be the UUID of the game version, not the
		// human-readable name. The UUID is fetched lazily by Send (see below).
		const FString GameVersionId = Config->GetGameVersionId();
		if (!GameVersionId.IsEmpty())
		{
			Headers.Add(TEXT("X-Game-Version-ID"), GameVersionId);
		}
	}
	if (bIncludeAuth)
	{
		if (const UQwackAuthSubsystem* Auth = GI->GetSubsystem<UQwackAuthSubsystem>())
		{
			const FString Token = Auth->GetAccessToken();
			if (!Token.IsEmpty())
			{
				Headers.Add(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Token));
			}
		}
	}
	return Headers;
}

FFlockOpResult UQwackFlockGameSubsystem::MakeMeta(const FQwackHTTPResponse& R)
{
	FFlockOpResult Meta;
	Meta.StatusCode = R.StatusCode;
	Meta.bSuccess = (R.StatusCode >= 200 && R.StatusCode < 300);
	Meta.ResultJson = R.FullText;
	if (!Meta.bSuccess) Meta.ErrorMessage = R.FullText;
	return Meta;
}

void UQwackFlockGameSubsystem::Send(const FSQwackFlockEndpoints& Endpoint,
                                    const FString& UrlOverride,
                                    const FString& Body,
                                    bool bIncludeAuth,
                                    TFunction<void(const FQwackHTTPResponse&)> OnDone) const
{
	if (!HttpClient)
	{
		FQwackHTTPResponse R; R.StatusCode = 0; R.FullText = TEXT("HttpClient not initialized");
		OnDone(R); return;
	}
	const FString Url = UrlOverride.IsEmpty() ? BuildUrl(Endpoint.EndPoint) : UrlOverride;
	if (Url.IsEmpty())
	{
		FQwackHTTPResponse R; R.StatusCode = 0; R.FullText = TEXT("Empty URL — ApiUrl not configured");
		OnDone(R); return;
	}

	// Defer until the X-Game-Version-ID UUID has been resolved from the
	// configured name. The resolver in UQwackConfigSubsystem uses FHttpModule
	// directly (it doesn't go through Send), so no recursion concern. If no
	// name is configured we let the request fly with no version header — the
	// server's behavior in that case is the user's call, not ours.
	UQwackConfigSubsystem* Config = nullptr;
	if (const UGameInstance* GI = GetGameInstance())
	{
		Config = GI->GetSubsystem<UQwackConfigSubsystem>();
	}
	if (Config
		&& !Config->IsGameVersionResolved()
		&& !Config->WasGameVersionResolveAttempted()
		&& !Config->GetGameVersion().IsEmpty())
	{
		TWeakObjectPtr<const UQwackFlockGameSubsystem> WeakThis(this);
		Config->OnGameVersionResolved(
			[WeakThis, Endpoint, UrlOverride, Body, bIncludeAuth, OnDone](bool)
			{
				if (const UQwackFlockGameSubsystem* Strong = WeakThis.Get())
				{
					Strong->Send(Endpoint, UrlOverride, Body, bIncludeAuth, OnDone);
				}
			});
		Config->EnsureGameVersionResolved();
		return;
	}

	const TCHAR* Verb = UQwackFlockGameEndpoints::QwackHttpVerb(Endpoint.RequestType);
	TMap<FString, FString> Headers = MakeHeaders(bIncludeAuth);

	FQwackFlockResponse Cb;
	Cb.BindLambda([OnDone](FQwackHTTPResponse R) { OnDone(R); });
	HttpClient->SendRequest(Url, Verb, Body, Cb, Headers);
}
