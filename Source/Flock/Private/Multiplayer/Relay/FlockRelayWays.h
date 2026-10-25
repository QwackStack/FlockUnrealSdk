// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockLogger.h"
#include "Multiplayer/Relay/FlockRelayConnection.h"

/**
 * Opens a relay address through the first way in that works, for a network that blocks some of them (UDP to the relay, as some
 * phone hotspots and offices do, or IPv4 on a network with IPv6 alone): UDP over IPv4, UDP over IPv6, then TCP and TLS over IPv4,
 * then over IPv6 (FFlockRelayLogin::InTheOrderTried), each starting once the last one started failed or has had its time to
 * open: NextPortAfterSeconds before another port of the same transport and family, NextWayAfterSeconds otherwise. Staggered
 * rather than all at once, so a network where UDP works never opens a stream. The first
 * to open, in that order, is kept; every other is closed, giving back any address it reserved. Game thread: its owner runs Tick
 * once a frame.
 */
class FFlockRelayWays : public TSharedFromThis<FFlockRelayWays>
{
public:
	using FOnOpened = FFlockRelayConnection::FOnOpened;

	/** Run once a way has opened and before it is handed out, so a test can end the session then. */
	static TFunction<void()> AfterAWayOpenedForTesting;

	/**
	 * Starts the first way in to the relay servers Logins name (in Flock's order, the first its main one). OnOpened answers once:
	 * the connection kept, or, when none opens, the main server's reason (from a way that reached it when one did) and every
	 * way's cause on one line; Close before then answers it Cancelled.
	 */
	static TSharedRef<FFlockRelayWays> Start(const TArray<FFlockRelayLogin>& Logins, const TArray<FFlockRelayStunLookUp>& StunServers,
		const FFlockRelayTiming& Timing, const TSharedRef<IFlockLogger>& Logger, FOnOpened OnOpened);

	/** Hands every way still opening its answers, keeps the first that opened, and starts the next way when it is due. */
	void Tick();

	/** Stops every way still opening and closes every way that opened, giving back what each reserved; nothing more starts. */
	void Close();

	/** True once a way was kept, none opened, or it was closed. */
	bool IsDone() const { return bDone; }

	/** The ways started so far, in the order tried. */
	TArray<FFlockRelayLogin> GetWaysStartedForTesting() const;

private:
	FFlockRelayWays(const TArray<FFlockRelayLogin>& InLogins, const TArray<FFlockRelayStunLookUp>& InStunServers, const FFlockRelayTiming& InTiming,
		const TSharedRef<IFlockLogger>& InLogger, FOnOpened InOnOpened);

	struct FWay
	{
		FFlockRelayLogin Login;
		TSharedPtr<FFlockRelayConnection> Connection;
		double StartedAt = 0.0;
		bool bAnswered = false;
		/** Its end was said in the debug log, once. */
		bool bEndTold = false;
		TFlockRelayResult<TSharedPtr<FFlockRelayConnection>> Result;
	};
	using FWayRef = TSharedRef<FWay>;

	void StartNextWay();
	/** Says once in the debug log (Enable Debug Logs) how each way that answered since the last frame did: opened, or why not. */
	void TellTheWaysThatEnded();
	void Keep(const FWayRef& Kept);
	void Answer(const TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>& Result);
	FFlockRelayFailure NoWayOpened() const;
	/** Says once, at Info, that the relay is reached over TCP or TLS, or over IPv6, and why play may stutter. */
	void TellAWayOtherThanUdpOverIpv4(const FFlockRelayConnection& Kept) const;

	/** The logins as Flock gave them, its main server first, and every way in to them in the order tried. */
	const TArray<FFlockRelayLogin> Logins;
	const TArray<FFlockRelayLogin> Ways;
	const TArray<FFlockRelayStunLookUp> StunServers;
	const FFlockRelayTiming Timing;
	TSharedRef<IFlockLogger> Logger;
	FOnOpened OnOpened;
	TArray<FWayRef> Started;
	/** When the race began, so the debug log says when each way started and ended within it. */
	double BeganAt = 0.0;
	double NextWayAt = 0.0;
	bool bDone = false;
};
