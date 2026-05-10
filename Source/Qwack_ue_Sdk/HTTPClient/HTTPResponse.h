// Copyright 2022, Qwack. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "HTTPResponse.generated.h"

/**
 * 
 */
USTRUCT(BlueprintType)
struct FQwackHTTPResponse
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "QwackStorm")
	bool success = false;
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "QwackStorm")
	int StatusCode = 0;
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "QwackStorm")
	FString FullText; // in schema format

};


DECLARE_DYNAMIC_DELEGATE_OneParam(FResponseCallbackBP, FQwackHTTPResponse, Response);
DECLARE_DYNAMIC_DELEGATE_OneParam(FQwackResponseCallback, FQwackHTTPResponse, Response);
DECLARE_DELEGATE_OneParam(FQwackFlockResponse, FQwackHTTPResponse);