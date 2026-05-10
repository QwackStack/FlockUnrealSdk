// Copyright 2022, Qwack. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "JsonObjectConverter.h"
#include "Schemas.h"
#include "UObject/Object.h"
#include "Dom/JsonObject.h" // For FJsonObject
#include "Interfaces/IHttpRequest.h"
#include "Misc/Base64.h"     // For FBase64
#include "Qwack_ue_Sdk/Qwack_ue_Sdk.h"
#include "Qwack_ue_Sdk/Config/QwackConfig.h"
#include "Qwack_ue_Sdk/HTTPClient/SHTTPClient.h"
#include "Qwack_ue_Sdk/Endpoints/QwackGameEndpoints.h"
#include "Qwack_ue_Sdk/HTTPClient/HTTPResponse.h"
#include "Qwack_ue_Sdk/StateData/QwackSDKStateData.h"
#include "Serialization/JsonReader.h"  // For TJsonReader
#include "Serialization/JsonSerializer.h"  // For FJsonSerializer

#include "QwackSDKUtils.generated.h"

constexpr FQwackEmptyRequest QwackEmptyRequest; // A constant instance of an empty request struct

class UQwackConfig;
/**
 * 
 */
UCLASS()
class QWACK_UE_SDK_API UQwackSDKUtils : public UObject
{
	GENERATED_BODY()

public:
	/**
 * Converts an array of structs to a JSON string.
 * @param StructArray The array of structs to convert.
 * @return The JSON string representing the array of structs.
 */
	
	template <typename StructType>
	static FString ConvertStructArrayToJsonString(const TArray<StructType>& StructArray )
	{
		// Create a JSON Array
		TArray<TSharedPtr<FJsonValue>> JsonArray;

		// Iterate over each struct in the array
		for (const StructType& Item : StructArray)
		{
			// Convert the struct to a JSON object
			TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);
			TSharedRef<FJsonObject> JsonObjectRef = JsonObject.ToSharedRef(); // Convert TSharedPtr to TSharedRef

			if (FJsonObjectConverter::UStructToJsonObject(StructType::StaticStruct(), &Item, JsonObjectRef, 0, 0))
			{
				// Add the JSON object to the array
				JsonArray.Add(MakeShareable(new FJsonValueObject(JsonObject)));
			}
		}

		// Convert the JSON array to string
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(JsonArray, Writer);

		return OutputString;
	}
	
	
	static bool IsValidSteamTicket(const FString& Ticket);
	UFUNCTION(BlueprintCallable)
	static FString GetValueFromQwackJson(const FString& FullJsonResponse, const FString& Key);
	static void LogLongMessage(const FString& Message, int32 MaxLength);
	static void LoadSettings();
	static FString GetClientID();
	static FString GetClientSecret();

	UFUNCTION(BlueprintCallable)
	static FString DecodeJwtPayload(const FString& JwtToken);
	

	// Function to load JSON into a specific struct
	
	UFUNCTION(BlueprintCallable, Category = "QwackSDK SDK| Json Utilities |Weapons")

	static bool IsTokenExpired(const FString& JwtToken);
	static const UQwackConfig* GetQwackConfig();

	static bool ParseFlockEnvolopeResponse(const FString& FullText, struct FFLockApiEnvelope& OutResult);

private:
	static FString ClientID;
	static FString ClientSecret;


	static TSharedPtr<FJsonObject> ParseJwtPayloadToJson(const FString& JwtToken);
};

// Utility functions for Qwack SDK

namespace QwackUtilities
{
	// Check if a JSON string represents an empty JSON object
	/* speed optimization to avoid unnecessary parsing */
	static bool IsEmptyJson(const FString& JsonStr)
	{
		return JsonStr.Equals(FString("{}")) ||JsonStr.Equals(FString("{\r\n}")) || JsonStr.Equals(FString("{\n}")) ||JsonStr.Equals(FString("{ }"));	
	}
	template<typename RequestType>
	static FString UQStructToJsonString(RequestType RequestStruct)
	{
		FString ContentString;
#if ENGINE_MAJOR_VERSION <5
		FJsonObjectConverter::UStructToJsonObjectString(RequestType::StaticStruct(), &RequestStruct, ContentString, 0, 0);
		if(IsEmptyJson(ContentString))
		{
			ContentString = FString();
		}
#else
		if(!std::is_same_v<RequestType, FQwackEmptyRequest>)
		{
			FJsonObjectConverter::UStructToJsonObjectString(RequestType::StaticStruct(), &RequestStruct, ContentString, 0, 0);
		
		}
#endif
		return ContentString;
		
	}
}
	
template <typename ResponseType>
struct FlockApi
{
	DECLARE_DELEGATE_OneParam(FInspectorCallback, ResponseType& /*Response*/);
	template<typename BlueprintCallbackDelegate, typename CppDelegate>
	// Creates a combined response callback that handles both Blueprint and C++ delegates.
	/* The CreateLambda function takes three parameters:
	 *  - BlueprintCallback: A delegate for Blueprint callbacks.
	 *   - OnCompletedRequest: A delegate for C++ callbacks.
	 *   - ResponseCallback: The original response callback to be executed.
	 *    The function returns a new FQwackFlockResponse that, when executed, will:
	 *      - Parse the HTTP response into the specified ResponseType struct.
	 *      - Execute the original response callback with the parsed response.
	 *      - Execute the Blueprint callback with the parsed response.
	 *      - Execute the C++ delegate with the original HTTP response.
	 */
	static FQwackFlockResponse CreateLambda(
		const BlueprintCallbackDelegate& BlueprintCallback,
		const CppDelegate& OnCompletedRequest,
		const  FInspectorCallback& ResponseCallback)
	{
		FQwackFlockResponse Callback = FQwackFlockResponse::CreateLambda(
			[BlueprintCallback, OnCompletedRequest, ResponseCallback]
			( FQwackHTTPResponse HttpResponse)
		{
			ResponseType Response;
			if(!HttpResponse.FullText.IsEmpty())
			{
				FJsonObjectConverter::JsonObjectStringToUStruct<ResponseType>(HttpResponse.FullText, &Response, 0, 0); // convert json to struct
				
			}
			Response.StatusCode = HttpResponse.StatusCode;
			Response.success = HttpResponse.success;
			Response.FullText = HttpResponse.FullText;
			// Call the original response callback
			ResponseCallback.ExecuteIfBound(Response);
			// Call the Blueprint callback
			BlueprintCallback.ExecuteIfBound(Response);
			// Call the C++ delegate
			OnCompletedRequest.ExecuteIfBound(Response);
			
		
		});
		return Callback;
	}
	template<typename RequestType, typename BlueprintCallbackDelegate, typename CppDelegate>
	static void CallAPI(USHTTPClient* HttpClient,
		RequestType RequestStruct,
		FString& ContentString,
		FSQwackFlockEndpoints Endpoint,
		const TArray<FStringFormatArg>& InOrderedArguments,
		const TMultiMap<FString, FString>& QueryParams,
		const BlueprintCallbackDelegate& BlueprintCallback,
		const CppDelegate& OnCompletedRequest,
		const FInspectorCallback& ResponseCallback =
		FlockApi<ResponseType>::FInspectorCallback::CreateLambda([](ResponseType& ignored){}),
		TMap<FString , FString> CustomHeaders = TMap<FString, FString>())
	{
		FString Content = QwackUtilities::UQStructToJsonString<RequestType>(RequestStruct);
		FlockApi<ResponseType>::CallAPIUsingJson(
		HttpClient,
		Content,
		ContentString,
		Endpoint,
		InOrderedArguments,
		QueryParams,
		BlueprintCallback,
		OnCompletedRequest,
		ResponseCallback,
		CustomHeaders
	);

	}
	
	template< typename BlueprintCallbackDelegate, typename CppDelegate>
	static void CallAPIUsingJson(
		USHTTPClient* HttpClient,
		const FString& JsonString ,
		FString& ContentString,
		FSQwackFlockEndpoints Endpoint,
		const TArray<FStringFormatArg>& InOrderedArguments,
		const TMultiMap<FString, FString>& QueryParams,
		const BlueprintCallbackDelegate& BlueprintCallback,
		const CppDelegate& OnCompletedRequest,
		const FInspectorCallback& ResponseCallback =
		FlockApi<ResponseType>::FInspectorCallback::CreateLambda([](ResponseType& ignored){}),
		TMap<FString, FString> CustomHeaders = TMap<FString, FString>())
	{
		const UQwackConfig* Config = UQwackSDKUtils::GetQwackConfig();
		FString EndPointWithArgs = FString::Format(*Endpoint.EndPoint,/*domain, game, api keys? currently i have no idea what to put here*/  FStringFormatNamedArguments{{"APIKey", Config && !Config->QwackAPIKey.IsEmpty() ? Config->QwackAPIKey +".":""}}); // need to revise this @TODO
		EndPointWithArgs = FString::Format(*EndPointWithArgs, InOrderedArguments);
		
		
		if (QueryParams.Num() != 0)
		{
			FString Delimiter = TEXT("?");
			for (const TPair<FString, FString>& Param : QueryParams)
			{
				EndPointWithArgs
					.Append(Delimiter)
					.Append(Param.Key)
					.Append(TEXT("="))
					.Append(Param.Value);

				Delimiter = TEXT("&");
			}
		}
		const FString HttpRequestType =  UQwackFlockGameEndpoints::QwackHttpVerb(Endpoint.RequestType);
		// TODO FIX THE PRIVATE KEY DOMAIN KEY GAME KEY ? HOW ARE THE GUYS GOING TO SEND IT OR ACCEPT REQUESTS
		CustomHeaders.Add(TEXT("API_KEY?_SOMETHING_TODO_HERE?"), UQwackSDKStateData::GetToken());
#if WITH_EDITOR
		UE_LOG(LOG_FLOCK_GAME_SDK, Log, TEXT("Qwack SDK Making Request to Endpoint: %s"), *EndPointWithArgs);
#endif
		 FQwackFlockResponse Response = CreateLambda<BlueprintCallbackDelegate, CppDelegate>(BlueprintCallback, OnCompletedRequest, ResponseCallback);;
		HttpClient->SendRequest(EndPointWithArgs, HttpRequestType, JsonString, Response, CustomHeaders);
		

	}
};