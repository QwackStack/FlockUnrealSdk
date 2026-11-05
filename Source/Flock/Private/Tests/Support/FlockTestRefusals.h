// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

#include "Http/FlockErrorCode.h"
#include "Http/FlockErrorHints.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockResult.h"
#include "Misc/AutomationTest.h"
#include "Models/FlockGameModels.h"
#include "Tests/Support/FlockFakeTransport.h"

/** The one set of checks every table of server refusals runs: each body parses to its code, its member, its fix. */
namespace FlockTestRefusals
{
	/** The `detail.code` string of a coded body, or empty when it carries none. */
	inline FString CodeIn(const FString& Body)
	{
		const FString Marker = TEXT("\"code\":\"");
		const int32 Start = Body.Find(Marker, ESearchCase::CaseSensitive);
		if (Start == INDEX_NONE)
		{
			return FString();
		}
		const int32 From = Start + Marker.Len();
		const int32 End = Body.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
		return End == INDEX_NONE ? FString() : Body.Mid(From, End - From);
	}

	/**
	 * Answers one request with Body under Status and checks the error the client builds: the raw code kept, its own member,
	 * the status, the error type for that status, the server's reason, and its member's fix. Label names the row in failures.
	 */
	inline void CheckRefusal(FAutomationTestBase& Test, FFlockFakeTransport& Fake, FFlockHttpClient& Client, const FString& Label,
		int32 Status, EFlockErrorType Type, EFlockErrorCode Member, const TCHAR* Body)
	{
		Fake.On(TEXT("refusal"), FFlockFakeTransport::Status(Status, Body));
		bool bAnswered = false;
		FFlockError Error;
		Client.Get<FFlockGameVersionSchema>(TEXT("http://x/refusal"), {},
			[&bAnswered, &Error](TFlockResult<FFlockGameVersionSchema> Result) { bAnswered = true; Error = Result.Error; });

		if (!Test.TestTrue(FString::Printf(TEXT("%s answered"), *Label), bAnswered))
		{
			return;
		}
		Test.TestTrue(FString::Printf(TEXT("%s: raw code kept"), *Label), Error.Code.Equals(CodeIn(Body), ESearchCase::CaseSensitive));
		Test.TestEqual(FString::Printf(TEXT("%s: its own member"), *Label), static_cast<int32>(Error.ErrorCode), static_cast<int32>(Member));
		Test.TestEqual(FString::Printf(TEXT("%s: status kept"), *Label), Error.StatusCode, Status);
		Test.TestEqual(FString::Printf(TEXT("%s: error type for its status"), *Label), static_cast<int32>(Error.Type), static_cast<int32>(Type));
		Test.TestFalse(FString::Printf(TEXT("%s: the server's reason survives"), *Label), Error.ServerMessage.IsEmpty());
		Test.TestFalse(FString::Printf(TEXT("%s: carries a hint"), *Label), Error.Hint.IsEmpty());
		Test.TestTrue(FString::Printf(TEXT("%s: the hint is its code's"), *Label),
			Error.Hint.Equals(FFlockErrorHints::For(Member), ESearchCase::CaseSensitive));
	}
}

#endif // WITH_AUTOMATION_TESTS
