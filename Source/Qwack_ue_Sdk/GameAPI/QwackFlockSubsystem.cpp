// Copyright 2022, Qwack. All Rights Reserved.


#include "QwackFlockSubsystem.h"

#include "Qwack_ue_Sdk/Endpoints/QwackGameEndpoints.h"
#include "Qwack_ue_Sdk/HTTPClient/HTTPResponse.h"
#include "Qwack_ue_Sdk/HTTPClient/SHTTPClient.h"
#include "Qwack_ue_Sdk/Utils/QwackSDKUtils.h"
#include "Qwack_ue_Sdk/Utils/Schemas.h"

void UQwackFlockSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	HttpClient = NewObject<USHTTPClient>(this);
}

void UQwackFlockSubsystem::Deinitialize()
{
	Super::Deinitialize();
	HttpClient = nullptr;
}

void UQwackFlockSubsystem::SetAccessToken(const FString& InAccessToken)
{
}

void UQwackFlockSubsystem::CreateAchievement(const FFlockCreateAchievementRequest& Request,
	const FFlockAchievementSingleCallback& Callback)
{
	//
	if(!HttpClient)
	{
		// to do inst
	
		Callback.ExecuteIfBound(MakeErrorResult(500, TEXT("HTTP Client not initialized.")));
		return;
	}
	if(AccessToken.IsEmpty())
	{
		Callback.ExecuteIfBound(MakeErrorResult(500, TEXT("Access Token is empty.")));
		return;
	}
	
}

void UQwackFlockSubsystem::GetAchievementById(
	const FString& AchievementId,
	const FFlockAchievementSingleCallback& Callback)
{
	FString ContentString;

	TArray<FStringFormatArg> Args; // empty unless endpoint needs formatting args

	TMultiMap<FString, FString> QueryParams;
	QueryParams.Add(TEXT("achievement_id"), AchievementId);

	FlockApi<FFlockAchievementSingleResponse>::CallAPI(
		HttpClient,
		FQwackEmptyRequest{},                               // RequestStruct
		ContentString,                                      // ContentString (unused for GET)
		UQwackFlockGameEndpoints::GetAchievementByID,        // Endpoint
		Args,                                               // Ordered args
		QueryParams,                                        // Query params
		Callback,                                           // BP callback
		Callback                                            // C++ callback (same type for now)
	);
}

void UQwackFlockSubsystem::GetAchievements(const FString& GameId, int32 Page, int32 Limit, const FString& NameFilter,
	const FFlockAchievementPageCallback& Callback)
{
}

void UQwackFlockSubsystem::UpdateAchievement(const FString& AchievementId,
	const FFlockUpdateAchievementRequest& Request, const FFlockAchievementSingleCallback& Callback)
{
}

void UQwackFlockSubsystem::DeleteAchievement(const FString& AchievementId, const FFlockOpCallback& Callback)
{
}

TMap<FString, FString> UQwackFlockSubsystem::MakeAuthHeaders() const
{
	TMap<FString, FString> Headers;
	if (!AccessToken.IsEmpty())
	{
		Headers.Add(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AccessToken));
	}
	return Headers;
}

FFlockOpResult UQwackFlockSubsystem::MakeMetaFromHttp(const FQwackHTTPResponse& R)
{
	FFlockOpResult Meta;
	Meta.StatusCode = R.StatusCode;
	Meta.bSuccess = R.success;
	if (!R.success)
	{
		Meta.ErrorMessage = R.FullText;
	}
	
	return Meta;
}

bool UQwackFlockSubsystem::TryParseAchievementFromEnvelope(
	const FString& FullText, FFlockAchievement& OutAchievement,
	FString& OutResultJson, FString& OutError)
{
	FFLockApiEnvelope Env;
	if(!UQwackSDKUtils::ParseFlockEnvolopeResponse(FullText, Env))
	{
		OutError = TEXT("Failed to parse Flock API envelope.");
		return false;
	}
	OutResultJson = Env.result_json;

	TSharedPtr<FJsonObject> ResultObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Env.result_json);
	if (!FJsonSerializer::Deserialize(Reader, ResultObj) || !ResultObj.IsValid())
	{
		OutError = TEXT("Failed to parse result JSON.");
		return false;
	}
	if(!FJsonObjectConverter::JsonObjectToUStruct<FFlockAchievement>(ResultObj.ToSharedRef(), &OutAchievement, 0, 0))
	{
		OutError = TEXT("Failed to convert JSON to FFlockAchievement struct.");
		return false;
	}
	return true;
}

bool UQwackFlockSubsystem::TryParseAchievementPage(const FString& FullText, FFlockAchievementPage& OutPage,
	FString& OutError)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FullText);
	if(!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("Failed to parse achievement page JSON.");
		return false;
	}
	if(!FJsonObjectConverter::JsonObjectToUStruct<FFlockAchievementPage>(Root.ToSharedRef(), &OutPage, 0, 0))
	{
		OutError = TEXT("Failed to convert JSON to FFlockAchievementPage struct.");
		return false;
	}
	return true;
}

// ---------------- Achievements ----------------
