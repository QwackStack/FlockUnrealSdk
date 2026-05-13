// Runtime-mutable façade over UQwackSettings. Each Set*Override value persists for the
// owning game instance — passing an empty string clears the override and reverts to the
// project setting. Feature subsystems read API URL / key / game id / game version through
// this layer instead of touching UQwackSettings directly.
//
// Also owns the resolved X-Game-Version-ID UUID: the configured GameVersion is a
// human-readable name (e.g. "0.1.2"); the server expects the matching UUID on the
// X-Game-Version-ID header. ResolveGameVersionId hits /v1/game_version/by-name/{name}
// to translate name -> UUID. The lookup runs lazily — triggered by the first Send
// that needs it — and downstream Sends queue via OnGameVersionResolved until it lands.

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
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintCallable, Category = "Flock|Config")
	FString GetApiUrl() const;

	UFUNCTION(BlueprintCallable, Category = "Flock|Config")
	FString GetApiKey() const;

	UFUNCTION(BlueprintCallable, Category = "Flock|Config")
	FString GetGameId() const;

	// Human-readable version name (e.g. "0.1.2"). Used only as the lookup key
	// against /v1/game_version/by-name/{name}; the server wants the UUID on
	// X-Game-Version-ID — call GetGameVersionId() for that.
	UFUNCTION(BlueprintCallable, Category = "Flock|Config")
	FString GetGameVersion() const;

	// Resolved UUID for the configured game version, or an empty string until
	// the by-name lookup completes. Use IsGameVersionResolved() to gate on it.
	UFUNCTION(BlueprintCallable, Category = "Flock|Config")
	FString GetGameVersionId() const { return ResolvedGameVersionId; }

	UFUNCTION(BlueprintCallable, Category = "Flock|Config")
	bool IsGameVersionResolved() const { return !ResolvedGameVersionId.IsEmpty(); }

	// True once the by-name lookup has run, regardless of outcome. Send() uses
	// this as a "stop gating" signal so a failed resolve doesn't cause every
	// subsequent request to retry the lookup forever.
	bool WasGameVersionResolveAttempted() const { return bGameVersionResolveAttempted; }

	// Triggers the by-name -> UUID lookup if not already resolved or in flight.
	// Safe to call any number of times; only the first call fires HTTP.
	void EnsureGameVersionResolved();

	// Registers Callback for the resolution result. Fires immediately with
	// bSuccess=true if the UUID is already cached. The callback receives true
	// only when a UUID was successfully fetched and stored.
	void OnGameVersionResolved(TFunction<void(bool /*bSuccess*/)> Callback);

	UFUNCTION(BlueprintCallable, Category = "Flock|Config")
	void SetApiUrlOverride(const FString& InUrl);

	UFUNCTION(BlueprintCallable, Category = "Flock|Config")
	void SetApiKeyOverride(const FString& InKey);

	UFUNCTION(BlueprintCallable, Category = "Flock|Config")
	void SetGameIdOverride(const FString& InGameId);

	// Changing the version name invalidates any cached UUID — the next Send
	// that needs the header will trigger a fresh by-name lookup.
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

	// Set after a successful by-name lookup. Cleared whenever the version name changes.
	UPROPERTY(Transient) FString ResolvedGameVersionId;

	bool bGameVersionResolveInFlight = false;
	bool bGameVersionResolveAttempted = false;
	TArray<TFunction<void(bool)>> PendingResolveCallbacks;

	void StartGameVersionResolve();
	void FinishGameVersionResolve(bool bSuccess, const FString& InResolvedId);
};
