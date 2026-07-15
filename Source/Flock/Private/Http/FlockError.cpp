// Copyright 2022, Qwacks. All Rights Reserved.

#include "Http/FlockError.h"
#include "UObject/Class.h"

FFlockError FFlockError::Make(EFlockErrorType InType, const FString& InMessage, int32 InStatusCode,
	const FString& InBody, const FString& InCode)
{
	FFlockError Error;
	Error.Type = InType;
	Error.Message = InMessage;
	Error.StatusCode = InStatusCode;
	Error.Body = InBody;
	Error.Code = InCode;
	Error.ErrorCode = FFlockErrorCodes::Parse(InCode);
	return Error;
}

FString FFlockError::ToString() const
{
	const UEnum* TypeEnum = StaticEnum<EFlockErrorType>();
	const FString TypeName = TypeEnum ? TypeEnum->GetNameStringByValue(static_cast<int64>(Type)) : TEXT("Error");

	FString Text = StatusCode > 0
		? FString::Printf(TEXT("[%s] %s (HTTP %d)"), *TypeName, *Message, StatusCode)
		: FString::Printf(TEXT("[%s] %s"), *TypeName, *Message);

	if (!Body.IsEmpty())
	{
		Text += FString::Printf(TEXT("\nResponse body: %s"), *Body);
	}
	return Text;
}

bool FFlockError::IsPermanentStatus(int32 InStatusCode)
{
	if (InStatusCode == 408 || InStatusCode == 429)
	{
		return false;
	}
	return InStatusCode >= 400 && InStatusCode < 500;
}

bool FFlockError::IsNotProcessed(int32 InStatusCode)
{
	return InStatusCode == 408 || InStatusCode == 429;
}
