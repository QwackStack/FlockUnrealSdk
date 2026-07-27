// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockError.h"
#include "Http/FlockResult.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Models/FlockPlayerModels.h"
#include "FlockPlayerAsyncActions.generated.h"

/**
 * Blueprint async nodes for player data, templates, and bans. Each resolves the SDK from its world
 * context on Activate and fires exactly one pin; when the SDK is unavailable the failure pin fires with a
 * Validation error. Wider than the (pre-codegen) public C++ intent on purpose, matching the config/game
 * and shop nodes — a graph needs the by-name/by-tag reads to build player UI.
 *
 * An empty Player Id means "the signed-in player" on Get Player Ban; on Get All Player Data it means "all
 * players" (no filter). Read a row/template's flattened data off its handle with the game-config-data
 * library nodes. Get My Data By Template/Tag returns an empty record (empty Id) when the player has no
 * row for that template.
 */

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockPlayerDataPin, const FFlockPlayerData&, Data, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockPlayerDataPagePin, const FFlockPlayerDataPage&, Page, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockPlayerDataListPin, const TArray<FFlockPlayerData>&, Rows, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockPlayerTemplatePin, const FFlockPlayerTemplateSchema&, Template, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockPlayerTemplateListPin, const TArray<FFlockPlayerTemplateSchema>&, Templates, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockPlayerBanPin, const FFlockPlayerBan&, Ban, const FFlockError&, Error);

/** Fetches a single player-data row by its id. */
UCLASS()
class FLOCK_API UFlockGetPlayerDataAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockPlayerDataPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockPlayerDataPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Player Data By Id"), Category = "Flock|Player")
	static UFlockGetPlayerDataAction* GetPlayerDataById(UObject* WorldContextObject, const FString& PlayerDataId);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockPlayerData>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString PlayerDataId;
};

/** Fetches a page of player-data rows (empty player id = all players). */
UCLASS()
class FLOCK_API UFlockGetAllPlayerDataAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockPlayerDataPagePin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockPlayerDataPagePin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get All Player Data"), Category = "Flock|Player")
	static UFlockGetAllPlayerDataAction* GetAllPlayerData(UObject* WorldContextObject, const FString& PlayerId,
		int32 Page = 1, int32 Limit = 100);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockPlayerDataPage>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString PlayerId;
	int32 Page = 1;
	int32 Limit = 100;
};

/** Resolves the signed-in player's row for a template (by template id or by tag). */
UCLASS()
class FLOCK_API UFlockGetMyPlayerDataAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockPlayerDataPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockPlayerDataPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get My Data By Template"), Category = "Flock|Player")
	static UFlockGetMyPlayerDataAction* GetMyDataByTemplate(UObject* WorldContextObject, const FString& PlayerTemplateId);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get My Data By Tag"), Category = "Flock|Player")
	static UFlockGetMyPlayerDataAction* GetMyDataByTag(UObject* WorldContextObject, const FString& Tag);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockPlayerData>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	bool bByTag = false;
	FString Argument;
};

/** Fetches all player templates for this game version. */
UCLASS()
class FLOCK_API UFlockGetPlayerTemplatesAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockPlayerTemplateListPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockPlayerTemplateListPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Player Templates"), Category = "Flock|Player")
	static UFlockGetPlayerTemplatesAction* GetPlayerTemplates(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<TArray<FFlockPlayerTemplateSchema>>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;
};

/** Fetches a single player template by id, by name, or by tag. */
UCLASS()
class FLOCK_API UFlockGetPlayerTemplateAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockPlayerTemplatePin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockPlayerTemplatePin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Player Template By Id"), Category = "Flock|Player")
	static UFlockGetPlayerTemplateAction* GetPlayerTemplateById(UObject* WorldContextObject, const FString& PlayerTemplateId);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Player Template By Name"), Category = "Flock|Player")
	static UFlockGetPlayerTemplateAction* GetPlayerTemplateByName(UObject* WorldContextObject, const FString& Name);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Player Template By Tag"), Category = "Flock|Player")
	static UFlockGetPlayerTemplateAction* GetPlayerTemplateByTag(UObject* WorldContextObject, const FString& Tag);

	virtual void Activate() override;

private:
	// How Argument is looked up. Kept off the wire — a plain internal selector for the three factories.
	enum class ELookup : uint8 { ById, ByName, ByTag };

	void Complete(const TFlockResult<FFlockPlayerTemplateSchema>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	ELookup Mode = ELookup::ById;
	FString Argument;
};

/** Fetches all data rows for a template (across players). */
UCLASS()
class FLOCK_API UFlockGetTemplatePlayerDataAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockPlayerDataListPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockPlayerDataListPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Template Player Data"), Category = "Flock|Player")
	static UFlockGetTemplatePlayerDataAction* GetTemplatePlayerData(UObject* WorldContextObject, const FString& PlayerTemplateId);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<TArray<FFlockPlayerData>>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString PlayerTemplateId;
};

/** Fetches a player's ban record (empty player id = the signed-in player). */
UCLASS()
class FLOCK_API UFlockGetPlayerBanAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockPlayerBanPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockPlayerBanPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Player Ban"), Category = "Flock|Player")
	static UFlockGetPlayerBanAction* GetPlayerBan(UObject* WorldContextObject, const FString& PlayerId);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockPlayerBan>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString PlayerId;
};
