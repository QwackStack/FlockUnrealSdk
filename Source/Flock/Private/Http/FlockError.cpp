// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Http/FlockError.h"
#include "Http/FlockErrorHints.h"
#include "UObject/Class.h"

FFlockError FFlockError::Make(EFlockErrorType InType, const FString& InMessage, int32 InStatusCode,
	const FString& InBody, const FString& InCode, const FString& InServerMessage)
{
	FFlockError Error;
	Error.Type = InType;
	Error.Message = InMessage;
	Error.StatusCode = InStatusCode;
	Error.Body = InBody;
	Error.Code = InCode;
	Error.ErrorCode = FFlockErrorCodes::Parse(InCode);
	Error.ServerMessage = InServerMessage;
	// One place: every coded failure gets its remedy here, whichever layer built the error.
	Error.Hint = FFlockErrorHints::For(Error.ErrorCode);
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

FString FFlockError::ToDisplayText() const
{
	FString Text;

	if (!Operation.IsEmpty())
	{
		Text += Operation + TEXT(" failed: ");
	}

	// The server's reason beats our generic "Validation failed" whenever the body carried one.
	const bool bFromServer = !ServerMessage.IsEmpty();
	Text += bFromServer ? ServerMessage : Message;

	// Bounded, low-cardinality identifiers only, so error-tracker grouping stays meaningful. The status
	// is only added alongside a server reason: the terse Message already spells it out.
	const bool bTagStatus = bFromServer && StatusCode > 0;
	if (!Code.IsEmpty())
	{
		Text += bTagStatus
			? FString::Printf(TEXT(" [%s, HTTP %d]"), *Code, StatusCode)
			: FString::Printf(TEXT(" [%s]"), *Code);
	}
	else if (bTagStatus)
	{
		Text += FString::Printf(TEXT(" [HTTP %d]"), StatusCode);
	}

	if (!Hint.IsEmpty())
	{
		Text += TEXT("\nFix: ") + Hint;
	}
	return Text;
}

bool FFlockError::IsAlreadyRegistered() const
{
	switch (ErrorCode)
	{
	case EFlockErrorCode::PlayerEmailAlreadyRegistered:
	case EFlockErrorCode::PlayerDeviceAlreadyRegistered:
	case EFlockErrorCode::PlayerGoogleAccountAlreadyRegistered:
	case EFlockErrorCode::PlayerAppleAccountAlreadyRegistered:
	case EFlockErrorCode::PlayerSteamAccountAlreadyRegistered:
		return true;
	default:
		return false;
	}
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
