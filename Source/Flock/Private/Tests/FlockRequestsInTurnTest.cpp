// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"
#include "Misc/FlockEngineCompat.h"

#if WITH_AUTOMATION_TESTS

#include "Http/FlockRequestsInTurn.h"

namespace FlockRequestsInTurnTestHelpers
{
	/** A request on its way that records whether it was cancelled. */
	struct FRequestOnItsWay
	{
		TSharedRef<FFlockCancelToken> Token = MakeShared<FFlockCancelToken>();
		TSharedRef<bool> Cancelled = MakeShared<bool>(false);

		FRequestOnItsWay()
		{
			const TSharedRef<bool> Flag = Cancelled;
			Token->Canceller = [Flag]() { *Flag = true; };
		}

		FFlockRequestHandle Handle() const { return FFlockRequestHandle(Token); }
	};
}

using namespace FlockRequestsInTurnTestHelpers;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRequestsInTurnOnlyHandleTest, "Flock.Http.RequestsInTurn.ACancelReachesTheRequestOnItsWayOnceNothingElseHoldsTheCall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FFlockRequestsInTurnOnlyHandleTest::RunTest(const FString& Parameters)
{
	// A call whose last request's completion holds nothing of the call: only the game's handle is left.
	const FRequestOnItsWay Request;
	FFlockRequestHandle Handle;
	{
		const TSharedRef<FFlockRequestsInTurn> InTurn = FFlockRequestsInTurn::Make();
		InTurn->Send([&Request]() { return Request.Handle(); });
		Handle = InTurn->GetHandle();
	}
	Handle.Cancel();
	TestTrue(TEXT("the request on its way was cancelled"), *Request.Cancelled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRequestsInTurnNewestTest, "Flock.Http.RequestsInTurn.ACancelReachesTheNewestRequestNotOneAlreadyAnswered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FFlockRequestsInTurnNewestTest::RunTest(const FString& Parameters)
{
	const FRequestOnItsWay Answered;
	const FRequestOnItsWay Next;
	const TSharedRef<FFlockRequestsInTurn> InTurn = FFlockRequestsInTurn::Make();
	// The first request is answered at once and sends the next before its own Send returns.
	InTurn->Send([&]()
	{
		InTurn->Send([&Next]() { return Next.Handle(); });
		return Answered.Handle();
	});
	TestFalse(TEXT("not cancelled before the game asks"), InTurn->IsCancelled());
	InTurn->GetHandle().Cancel();
	TestTrue(TEXT("the call reads as cancelled"), InTurn->IsCancelled());
	TestTrue(TEXT("the request on its way was cancelled"), *Next.Cancelled);
	TestFalse(TEXT("the one already answered was left alone"), *Answered.Cancelled);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
