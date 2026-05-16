// One-node Blueprint access to the synchronous parts of the Flock SDK: auth token
// state, live session info, and the runtime config façade. Every function resolves
// the relevant GameInstance subsystem internally from the world context, so callers
// never have to fetch a subsystem first. Async endpoints live in the Flock*Async nodes.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Qwack_ue_Sdk/Utils/Schemas.h"
#include "FlockBlueprintLibrary.generated.h"

class UQwackFlockSubsystem;

// Multicast pin types for the Flock async action nodes ("On Success" / "On Failed").
// Declared here rather than in a delegate-only header because UE's UHT only registers
// delegate symbols from headers that also carry a generated body (this library's UCLASS).
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockGenericResultPin,      const FFlockGenericResponse&,      Response);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockAuthResultPin,         const FFlockPlayerAuthResponse&,   Response);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockAuthTestResultPin,     const FFlockAuthTestResponse&,     Response);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFlockSessionStartResultPin, const FFlockSessionStartResponse&, Response);

UCLASS()
class QWACK_UE_SDK_API UFlockBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	// Escape hatch for advanced graphs that want the subsystem directly. Most graphs
	// should use the dedicated nodes below or the Flock*Async action nodes.
	UFUNCTION(BlueprintPure, Category = "Flock", meta = (WorldContext = "WorldContextObject"))
	static UQwackFlockSubsystem* GetFlockSubsystem(const UObject* WorldContextObject);

	// ---------------- Auth state ----------------

	UFUNCTION(BlueprintCallable, Category = "Flock|Auth", meta = (WorldContext = "WorldContextObject"))
	static void FlockSetAccessToken(const UObject* WorldContextObject, const FString& AccessToken);

	UFUNCTION(BlueprintPure, Category = "Flock|Auth", meta = (WorldContext = "WorldContextObject"))
	static FString FlockGetAccessToken(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Flock|Auth", meta = (WorldContext = "WorldContextObject"))
	static FString FlockGetRefreshToken(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Flock|Auth", meta = (WorldContext = "WorldContextObject"))
	static FString FlockGetPlayerId(const UObject* WorldContextObject);

	// True once a non-empty access token is held (login / register / refresh succeeded).
	UFUNCTION(BlueprintPure, Category = "Flock|Auth", meta = (WorldContext = "WorldContextObject"))
	static bool FlockIsLoggedIn(const UObject* WorldContextObject);

	// ---------------- Session (read-only; lifecycle is automatic) ----------------

	UFUNCTION(BlueprintPure, Category = "Flock|Session", meta = (WorldContext = "WorldContextObject"))
	static bool FlockIsSessionActive(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Flock|Session", meta = (WorldContext = "WorldContextObject"))
	static FString FlockGetActiveSessionId(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Flock|Session", meta = (WorldContext = "WorldContextObject"))
	static int32 FlockGetScreensViewed(const UObject* WorldContextObject);

	// ---------------- Config façade ----------------

	UFUNCTION(BlueprintPure, Category = "Flock|Config", meta = (WorldContext = "WorldContextObject"))
	static FString FlockGetApiUrl(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Flock|Config", meta = (WorldContext = "WorldContextObject"))
	static FString FlockGetApiKey(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Flock|Config", meta = (WorldContext = "WorldContextObject"))
	static FString FlockGetGameId(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Flock|Config", meta = (WorldContext = "WorldContextObject"))
	static FString FlockGetGameVersion(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Flock|Config", meta = (WorldContext = "WorldContextObject"))
	static FString FlockGetGameVersionId(const UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "Flock|Config", meta = (WorldContext = "WorldContextObject"))
	static bool FlockIsGameVersionResolved(const UObject* WorldContextObject);

	// Pass an empty string to clear an override and revert to the project setting.
	UFUNCTION(BlueprintCallable, Category = "Flock|Config", meta = (WorldContext = "WorldContextObject"))
	static void FlockSetApiUrlOverride(const UObject* WorldContextObject, const FString& ApiUrl);

	UFUNCTION(BlueprintCallable, Category = "Flock|Config", meta = (WorldContext = "WorldContextObject"))
	static void FlockSetApiKeyOverride(const UObject* WorldContextObject, const FString& ApiKey);

	UFUNCTION(BlueprintCallable, Category = "Flock|Config", meta = (WorldContext = "WorldContextObject"))
	static void FlockSetGameIdOverride(const UObject* WorldContextObject, const FString& GameId);

	UFUNCTION(BlueprintCallable, Category = "Flock|Config", meta = (WorldContext = "WorldContextObject"))
	static void FlockSetGameVersionOverride(const UObject* WorldContextObject, const FString& GameVersion);

	UFUNCTION(BlueprintCallable, Category = "Flock|Config", meta = (WorldContext = "WorldContextObject"))
	static void FlockClearConfigOverrides(const UObject* WorldContextObject);

	// ---------------- Convenience ----------------

	// Builds a JSON object string from a string map, so designers can fill the
	// "extra data" / "properties" inputs without hand-writing JSON. Values are
	// emitted as JSON strings; pass a raw JSON string directly if you need
	// numbers, bools, or nested objects.
	UFUNCTION(BlueprintPure, Category = "Flock|Util")
	static FString MakeFlockJsonObject(const TMap<FString, FString>& Fields);
};
