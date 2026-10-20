// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/Relay/FlockRelayRenewals.h"
#include "HAL/PlatformTime.h"
#include "Multiplayer/Relay/FlockRelayConnection.h"
#include "Multiplayer/Relay/FlockRelayPackets.h"
#include "Multiplayer/Relay/FlockRelayRequests.h"
#include "Multiplayer/Relay/FlockRelayWire.h"

namespace
{
	constexpr uint32 ReservationSecondsAsked = 600;
}

FFlockRelayRenewals::FFlockRelayRenewals(FFlockRelayRequests& InRequests, FFlockRelayPackets& InPackets, const FFlockRelayTiming& InTiming,
	TFunction<void(const FString&)> InWarn, TFunction<void(const FString&)> InLoseTheRelay)
	: Requests(InRequests)
	, Packets(InPackets)
	, Timing(InTiming)
	, Warn(MoveTemp(InWarn))
	, LoseTheRelay(MoveTemp(InLoseTheRelay))
{
}

void FFlockRelayRenewals::KeepReservation(const FFlockRelayMessage& Answer, double Now)
{
	const FRenewalRef Reservation = MakeShared<FRenewal, ESPMode::ThreadSafe>();
	Reservation->Kind = EKind::Reservation;
	KeepReservation(*Reservation, &Answer, Now);
	Renewals.Add(Reservation);
}

void FFlockRelayRenewals::KeepOpening(uint32 Ip, double Now)
{
	const FRenewalRef Opening = MakeShared<FRenewal, ESPMode::ThreadSafe>();
	Opening->Kind = EKind::Opening;
	Opening->Address = Ip;
	Opening->GoodUntil = Now + Timing.OpeningLastsSeconds;
	Opening->RenewAt = Now + Timing.RenewOpeningsEverySeconds;
	Renewals.Add(Opening);
}

void FFlockRelayRenewals::BindChannel(const FFlockRelayPeer& Peer, uint16 Channel, double Now)
{
	const FRenewalRef Binding = MakeShared<FRenewal, ESPMode::ThreadSafe>();
	Binding->Kind = EKind::Channel;
	Binding->Peer = Peer;
	Binding->Channel = Channel;
	Binding->RenewAt = Now;
	Renewals.Add(Binding);
}

void FFlockRelayRenewals::RenewDue(double Now)
{
	// A renewal sent here may be answered, and the relay lost, only on a later turn; the list does not change under this walk.
	for (int32 Index = 0; Index < Renewals.Num(); ++Index)
	{
		const FRenewalRef Renewal = Renewals[Index];
		if (Renewal->bRefused)
		{
			continue;
		}
		if (Renewal->Kind == EKind::Channel)
		{
			if (Renewal->bBound && Now > Renewal->GoodUntil)
			{
				// A channel the relay let go: packets to its peer go as send indications until it is bound again.
				Renewal->bBound = false;
				Packets.ChannelLetGo(Renewal->Peer);
			}
			// No longer wanted (a player that left): not renewed, so it lapses; wanted again, it is renewed on its number.
			if (!Packets.KeepsChannelTo(Renewal->Peer))
			{
				continue;
			}
		}
		else if (Now > Renewal->GoodUntil)
		{
			LoseTheRelay(Renewal->Kind == EKind::Reservation
				? TEXT("the relay address was not renewed before the relay let it go")
				: TEXT("the opening to the host's relay address was not renewed before the relay let it go"));
			return;
		}
		if (Renewal->bSent || Now < Renewal->RenewAt)
		{
			continue;
		}
		Renewal->bSent = true;
		Send(Renewal);
	}
}

void FFlockRelayRenewals::Send(const FRenewalRef& Renewal)
{
	FFlockRelayRequestDone Done = [this, Renewal](int32 Code, const FString& Words, const FFlockRelayMessage* Answer)
	{
		Renewed(Renewal, Code, Words, Answer);
	};
	TArray<FFlockRelayAttribute> Attributes;
	uint16 Method = FFlockRelayWire::ChannelBindMethod;
	switch (Renewal->Kind)
	{
	case EKind::Reservation:
		Method = FFlockRelayWire::RefreshMethod;
		Attributes.Emplace(FFlockRelayWire::LifetimeAttribute, FFlockRelayWire::UInt32Value(ReservationSecondsAsked));
		break;
	case EKind::Opening:
		Method = FFlockRelayWire::CreatePermissionMethod;
		Attributes.Emplace(FFlockRelayWire::XorPeerAddressAttribute, FFlockRelayWire::AddressValue(FFlockRelayPeer(Renewal->Address, 0)));
		break;
	default:
		Attributes.Emplace(FFlockRelayWire::ChannelNumberAttribute, FFlockRelayWire::ChannelValue(Renewal->Channel));
		Attributes.Emplace(FFlockRelayWire::XorPeerAddressAttribute, FFlockRelayWire::AddressValue(Renewal->Peer));
		break;
	}
	Requests.Send(MakeShared<FFlockRelayRequest, ESPMode::ThreadSafe>(Method, MoveTemp(Done), MoveTemp(Attributes)));
}

void FFlockRelayRenewals::Renewed(const FRenewalRef& Renewal, int32 Code, const FString& Words, const FFlockRelayMessage* Answer)
{
	Renewal->bSent = false;
	const double Now = FPlatformTime::Seconds();
	if (Code == 0)
	{
		if (Renewal->Kind == EKind::Reservation)
		{
			KeepReservation(*Renewal, Answer, Now);
		}
		else if (Renewal->Kind == EKind::Opening)
		{
			Renewal->GoodUntil = Now + Timing.OpeningLastsSeconds;
			Renewal->RenewAt = Now + Timing.RenewOpeningsEverySeconds;
		}
		else
		{
			if (!Renewal->bBound)
			{
				Renewal->bBound = true;
				Packets.ChannelBound(Renewal->Peer, Renewal->Channel);
			}
			Renewal->GoodUntil = Now + Timing.ChannelLastsSeconds;
			Renewal->RenewAt = Now + Timing.RenewChannelsEverySeconds;
		}
		return;
	}
	// No answer: tried again soon; what lapses meanwhile is judged by how long the relay keeps it.
	if (Code == FFlockRelayRequests::NoAnswer)
	{
		Renewal->RenewAt = Now + Timing.RetryRenewalAfterSeconds;
		return;
	}
	if (Renewal->Kind == EKind::Channel)
	{
		Renewal->bRefused = true;
		if (Renewal->bBound)
		{
			Renewal->bBound = false;
			Packets.ChannelLetGo(Renewal->Peer);
		}
		if (!bToldChannelRefused)
		{
			bToldChannelRefused = true;
			Warn(FString::Printf(TEXT("The relay refused a channel to %s (%d %s); packets to it go as send indications, 32 bytes larger each."),
				*Renewal->Peer.ToString(), Code, *Words));
		}
		return;
	}
	LoseTheRelay(Renewal->Kind == EKind::Reservation
		? FString::Printf(TEXT("the relay refused to renew the relay address (%d %s)"), Code, *Words)
		: FString::Printf(TEXT("the relay refused to renew the opening to the host's relay address (%d %s)"), Code, *Words));
}

void FFlockRelayRenewals::KeepReservation(FRenewal& Renewal, const FFlockRelayMessage* Answer, double Now) const
{
	uint32 Seconds = ReservationSecondsAsked;
	if (Answer != nullptr)
	{
		const TArray<uint8>* Lifetime = Answer->Attributes.Find(FFlockRelayWire::LifetimeAttribute);
		if (Lifetime != nullptr && Lifetime->Num() == 4)
		{
			Seconds = FFlockRelayWire::ReadUInt32(Lifetime->GetData(), 0);
		}
	}
	Renewal.GoodUntil = Now + Seconds;
	Renewal.RenewAt = Now + FMath::Min(Seconds / 2.0, Timing.LongestBetweenReservationRenewalsSeconds);
}
