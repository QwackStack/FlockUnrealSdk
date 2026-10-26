// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/Relay/FlockRelayWays.h"
#include "HAL/PlatformTime.h"

TFunction<void()> FFlockRelayWays::AfterAWayOpenedForTesting;

TSharedRef<FFlockRelayWays> FFlockRelayWays::Start(const TArray<FFlockRelayLogin>& Logins, const TArray<FFlockRelayStunLookUp>& StunServers,
	const FFlockRelayTiming& Timing, const TSharedRef<IFlockLogger>& Logger, FOnOpened OnOpened)
{
	const TSharedRef<FFlockRelayWays> Opening = MakeShareable(new FFlockRelayWays(Logins, StunServers, Timing, Logger, MoveTemp(OnOpened)));
	if (Opening->Ways.Num() == 0)
	{
		Opening->bDone = true;
		Opening->Answer(TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>::Fail(FFlockRelayFailure::Of(FlockRelayFailureReasons::NotOffered,
			TEXT("Flock listed no way in to its relay that this game can use"))));
		return Opening;
	}
	Opening->BeganAt = FPlatformTime::Seconds();
	Opening->StartNextWay();
	return Opening;
}

FFlockRelayWays::FFlockRelayWays(const TArray<FFlockRelayLogin>& InLogins, const TArray<FFlockRelayStunLookUp>& InStunServers,
	const FFlockRelayTiming& InTiming, const TSharedRef<IFlockLogger>& InLogger, FOnOpened InOnOpened)
	: Logins(InLogins)
	, Ways(FFlockRelayLogin::InTheOrderTried(InLogins))
	, StunServers(InStunServers)
	, Timing(InTiming)
	, Logger(InLogger)
	, OnOpened(MoveTemp(InOnOpened))
{
}

void FFlockRelayWays::Tick()
{
	if (bDone)
	{
		return;
	}
	// Kept alive through the answers, which may drop the owner's hold on this race.
	const TSharedRef<FFlockRelayWays> KeepWhileAnswering = AsShared();
	// By index, with no copy each frame: a connection's answer only records itself, so nothing starts or ends meanwhile.
	for (int32 Index = 0; Index < Started.Num(); ++Index)
	{
		const FWayRef Way = Started[Index];
		if (!Way->bAnswered)
		{
			Way->Connection->Tick();
		}
	}
	TellTheWaysThatEnded();
	// The first way, in the order tried, that opened: of two that opened together, the better one.
	for (const FWayRef& Way : Started)
	{
		if (Way->bAnswered && Way->Result.bSuccess)
		{
			Keep(Way);
			return;
		}
	}
	for (const FWayRef& Way : Started)
	{
		if (Way->bAnswered && Way->Result.Failure.Reason.Equals(FlockRelayFailureReasons::NoThread, ESearchCase::CaseSensitive))
		{
			// No thread for one way is none for any (a launch without threads): told once, rather than by every way in turn.
			bDone = true;
			for (const FWayRef& Other : Started)
			{
				Other->Connection->Close();
			}
			Answer(Way->Result);
			return;
		}
	}
	const bool bAnyOpening = Started.ContainsByPredicate([](const FWayRef& Way) { return !Way->bAnswered; });
	// The next way starts once the last one started has failed, even while an earlier one still waits, or once it has had its time.
	const bool bLastStartedFailed = Started.Num() > 0 && Started.Last()->bAnswered;
	if (Started.Num() < Ways.Num() && (!bAnyOpening || bLastStartedFailed || FPlatformTime::Seconds() >= NextWayAt))
	{
		StartNextWay();
		return;
	}
	if (!bAnyOpening)
	{
		bDone = true;
		Answer(TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>::Fail(NoWayOpened()));
	}
}

void FFlockRelayWays::Close()
{
	if (bDone)
	{
		return;
	}
	bDone = true;
	for (const FWayRef& Way : Started)
	{
		Way->Connection->Close();
	}
	Answer(TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>::Fail(FFlockRelayFailure::Cancelled(TEXT("The relay was closed while its ways in opened"))));
}

TArray<FFlockRelayLogin> FFlockRelayWays::GetWaysStartedForTesting() const
{
	TArray<FFlockRelayLogin> Out;
	for (const FWayRef& Way : Started)
	{
		Out.Add(Way->Login);
	}
	return Out;
}

void FFlockRelayWays::StartNextWay()
{
	const FWayRef Way = MakeShared<FWay>();
	Way->Login = Ways[Started.Num()];
	Started.Add(Way);
	// Another port of the way just started (the same transport and family) comes soon after it; the next kind of way waits longer.
	const bool bSameKindNext = Ways.IsValidIndex(Started.Num()) && Ways[Started.Num()].Transport == Way->Login.Transport
		&& Ways[Started.Num()].bOverIpv6 == Way->Login.bOverIpv6;
	Way->StartedAt = FPlatformTime::Seconds();
	NextWayAt = Way->StartedAt + (bSameKindNext ? Timing.NextPortAfterSeconds : Timing.NextWayAfterSeconds);
	Logger->LogDebug(FString::Printf(TEXT("Flock's relay: trying %s (%.1f s into opening it)."), *Way->Login.GetWay(), Way->StartedAt - BeganAt));
	const TWeakPtr<FWay> WeakWay = Way;
	Way->Connection = FFlockRelayConnection::Open(Way->Login, StunServers, Timing, Logger, [WeakWay](TFlockRelayResult<TSharedPtr<FFlockRelayConnection>> Result)
	{
		if (const TSharedPtr<FWay> Pinned = WeakWay.Pin())
		{
			Pinned->bAnswered = true;
			Pinned->Result = MoveTemp(Result);
		}
	});
}

void FFlockRelayWays::TellTheWaysThatEnded()
{
	const double Now = FPlatformTime::Seconds();
	for (const FWayRef& Way : Started)
	{
		if (Way->bAnswered && !Way->bEndTold)
		{
			Way->bEndTold = true;
			Logger->LogDebug(Way->Result.bSuccess
				? FString::Printf(TEXT("Flock's relay: %s opened after %.1f s (%.1f s into opening it)."), *Way->Login.GetWay(), Now - Way->StartedAt, Now - BeganAt)
				: FString::Printf(TEXT("Flock's relay: %s did not open after %.1f s (%.1f s into opening it): %s"), *Way->Login.GetWay(), Now - Way->StartedAt,
					Now - BeganAt, *Way->Result.Failure.Error.Message));
		}
	}
}

void FFlockRelayWays::Keep(const FWayRef& Kept)
{
	if (AfterAWayOpenedForTesting)
	{
		AfterAWayOpenedForTesting();
	}
	if (bDone)
	{
		// Closed as the way opened (the session ended): nobody holds it, so it was given back with every other.
		return;
	}
	bDone = true;
	// A way that opened beside the one kept, or is still opening, is closed, so its address is given back.
	for (const FWayRef& Way : Started)
	{
		if (Way != Kept)
		{
			Way->Connection->Close();
		}
	}
	const TSharedRef<FFlockRelayConnection> Connection = Kept->Connection.ToSharedRef();
	TellAWayOtherThanUdpOverIpv4(*Connection);
	Answer(TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>::Ok(Connection));
}

void FFlockRelayWays::Answer(const TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>& Result)
{
	// Answered once: taken out before it runs, so an answer it causes cannot answer it again.
	FOnOpened Answering = MoveTemp(OnOpened);
	OnOpened = nullptr;
	if (Answering)
	{
		Answering(Result);
	}
}

FFlockRelayFailure FFlockRelayWays::NoWayOpened() const
{
	if (Started.Num() == 1)
	{
		return Started[0]->Result.Failure;
	}
	// Told with the reason of Flock's main server (the first it lists), from a way that reached it when one did: a refused login
	// says more than a blocked port.
	const FString MainServer = Logins.Num() > 0 ? Logins[0].ToString() : FString();
	const FFlockRelayFailure* Told = nullptr;
	for (const FWayRef& Way : Started)
	{
		const FFlockRelayFailure& Failure = Way->Result.Failure;
		if (Way->Login.IsOn(MainServer) && (Told == nullptr || (Told->Reason.Equals(FlockRelayFailureReasons::Unreachable, ESearchCase::CaseSensitive)
			&& !Failure.Reason.Equals(FlockRelayFailureReasons::Unreachable, ESearchCase::CaseSensitive))))
		{
			Told = &Failure;
		}
	}
	if (Told == nullptr)
	{
		Told = &Started[0]->Result.Failure;
	}
	// Every way's own cause, without its fix: the reason's one fix follows them all.
	FString Message = TEXT("No way in to Flock's relay opened.");
	for (const FWayRef& Way : Started)
	{
		FString Cause = Way->Result.Failure.Error.Message;
		Cause.TrimStartAndEndInline();
		while (Cause.EndsWith(TEXT(".")))
		{
			Cause.LeftChopInline(1);
		}
		Message += TEXT(" ") + Cause + TEXT(".");
	}
	return FFlockRelayFailure::Of(Told->Reason, Message);
}

void FFlockRelayWays::TellAWayOtherThanUdpOverIpv4(const FFlockRelayConnection& Kept) const
{
	if (Kept.GetTransport() == EFlockRelayTransport::Udp && !Kept.IsOverIpv6())
	{
		return;
	}
	const bool bListsUdp = Ways.ContainsByPredicate([](const FFlockRelayLogin& Way) { return Way.Transport == EFlockRelayTransport::Udp; });
	const TCHAR* Why = Kept.IsOverIpv6()
		? TEXT("no way in over IPv4 opened first, as on a network with IPv6 alone")
		: bListsUdp
			? TEXT("no UDP way in opened first, as on a network that blocks UDP to the relay")
			: TEXT("Flock lists no UDP way in to it");
	const TCHAR* Stutter = Kept.GetTransport() == EFlockRelayTransport::Udp
		? TEXT("")
		: TEXT(" A packet lost on the way holds back the ones after it until it is sent again, so play may stutter on a poor connection.");
	Logger->LogInfo(FString::Printf(TEXT("Flock's relay is reached at %s: %s.%s"), *Kept.GetWay(), Why, Stutter));
}
