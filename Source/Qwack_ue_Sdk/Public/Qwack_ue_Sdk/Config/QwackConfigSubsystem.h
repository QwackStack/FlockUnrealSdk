// Runtime-mutable façade over UQwackSettings. Each Set*Override value persists for the
// owning game instance — passing an empty string clears the override and reverts to the
// project setting. Feature subsystems read API URL / key / game id / game version through
// this layer instead of touching UQwackSettings directly.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "QwackConfigSubsystem.generated.h"

class UQwackSettings;

UCLASS()
class QWACK_UE_SDK_API UQwackConfigSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "Flock|Config")
	FString GetApiUrl() const;

	UFUNCTION(BlueprintCallable, Category = "Flock|Config")
	FString GetApiKey() const;

	UFUNCTION(BlueprintCallable, Category = "Flock|Config")
	FString GetGameId() const;

	UFUNCTION(BlueprintCallable, Category = "Flock|Config")
	FString GetGameVersion() const;

	UFUNCTION(BlueprintCallable, Category = "Flock|Config")
	void SetApiUrlOverride(const FString& InUrl);

	UFUNCTION(BlueprintCallable, Category = "Flock|Config")
	void SetApiKeyOverride(const FString& InKey);

	UFUNCTION(BlueprintCallable, Category = "Flock|Config")
	void SetGameIdOverride(const FString& InGameId);

	UFUNCTION(BlueprintCallable, Category = "Flock|Config")
	void SetGameVersionOverride(const FString& InVersion);

	UFUNCTION(BlueprintCallable, Category = "Flock|Config")
	void ClearOverrides();

	// Direct accessor for non-overridable settings (analytics toggles, fps interval, etc.).
	const UQwackSettings* GetSettings() const;

private:
	UPROPERTY(Transient) FString ApiUrlOverride;
	UPROPERTY(Transient) FString ApiKeyOverride;
	UPROPERTY(Transient) FString GameIdOverride;
	UPROPERTY(Transient) FString GameVersionOverride;
};
