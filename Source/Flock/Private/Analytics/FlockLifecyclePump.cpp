// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Analytics/FlockLifecyclePump.h"

#include "Misc/CoreDelegates.h"

FFlockLifecyclePump::~FFlockLifecyclePump()
{
	Stop();
}

void FFlockLifecyclePump::Start()
{
	if (bRunning)
	{
		return;
	}
	bRunning = true;

	// Raw binding is safe here: every handle is released in Stop(), which the destructor calls, so
	// the ticker can never outlive the pump.
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FFlockLifecyclePump::HandleTick));

	BackgroundHandle = FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddLambda(
		[this]() { HandleBackgroundChanged(true); });
	ForegroundHandle = FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddLambda(
		[this]() { HandleBackgroundChanged(false); });

	// Mobile raises Terminate; desktop and editor generally only reach OnPreExit. Subscribing to
	// both is what makes "end the session on quit" work on every platform.
	TerminateHandle = FCoreDelegates::ApplicationWillTerminateDelegate.AddLambda(
		[this]() { HandleQuit(); });
	PreExitHandle = FCoreDelegates::OnPreExit.AddLambda(
		[this]() { HandleQuit(); });
}

void FFlockLifecyclePump::Stop()
{
	if (!bRunning)
	{
		return;
	}
	bRunning = false;

	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	FCoreDelegates::ApplicationWillEnterBackgroundDelegate.Remove(BackgroundHandle);
	FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Remove(ForegroundHandle);
	FCoreDelegates::ApplicationWillTerminateDelegate.Remove(TerminateHandle);
	FCoreDelegates::OnPreExit.Remove(PreExitHandle);

	BackgroundHandle.Reset();
	ForegroundHandle.Reset();
	TerminateHandle.Reset();
	PreExitHandle.Reset();
}

bool FFlockLifecyclePump::HandleTick(float DeltaSeconds)
{
	// A backgrounded app on some platforms keeps ticking; session time must not accrue while it is
	// not in front of the player.
	if (!bBackgrounded)
	{
		OnTick.Broadcast(DeltaSeconds);
	}
	return true; // keep ticking
}

void FFlockLifecyclePump::HandleBackgroundChanged(bool bInBackgrounded)
{
	if (bBackgrounded == bInBackgrounded)
	{
		return;
	}
	bBackgrounded = bInBackgrounded;
	OnBackgroundChanged.Broadcast(bInBackgrounded);
}

void FFlockLifecyclePump::HandleQuit()
{
	if (bQuitBroadcast)
	{
		return;
	}
	bQuitBroadcast = true;
	OnQuit.Broadcast();
}
