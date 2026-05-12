#include "QwackConfigSubsystem.h"
#include "QwackSettings.h"

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
	GameVersionOverride = InVersion;
}

void UQwackConfigSubsystem::ClearOverrides()
{
	ApiUrlOverride.Reset();
	ApiKeyOverride.Reset();
	GameIdOverride.Reset();
	GameVersionOverride.Reset();
}
