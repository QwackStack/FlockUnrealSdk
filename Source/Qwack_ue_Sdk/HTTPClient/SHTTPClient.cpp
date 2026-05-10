// Copyright 2022, Qwack. All Rights Reserved.

#include "SHTTPClient.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "HTTPResponse.h"
#include "Qwack_ue_Sdk/Config/QwackConfig.h"
#include "Qwack_ue_Sdk/Utils/QwackSDKUtils.h"


const FString USHTTPClient::UserAgent = FString::Format(TEXT("X-UnrealEngine-Agent/{0}"), { ENGINE_VERSION_STRING });
const FString USHTTPClient::UserInstanceIdentifier = FGuid::NewGuid().ToString();
USHTTPClient::USHTTPClient()
{
}

void USHTTPClient::SendRequest(const FString& endPoint, const FString& requestType, const FString& data,
	 FQwackFlockResponse OnCompleteRequest, TMap<FString, FString> customHeaders) const
{
	// Create HTTP request.
	
	TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(endPoint);
	HttpRequest->SetVerb(requestType);

	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	HttpRequest->SetHeader(TEXT("User-Agent"), UserAgent);
	// Set request data, if any.
	if (!data.IsEmpty())
	{
		HttpRequest->SetContentAsString(data);
		HttpRequest->SetHeader(TEXT("User-Instance-Identifier"), UserInstanceIdentifier);
	}

	// Add custom headers, if any.
	for (const auto& Header : customHeaders)
	{
		HttpRequest->SetHeader(Header.Key, Header.Value);
	}

	// Set up completion callback using lambda function.
	HttpRequest->OnProcessRequestComplete().BindLambda([OnCompleteRequest, this, endPoint, requestType, data](FHttpRequestPtr Req, const FHttpResponsePtr& Response, bool bWasSuccessful)
	{
		FQwackHTTPResponse FlockResponse;
		FlockResponse.StatusCode = (Response.IsValid() ? Response->GetResponseCode() : 0);
		FlockResponse.FullText   = (Response.IsValid() ? Response->GetContentAsString() : TEXT("Failed to process request."));

		// TODO Handle different status codes and errors as needed.
		// need helper function for that
		/*if (bWasSuccessful && Response.IsValid() && EHttpResponseCodes::IsOk(FlockResponse.StatusCode))
		{
			FlockResponse.success = true;
		}
		else
		{
			FlockResponse.success = false;
		}*/
		// Call the provided completion callback with the response.
		OnCompleteRequest.ExecuteIfBound(FlockResponse);
	});

	// Send the request.
	HttpRequest->ProcessRequest();
}

// found out this may not be necssary it just brings overhead ! we are supposed to obtain a ticket by asking backend directly instead?
void USHTTPClient::GetToken(const FString& TokenEndpoint,bool bPlayer, const FString& SteamToken,const FQwackHTTPResponse& OnCompleteRequest) const
{
	// TODO INSTEAD OF INVALIDATING TICKET AFTER SOMETIME MAKE IT ACTIVE AS LONG AS THE GAME SESSION IS ACTIVE IF GAME IS OFF INVALIDATE IT
	// Create HTTP request.
	TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();
	
	UQwackSDKUtils::LoadSettings();
	
	HttpRequest->SetURL(TokenEndpoint);
	HttpRequest->SetVerb(TEXT("POST"));
	
	// Set request data as form-urlencoded.
	FString RequestData ;
	if(bPlayer)
	{
		// TODO 
		//RequestData = FString::Printf(TEXT("grant_type=password&client_id=%s&steamSessionTicket=%s"),*UQwackSDKUtils::GetQwackConfig()->KeyCloak_Client_ID,*SteamToken );
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("CLIENT ID : %s, Client Secret %s"),*UQwackSDKUtils::GetClientID(), *UQwackSDKUtils::GetClientSecret() );
		RequestData = FString::Printf(TEXT("grant_type=client_credentials&client_id=%s&client_secret=%s"), *UQwackSDKUtils::GetClientID(), *UQwackSDKUtils::GetClientSecret());
	}
	HttpRequest->SetContentAsString(RequestData);

	// Set Content-Type header.
	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/x-www-form-urlencoded"));

	// Set up completion callback using lambda function.
	HttpRequest->OnProcessRequestComplete().BindLambda([OnCompleteRequest](FHttpRequestPtr Req, const FHttpResponsePtr& Response, bool bWasSuccessful)
	{
		
		FQwackHTTPResponse QwackResponse;

		// Populate QwackResponse from Response
		if (bWasSuccessful && Response.IsValid())
		{
			QwackResponse.success = true;
			QwackResponse.StatusCode = Response->GetResponseCode();
			QwackResponse.FullText = Response->GetContentAsString();

			

			

#if WITH_SERVER_CODE
// Code specific to dedicated server
			// todo remove this
			/*FString FilePath = FPaths::ProjectLogDir() + TEXT("HttpResponse.txt");
			FFileHelper::SaveStringToFile(QwackResponse.FullText, *FilePath);*/
			if(IsRunningDedicatedServer())
			{
				/*UE_LOG(LogTemp, Warning, TEXT(" SERVER -> TOEKN : %s "),*UQwackSDKUtils::GetValueFromQwackJson(QwackResponse.FullText, "access_token") );*/
				/*UQwackSDKStateData::SetServerToken(UQwackSDKUtils::GetValueFromQwackJson(QwackResponse.FullText, "access_token"));
				UQwackSDKStateData::SetServerRefreshToken(UQwackSDKUtils::GetValueFromQwackJson(QwackResponse.FullText, "refresh_token"));*/
			}

#else
			/*UE_LOG(LogTemp, Warning, TEXT(" ELSE SERVER -> TOEKN : %s, "),*QwackResponse.FullText );*/
			UQwackSDKStateData::SetPlayerSteamToken(UQwackSDKUtils::GetValueFromQwackJson(QwackResponse.FullText, "access_token"));
			UQwackSDKStateData::SetPlayerRefreshToken(UQwackSDKUtils::GetValueFromQwackJson(QwackResponse.FullText, "refresh_token"));
#endif
		}
		else
		{
			/*UE_LOG(LogTemp, Warning, TEXT(" ELSE RESPONSE -> Failed "));*/
			QwackResponse.success = false;
			QwackResponse.StatusCode = Response->GetResponseCode();
			QwackResponse.FullText = Response->GetContentAsString();
		}

		// Call the provided completion callback with the response.
		//onCompleteRequest.ExecuteIfBound(QwackResponse);
	});

	// Send the request.
	HttpRequest->ProcessRequest();
}

