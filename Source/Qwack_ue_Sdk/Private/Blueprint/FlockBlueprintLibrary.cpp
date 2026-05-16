#include "Qwack_ue_Sdk/Blueprint/FlockBlueprintLibrary.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "Qwack_ue_Sdk/Config/QwackConfigSubsystem.h"
#include "Qwack_ue_Sdk/GameAPI/QwackFlockSubsystem.h"
#include "Qwack_ue_Sdk/Session/QwackSessionSubsystem.h"

namespace
{
	// Resolves any GameInstance subsystem from a Blueprint world-context object.
	// Returns null (rather than asserting) so callers degrade gracefully when
	// invoked from a context with no live game instance.
	template <typename T>
	T* ResolveSubsystem(const UObject* WorldContextObject)
	{
		if (!WorldContextObject || !GEngine)
		{
			return nullptr;
		}
		if (const UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull))
		{
			if (UGameInstance* GI = World->GetGameInstance())
			{
				return GI->GetSubsystem<T>();
			}
		}
		return nullptr;
	}
}

UQwackFlockSubsystem* UFlockBlueprintLibrary::GetFlockSubsystem(const UObject* WorldContextObject)
{
	return ResolveSubsystem<UQwackFlockSubsystem>(WorldContextObject);
}

// ---------------- Auth state ----------------

void UFlockBlueprintLibrary::FlockSetAccessToken(const UObject* WorldContextObject, const FString& AccessToken)
{
	if (UQwackFlockSubsystem* S = ResolveSubsystem<UQwackFlockSubsystem>(WorldContextObject))
	{
		S->SetAccessToken(AccessToken);
	}
}

FString UFlockBlueprintLibrary::FlockGetAccessToken(const UObject* WorldContextObject)
{
	const UQwackFlockSubsystem* S = ResolveSubsystem<UQwackFlockSubsystem>(WorldContextObject);
	return S ? S->GetAccessToken() : FString();
}

FString UFlockBlueprintLibrary::FlockGetRefreshToken(const UObject* WorldContextObject)
{
	const UQwackFlockSubsystem* S = ResolveSubsystem<UQwackFlockSubsystem>(WorldContextObject);
	return S ? S->GetRefreshToken() : FString();
}

FString UFlockBlueprintLibrary::FlockGetPlayerId(const UObject* WorldContextObject)
{
	const UQwackFlockSubsystem* S = ResolveSubsystem<UQwackFlockSubsystem>(WorldContextObject);
	return S ? S->GetPlayerId() : FString();
}

bool UFlockBlueprintLibrary::FlockIsLoggedIn(const UObject* WorldContextObject)
{
	const UQwackFlockSubsystem* S = ResolveSubsystem<UQwackFlockSubsystem>(WorldContextObject);
	return S ? !S->GetAccessToken().IsEmpty() : false;
}

// ---------------- Session ----------------

bool UFlockBlueprintLibrary::FlockIsSessionActive(const UObject* WorldContextObject)
{
	const UQwackSessionSubsystem* S = ResolveSubsystem<UQwackSessionSubsystem>(WorldContextObject);
	return S ? S->IsSessionActive() : false;
}

FString UFlockBlueprintLibrary::FlockGetActiveSessionId(const UObject* WorldContextObject)
{
	const UQwackSessionSubsystem* S = ResolveSubsystem<UQwackSessionSubsystem>(WorldContextObject);
	return S ? S->GetActiveSessionId() : FString();
}

int32 UFlockBlueprintLibrary::FlockGetScreensViewed(const UObject* WorldContextObject)
{
	const UQwackSessionSubsystem* S = ResolveSubsystem<UQwackSessionSubsystem>(WorldContextObject);
	return S ? S->GetScreensViewed() : 0;
}

// ---------------- Config ----------------

FString UFlockBlueprintLibrary::FlockGetApiUrl(const UObject* WorldContextObject)
{
	const UQwackConfigSubsystem* S = ResolveSubsystem<UQwackConfigSubsystem>(WorldContextObject);
	return S ? S->GetApiUrl() : FString();
}

FString UFlockBlueprintLibrary::FlockGetApiKey(const UObject* WorldContextObject)
{
	const UQwackConfigSubsystem* S = ResolveSubsystem<UQwackConfigSubsystem>(WorldContextObject);
	return S ? S->GetApiKey() : FString();
}

FString UFlockBlueprintLibrary::FlockGetGameId(const UObject* WorldContextObject)
{
	const UQwackConfigSubsystem* S = ResolveSubsystem<UQwackConfigSubsystem>(WorldContextObject);
	return S ? S->GetGameId() : FString();
}

FString UFlockBlueprintLibrary::FlockGetGameVersion(const UObject* WorldContextObject)
{
	const UQwackConfigSubsystem* S = ResolveSubsystem<UQwackConfigSubsystem>(WorldContextObject);
	return S ? S->GetGameVersion() : FString();
}

FString UFlockBlueprintLibrary::FlockGetGameVersionId(const UObject* WorldContextObject)
{
	const UQwackConfigSubsystem* S = ResolveSubsystem<UQwackConfigSubsystem>(WorldContextObject);
	return S ? S->GetGameVersionId() : FString();
}

bool UFlockBlueprintLibrary::FlockIsGameVersionResolved(const UObject* WorldContextObject)
{
	const UQwackConfigSubsystem* S = ResolveSubsystem<UQwackConfigSubsystem>(WorldContextObject);
	return S ? S->IsGameVersionResolved() : false;
}

void UFlockBlueprintLibrary::FlockSetApiUrlOverride(const UObject* WorldContextObject, const FString& ApiUrl)
{
	if (UQwackConfigSubsystem* S = ResolveSubsystem<UQwackConfigSubsystem>(WorldContextObject))
	{
		S->SetApiUrlOverride(ApiUrl);
	}
}

void UFlockBlueprintLibrary::FlockSetApiKeyOverride(const UObject* WorldContextObject, const FString& ApiKey)
{
	if (UQwackConfigSubsystem* S = ResolveSubsystem<UQwackConfigSubsystem>(WorldContextObject))
	{
		S->SetApiKeyOverride(ApiKey);
	}
}

void UFlockBlueprintLibrary::FlockSetGameIdOverride(const UObject* WorldContextObject, const FString& GameId)
{
	if (UQwackConfigSubsystem* S = ResolveSubsystem<UQwackConfigSubsystem>(WorldContextObject))
	{
		S->SetGameIdOverride(GameId);
	}
}

void UFlockBlueprintLibrary::FlockSetGameVersionOverride(const UObject* WorldContextObject, const FString& GameVersion)
{
	if (UQwackConfigSubsystem* S = ResolveSubsystem<UQwackConfigSubsystem>(WorldContextObject))
	{
		S->SetGameVersionOverride(GameVersion);
	}
}

void UFlockBlueprintLibrary::FlockClearConfigOverrides(const UObject* WorldContextObject)
{
	if (UQwackConfigSubsystem* S = ResolveSubsystem<UQwackConfigSubsystem>(WorldContextObject))
	{
		S->ClearOverrides();
	}
}

// ---------------- Convenience ----------------

FString UFlockBlueprintLibrary::MakeFlockJsonObject(const TMap<FString, FString>& Fields)
{
	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : Fields)
	{
		Obj->SetStringField(Pair.Key, Pair.Value);
	}
	FString Out;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Obj, Writer);
	return Out;
}
