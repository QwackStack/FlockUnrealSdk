#include "QwackAuthSubsystem.h"

#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Qwack_ue_Sdk/Endpoints/QwackGameEndpoints.h"
#include "Qwack_ue_Sdk/GameAPI/QwackFlockGameSubsystem.h"
#include "Qwack_ue_Sdk/HTTPClient/HTTPResponse.h"

namespace
{
	void AppendDebugLog(const FString& Message, const FString& DataJson)
	{
		const FString Payload = FString::Printf(
			TEXT("{\"sessionId\":\"70cb75\",\"message\":\"%ls\",\"data\":%s,\"timestamp\":%lld}"),
			 *Message.ReplaceCharWithEscapedChar(), *DataJson, FDateTime::UtcNow().ToUnixTimestamp() * 1000LL);
		FFileHelper::SaveStringToFile(
			Payload,
			*(FPaths::ProjectDir() / TEXT("debug-70cb75.log")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(),
			FILEWRITE_Append);
	}

	UQwackFlockGameSubsystem* GetGame(const UGameInstanceSubsystem* Self)
	{
		if (!Self) return nullptr;
		const UGameInstance* GI = Self->GetGameInstance();
		return GI ? GI->GetSubsystem<UQwackFlockGameSubsystem>() : nullptr;
	}
}

void UQwackAuthSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// #region agent log
	AppendDebugLog( TEXT("Auth subsystem initialize"), TEXT("{\"accessTokenPresent\":false,\"refreshTokenPresent\":false,\"playerIdPresent\":false}"));
	// #endregion
}

void UQwackAuthSubsystem::Deinitialize()
{
	// #region agent log
	AppendDebugLog(
		TEXT("Auth subsystem deinitialize"),
		FString::Printf(
			TEXT("{\"accessTokenPresent\":%s,\"refreshTokenPresent\":%s,\"playerIdPresent\":%s}"),
			AccessToken.IsEmpty() ? TEXT("false") : TEXT("true"),
			RefreshTokenValue.IsEmpty() ? TEXT("false") : TEXT("true"),
			PlayerId.IsEmpty() ? TEXT("false") : TEXT("true")));
	// #endregion
	Super::Deinitialize();
}

void UQwackAuthSubsystem::SetAccessToken(const FString& InAccessToken)
{
	if (AccessToken == InAccessToken) return;
	AccessToken = InAccessToken;
	// #region agent log
	AppendDebugLog(
		TEXT("SetAccessToken invoked"),
		FString::Printf(TEXT("{\"newTokenPresent\":%s}"), AccessToken.IsEmpty() ? TEXT("false") : TEXT("true")));
	// #endregion
	OnAccessTokenChanged.Broadcast(AccessToken);
}

void UQwackAuthSubsystem::StoreAuthFromResponse(const FFlockPlayerAuth& Auth)
{
	PlayerId = Auth.player_id;
	RefreshTokenValue = Auth.refresh_token;
	if (AccessToken != Auth.access_token)
	{
		AccessToken = Auth.access_token;
		// #region agent log
		AppendDebugLog(
			TEXT("StoreAuthFromResponse updated token"),
			FString::Printf(TEXT("{\"newTokenPresent\":%s,\"refreshTokenPresent\":%s,\"playerIdPresent\":%s}"),
				AccessToken.IsEmpty() ? TEXT("false") : TEXT("true"),
				RefreshTokenValue.IsEmpty() ? TEXT("false") : TEXT("true"),
				PlayerId.IsEmpty() ? TEXT("false") : TEXT("true")));
		// #endregion
		OnAccessTokenChanged.Broadcast(AccessToken);
	}
}

void UQwackAuthSubsystem::RegisterPlayer(const FFlockPlayerRegisterRequest& Request, const FFlockOnAuthResponse& Callback)
{
	UQwackFlockGameSubsystem* Game = GetGame(this);
	if (!Game)
	{
		FFlockPlayerAuthResponse Out;
		Callback.ExecuteIfBound(Out);
		return;
	}

	FString Body;
	FJsonObjectConverter::UStructToJsonObjectString(Request, Body, 0, 0);

	TWeakObjectPtr<UQwackAuthSubsystem> WeakThis(this);
	Game->Send(UQwackFlockGameEndpoints::PlayerRegister, FString(), Body, /*bIncludeAuth*/ false,
		[WeakThis, Callback](const FQwackHTTPResponse& R)
		{
			FFlockPlayerAuthResponse Out;
			Out.Meta = UQwackFlockGameSubsystem::MakeMeta(R);
			if (Out.Meta.bSuccess)
			{
				FJsonObjectConverter::JsonObjectStringToUStruct(R.FullText, &Out.Auth, 0, 0);
				if (UQwackAuthSubsystem* Strong = WeakThis.Get())
				{
					Strong->StoreAuthFromResponse(Out.Auth);
				}
			}
			Callback.ExecuteIfBound(Out);
		});
}

void UQwackAuthSubsystem::LoginPlayer(const FFlockPlayerLoginRequest& Request, const FFlockOnAuthResponse& Callback)
{
	UQwackFlockGameSubsystem* Game = GetGame(this);
	if (!Game)
	{
		FFlockPlayerAuthResponse Out;
		Callback.ExecuteIfBound(Out);
		return;
	}

	FString Body;
	FJsonObjectConverter::UStructToJsonObjectString(Request, Body, 0, 0);
	const UEnum* LoginTypeEnum = StaticEnum<EFlockLoginType>();
	const FString LoginType = LoginTypeEnum ? LoginTypeEnum->GetNameStringByValue(static_cast<int64>(Request.login_type)) : TEXT("unknown");
	// #region agent log
	AppendDebugLog(
		TEXT("Login request serialized"),
		FString::Printf(
			TEXT("{\"loginType\":\"%s\",\"emailPresent\":%s,\"passwordPresent\":%s,\"googleIdPresent\":%s,\"deviceIdPresent\":%s,\"steamIdPresent\":%s,\"requestJsonLength\":%d}"),
			*LoginType,
			Request.email.IsEmpty() ? TEXT("false") : TEXT("true"),
			Request.password.IsEmpty() ? TEXT("false") : TEXT("true"),
			Request.google_id.IsEmpty() ? TEXT("false") : TEXT("true"),
			Request.device_id.IsEmpty() ? TEXT("false") : TEXT("true"),
			Request.steam_id.IsEmpty() ? TEXT("false") : TEXT("true"),
			Body.Len()));
	// #endregion

	TWeakObjectPtr<UQwackAuthSubsystem> WeakThis(this);
	Game->Send(UQwackFlockGameEndpoints::PlayerLogin, FString(), Body, /*bIncludeAuth*/ false,
		[WeakThis, Callback](const FQwackHTTPResponse& R)
		{
			// #region agent log
			AppendDebugLog(
				TEXT("Login response received"),
				FString::Printf(TEXT("{\"statusCode\":%d,\"success\":%s,\"bodyLength\":%d}"),
					R.StatusCode,
					R.success ? TEXT("true") : TEXT("false"),
					R.FullText.Len()));
			// #endregion
			FFlockPlayerAuthResponse Out;
			Out.Meta = UQwackFlockGameSubsystem::MakeMeta(R);
			if (Out.Meta.bSuccess)
			{
				FJsonObjectConverter::JsonObjectStringToUStruct(R.FullText, &Out.Auth, 0, 0);
				if (UQwackAuthSubsystem* Strong = WeakThis.Get())
				{
					Strong->StoreAuthFromResponse(Out.Auth);
				}
			}
			Callback.ExecuteIfBound(Out);
		});
}

void UQwackAuthSubsystem::RefreshToken(const FFlockPlayerRefreshRequest& Request, const FFlockOnAuthResponse& Callback)
{
	UQwackFlockGameSubsystem* Game = GetGame(this);
	if (!Game)
	{
		FFlockPlayerAuthResponse Out;
		Callback.ExecuteIfBound(Out);
		return;
	}

	FString Body;
	FJsonObjectConverter::UStructToJsonObjectString(Request, Body, 0, 0);

	TWeakObjectPtr<UQwackAuthSubsystem> WeakThis(this);
	Game->Send(UQwackFlockGameEndpoints::PlayerTokenRefresh, FString(), Body, /*bIncludeAuth*/ false,
		[WeakThis, Callback](const FQwackHTTPResponse& R)
		{
			FFlockPlayerAuthResponse Out;
			Out.Meta = UQwackFlockGameSubsystem::MakeMeta(R);
			if (Out.Meta.bSuccess)
			{
				FJsonObjectConverter::JsonObjectStringToUStruct(R.FullText, &Out.Auth, 0, 0);
				if (UQwackAuthSubsystem* Strong = WeakThis.Get())
				{
					Strong->StoreAuthFromResponse(Out.Auth);
				}
			}
			Callback.ExecuteIfBound(Out);
		});
}

void UQwackAuthSubsystem::AuthTest(const FFlockOnAuthTestResponse& Callback)
{
	UQwackFlockGameSubsystem* Game = GetGame(this);
	if (!Game)
	{
		FFlockAuthTestResponse Out;
		Callback.ExecuteIfBound(Out);
		return;
	}

	Game->Send(UQwackFlockGameEndpoints::PlayerAuthTest, FString(), FString(), /*bIncludeAuth*/ true,
		[Callback](const FQwackHTTPResponse& R)
		{
			FFlockAuthTestResponse Out;
			Out.Meta = UQwackFlockGameSubsystem::MakeMeta(R);
			Out.RequesterJson = R.FullText;
			Callback.ExecuteIfBound(Out);
		});
}
