// Shared HTTP plumbing for the Flock SDK. Owns the USHTTPClient, builds URLs from
// UQwackConfigSubsystem, attaches headers (including the bearer pulled from
// UQwackAuthSubsystem at send time), dispatches requests, and converts raw HTTP
// responses to FFlockOpResult metadata. Feature subsystems compose against it.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Qwack_ue_Sdk/Utils/Schemas.h"
#include "QwackFlockGameSubsystem.generated.h"

class USHTTPClient;
struct FSQwackFlockEndpoints;
struct FQwackHTTPResponse;

UCLASS()
class QWACK_UE_SDK_API UQwackFlockGameSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	FString BuildUrl(const FString& Path) const;
	TMap<FString, FString> MakeHeaders(bool bIncludeAuth) const;

	// Sends an HTTP request and routes the FQwackHTTPResponse into OnDone. If
	// UrlOverride is non-empty it is used as-is; otherwise BuildUrl(Endpoint.EndPoint)
	// supplies the URL. Auth is pulled from UQwackAuthSubsystem lazily so token
	// rotation is reflected without per-feature plumbing.
	void Send(const FSQwackFlockEndpoints& Endpoint,
	          const FString& UrlOverride,
	          const FString& Body,
	          bool bIncludeAuth,
	          TFunction<void(const FQwackHTTPResponse&)> OnDone) const;

	static FFlockOpResult MakeMeta(const FQwackHTTPResponse& R);

private:
	UPROPERTY(Transient) TObjectPtr<USHTTPClient> HttpClient;
};
