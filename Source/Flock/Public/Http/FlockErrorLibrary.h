// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Http/FlockError.h"
#include "FlockErrorLibrary.generated.h"

/**
 * Blueprint access to FFlockError's derived views. The struct's fields (Type, Message, StatusCode,
 * Code, ErrorCode, ServerMessage, ...) break out directly in Blueprint; these expose what C++ gets as
 * FFlockError member functions, which Blueprint can't call on a USTRUCT.
 */
UCLASS()
class FLOCK_API UFlockErrorLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Log-friendly text: [Type] Message (HTTP status), plus the raw server body when present. */
	UFUNCTION(BlueprintPure, Category = "Flock|Error", meta = (DisplayName = "To String (Flock Error)"))
	static FString ToDisplayString(const FFlockError& Error);

	/**
	 * True when a register/login route reports this identity (email/device/OAuth) already belongs to an
	 * account. Excludes a taken display name — that's a different fix.
	 */
	UFUNCTION(BlueprintPure, Category = "Flock|Error")
	static bool IsAlreadyRegistered(const FFlockError& Error);
};
