// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Multiplayer/Relay/FlockRelayPeer.h"

class FFlockRelayPackets;
class FFlockRelayRequests;
struct FFlockRelayMessage;
struct FFlockRelayTiming;

/**
 * What the relay keeps only while it is renewed (the relay address, each opening, each channel): renewed before the relay
 * lets it go, and the relay lost when one lapses or a renewal is refused. The relay's thread alone.
 */
class FFlockRelayRenewals
{
public:
	/** Warn tells the game something it should know (on the game thread); LoseTheRelay stops the connection as lost, naming what lapsed. */
	FFlockRelayRenewals(FFlockRelayRequests& InRequests, FFlockRelayPackets& InPackets, const FFlockRelayTiming& InTiming,
		TFunction<void(const FString&)> InWarn, TFunction<void(const FString&)> InLoseTheRelay);

	/** Keeps the relay address the reservation's answer granted, renewed from now on. */
	void KeepReservation(const FFlockRelayMessage& Answer, double Now);

	/** Keeps an opening the relay just granted, renewed from now on. */
	void KeepOpening(uint32 Ip, double Now);

	/** Binds Channel to Peer now, and renews it from then on. */
	void BindChannel(const FFlockRelayPeer& Peer, uint16 Channel, double Now);

	/** Sends every renewal that is due, and loses the relay when something it keeps lapsed. */
	void RenewDue(double Now);

private:
	enum class EKind : uint8 { Reservation, Opening, Channel };

	/** Something the relay keeps only as long as it is renewed: the relay address, an opening, or a channel. */
	struct FRenewal
	{
		EKind Kind = EKind::Reservation;
		uint32 Address = 0;
		FFlockRelayPeer Peer;
		uint16 Channel = 0;
		bool bBound = false;
		bool bRefused = false;
		bool bSent = false;
		double RenewAt = 0.0;
		double GoodUntil = 0.0;
	};
	using FRenewalRef = TSharedRef<FRenewal, ESPMode::ThreadSafe>;

	void Send(const FRenewalRef& Renewal);
	void Renewed(const FRenewalRef& Renewal, int32 Code, const FString& Words, const FFlockRelayMessage* Answer);
	void KeepReservation(FRenewal& Renewal, const FFlockRelayMessage* Answer, double Now) const;

	FFlockRelayRequests& Requests;
	FFlockRelayPackets& Packets;
	const FFlockRelayTiming& Timing;
	TFunction<void(const FString&)> Warn;
	TFunction<void(const FString&)> LoseTheRelay;
	TArray<FRenewalRef> Renewals;
	bool bToldChannelRefused = false;
};
