// Copyright 2022, Qwack. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/Engine/Public/Subsystems/GameInstanceSubsystem.h"
#include "Qwack_ue_Sdk/Utils/Schemas.h"
#include "QwackFlockSubsystem.generated.h"



struct FFLockApiEnvelope;
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockOnAuthChanged, const FString&, AccessToken);

/**
 * 
 */
UCLASS()
class QWACK_UE_SDK_API UQwackFlockSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintCallable, Category="Flock|Auth")
	void SetAccessToken(const FString& InAccessToken);
	UFUNCTION(BlueprintCallable, Category="Flock|Auth")
	FString GetAccessToken() const { return AccessToken;}
	UPROPERTY(BlueprintAssignable, Category="Flock|Auth")
	FFlockOnAuthChanged  OnAccessTokenChanged;

	// -------------------- Achievement APIs -------------------- //
	

	DECLARE_DYNAMIC_DELEGATE_OneParam(FFlockAchievementSingleCallback, const FFlockAchievementSingleResponse&, Response);
	DECLARE_DYNAMIC_DELEGATE_OneParam(FFlockAchievementPageCallback, const FFlockAchievementPageResponse&, Response);
	DECLARE_DYNAMIC_DELEGATE_OneParam(FFlockOpCallback, const FFlockOpResult&, Result);

	UFUNCTION(BlueprintCallable, Category="Flock|Achievements")
	void CreateAchievement(const struct FFlockCreateAchievementRequest& Request, const FFlockAchievementSingleCallback& Callback);

	UFUNCTION(BlueprintCallable, Category="Flock|Achievements")
	void GetAchievementById(const FString& AchievementId, const FFlockAchievementSingleCallback& Callback);

	UFUNCTION(BlueprintCallable, Category="Flock|Achievements")
	void GetAchievements(const FString& GameId, int32 Page, int32 Limit, const FString& NameFilter, const FFlockAchievementPageCallback& Callback);

	UFUNCTION(BlueprintCallable, Category="Flock|Achievements")
	void UpdateAchievement(const FString& AchievementId, const struct FFlockUpdateAchievementRequest& Request, const FFlockAchievementSingleCallback& Callback);

	UFUNCTION(BlueprintCallable, Category="Flock|Achievements")
	void DeleteAchievement(const FString& AchievementId, const FFlockOpCallback& Callback);
	
private:
	UPROPERTY(Transient)
	TObjectPtr<class USHTTPClient> HttpClient;
	
	UPROPERTY(Transient)
	FString AccessToken;

	TMap<FString, FString> MakeAuthHeaders() const;
	static struct FFlockOpResult MakeMetaFromHttp(const struct FQwackHTTPResponse& R);
	static bool TryParseAchievementFromEnvelope(const FString& FullText, struct FFlockAchievement& OutAchievement, FString& OutResultJson, FString& OutError);
	static bool TryParseAchievementPage(const FString& FullText, struct FFlockAchievementPage& OutPage, FString& OutError);
	FORCEINLINE FFlockAchievementSingleResponse MakeErrorResult(int32 StatusCode, const FString& ErrorMessage)
	{
		FFlockAchievementSingleResponse Result;
		Result.Meta.bSuccess = false;
		Result.Meta.StatusCode = StatusCode;
		Result.Meta.ErrorMessage = ErrorMessage;
		return Result;
	}
	
};
