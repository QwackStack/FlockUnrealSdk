// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockHttpAdapter.h"

/**
 * One cancel handle for a call made of several requests sent in turn (join a party, then read it). Cancelling the handle
 * cancels the request on its way; the call checks IsCancelled() before sending the next. Game-thread only.
 */
class FLOCK_API FFlockRequestsInTurn : public TSharedFromThis<FFlockRequestsInTurn>
{
public:
	static TSharedRef<FFlockRequestsInTurn> Make()
	{
		const TSharedRef<FFlockRequestsInTurn> Requests = MakeShared<FFlockRequestsInTurn>();
		const TWeakPtr<FFlockRequestsInTurn> WeakRequests = Requests;
		Requests->Token->Canceller = [WeakRequests]()
		{
			if (const TSharedPtr<FFlockRequestsInTurn> Pinned = WeakRequests.Pin())
			{
				Pinned->Current.Cancel();
			}
		};
		return Requests;
	}

	FFlockRequestsInTurn()
		: Token(MakeShared<FFlockCancelToken>())
	{
	}

	FFlockRequestHandle GetHandle() const { return FFlockRequestHandle(Token); }

	bool IsCancelled() const { return Token->bCancelled; }

	/**
	 * Sends the next request and keeps its handle for cancelling. A request answered at once may send the one after it
	 * before Send returns its handle; the newest is kept, so a cancel always reaches the request still on its way.
	 */
	void Send(TFunctionRef<FFlockRequestHandle()> SendRequest)
	{
		const int32 ThisRequest = ++RequestsSent;
		FFlockRequestHandle Handle = SendRequest();
		if (ThisRequest == RequestsSent)
		{
			Current = Handle;
		}
	}

private:
	TSharedRef<FFlockCancelToken> Token;
	FFlockRequestHandle Current;
	int32 RequestsSent = 0;
};
