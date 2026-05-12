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
		const FString GameVersion = Config->GetGameVersion();
		if (!GameVersion.IsEmpty())
		{
			Headers.Add(TEXT("X-Game-Version-ID"), GameVersion);
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
	const TCHAR* Verb = UQwackFlockGameEndpoints::QwackHttpVerb(Endpoint.RequestType);
	TMap<FString, FString> Headers = MakeHeaders(bIncludeAuth);

	FQwackFlockResponse Cb;
	Cb.BindLambda([OnDone](FQwackHTTPResponse R) { OnDone(R); });
	HttpClient->SendRequest(Url, Verb, Body, Cb, Headers);
}
