// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/Relay/FlockRelayPackets.h"
#include "Multiplayer/Relay/FlockRelaySocket.h"

FFlockRelayPackets::FFlockRelayPackets(FFlockRelaySocket& InSocket)
	: Socket(InSocket)
{
	const TArray<uint8> Transaction = FFlockRelayWire::NewTransaction();
	FMemory::Memcpy(IndicationTransaction, Transaction.GetData(), FFlockRelayWire::TransactionLength);
	FMemory::Memzero(ArrivedLengths, sizeof(ArrivedLengths));
}

bool FFlockRelayPackets::IsOpenedTo(uint32 Ip) const
{
	FScopeLock Locked(&StateLock);
	return OpenedTo.Contains(Ip);
}

int32 FFlockRelayPackets::GetOpeningsCount() const
{
	FScopeLock Locked(&StateLock);
	return OpenedTo.Num();
}

bool FFlockRelayPackets::AddOpening(uint32 Ip)
{
	FScopeLock Locked(&StateLock);
	if (OpenedTo.Contains(Ip))
	{
		return false;
	}
	OpenedTo.Add(Ip);
	return true;
}

bool FFlockRelayPackets::Send(const FFlockRelayPeer& To, const uint8* Data, int32 Count)
{
	if (Data == nullptr || Count <= 0 || Count > MostBytesInAPacket)
	{
		return false;
	}
	FScopeLock Locked(&StateLock);
	if (!OpenedTo.Contains(To.Address))
	{
		return false;
	}
	const uint16 Channel = ChannelForLocked(To);
	int32 Length = 0;
	if (Channel != 0)
	{
		Length = FFlockRelayWire::WriteChannelFrame(SendBuffer, Channel, Data, Count, Socket.PadsChannelFrames());
	}
	else
	{
		FFlockRelayWire::WriteUInt32(IndicationTransaction, 8, ++IndicationsSent);
		Length = FFlockRelayWire::WriteSendIndication(SendBuffer, IndicationTransaction, To, Data, Count);
	}
	if (!Socket.TrySend(SendBuffer, Length))
	{
		return false;
	}
	++PacketsSent;
	return true;
}

bool FFlockRelayPackets::HandArrivalsTo(const TSharedRef<IFlockRelayReceiver, ESPMode::ThreadSafe>& InReceiver)
{
	FScopeLock Locked(&ReceiverLock);
	if (Receiver.IsValid())
	{
		return false;
	}
	Receiver = InReceiver;
	return true;
}

void FFlockRelayPackets::StopHandingArrivalsTo(const TSharedRef<IFlockRelayReceiver, ESPMode::ThreadSafe>& InReceiver)
{
	FScopeLock Locked(&ReceiverLock);
	if (Receiver.Pin() == InReceiver)
	{
		Receiver.Reset();
	}
}

bool FFlockRelayPackets::HandsArrivalsOver() const
{
	FScopeLock Locked(&ReceiverLock);
	return Receiver.IsValid();
}

bool FFlockRelayPackets::KeepsPacketsForTesting() const
{
	FScopeLock Locked(&ArrivedLock);
	return ArrivedBytes.Num() > 0;
}

bool FFlockRelayPackets::TryReceive(uint8* Into, int32 IntoSize, int32& OutCount, FFlockRelayPeer& OutFrom)
{
	OutCount = 0;
	OutFrom = FFlockRelayPeer();
	if (Into == nullptr || IntoSize < MostBytesInAPacket)
	{
		return false;
	}
	FScopeLock Locked(&ArrivedLock);
	if (ArrivedCount == 0)
	{
		return false;
	}
	OutCount = ArrivedLengths[ArrivedFirst];
	OutFrom = ArrivedFrom[ArrivedFirst];
	FMemory::Memcpy(Into, ArrivedBytes.GetData() + ArrivedFirst * MostBytesInAPacket, OutCount);
	ArrivedFirst = (ArrivedFirst + 1) % PacketsHeld;
	--ArrivedCount;
	return true;
}

bool FFlockRelayPackets::TryTake(const uint8* Packet, int32 Length)
{
	uint16 Channel = 0;
	int32 DataLength = 0;
	if (FFlockRelayWire::TryReadChannelFrame(Packet, Length, Channel, DataLength))
	{
		const int32 Index = Channel - FFlockRelayWire::FirstChannel;
		if (Index >= 0 && Index < MostChannels && !PeerOfChannel[Index].IsEmpty())
		{
			Arrive(PeerOfChannel[Index], Packet, FFlockRelayWire::ChannelFrameHeaderLength, DataLength);
		}
		return true;
	}
	FFlockRelayPeer From;
	int32 DataOffset = 0;
	int32 DataCount = 0;
	if (FFlockRelayWire::TryReadDataIndication(Packet, Length, From, DataOffset, DataCount))
	{
		Arrive(From, Packet, DataOffset, DataCount);
		return true;
	}
	return false;
}

bool FFlockRelayPackets::TryGiveNextChannel(FFlockRelayPeer& OutPeer, uint16& OutChannel)
{
	{
		FScopeLock Locked(&WantedLock);
		if (ChannelsAsked >= ChannelsWanted.Num())
		{
			OutPeer = FFlockRelayPeer();
			OutChannel = 0;
			return false;
		}
		OutPeer = ChannelsWanted[ChannelsAsked];
	}
	PeerOfChannel[ChannelsAsked] = OutPeer;
	OutChannel = static_cast<uint16>(FFlockRelayWire::FirstChannel + ChannelsAsked);
	++ChannelsAsked;
	return true;
}

void FFlockRelayPackets::ChannelBound(const FFlockRelayPeer& Peer, uint16 Number)
{
	FScopeLock Locked(&StateLock);
	Channels.Add({ Peer, Number });
}

void FFlockRelayPackets::ChannelLetGo(const FFlockRelayPeer& Peer)
{
	FScopeLock Locked(&StateLock);
	Channels.RemoveAll([&Peer](const FChannel& Channel) { return Channel.Peer == Peer; });
}

uint16 FFlockRelayPackets::ChannelFor(const FFlockRelayPeer& Peer) const
{
	FScopeLock Locked(&StateLock);
	return ChannelForLocked(Peer);
}

uint16 FFlockRelayPackets::ChannelForLocked(const FFlockRelayPeer& Peer) const
{
	for (const FChannel& Channel : Channels)
	{
		if (Channel.Peer == Peer)
		{
			return Channel.Number;
		}
	}
	return 0;
}

void FFlockRelayPackets::WantChannel(const FFlockRelayPeer& Peer)
{
	FScopeLock Locked(&WantedLock);
	// Wanted again after it was let go: renewed from the relay's thread's next turn, on the number it had.
	ChannelsLetGo.Remove(Peer);
	// Once the most channels were wanted, nothing more is asked.
	if (ChannelsWanted.Num() >= MostChannels || ChannelsWanted.Contains(Peer))
	{
		return;
	}
	ChannelsWanted.Add(Peer);
}

void FFlockRelayPackets::LetChannelGo(const FFlockRelayPeer& Peer)
{
	FScopeLock Locked(&WantedLock);
	if (ChannelsWanted.Contains(Peer))
	{
		ChannelsLetGo.AddUnique(Peer);
	}
}

bool FFlockRelayPackets::KeepsChannelTo(const FFlockRelayPeer& Peer) const
{
	FScopeLock Locked(&WantedLock);
	return ChannelsWanted.Contains(Peer) && !ChannelsLetGo.Contains(Peer);
}

void FFlockRelayPackets::Arrive(const FFlockRelayPeer& From, const uint8* Packet, int32 Offset, int32 Count)
{
	if (Count > MostBytesInAPacket)
	{
		++PacketsDropped;
		return;
	}
	TSharedPtr<IFlockRelayReceiver, ESPMode::ThreadSafe> HandTo;
	{
		FScopeLock Locked(&ReceiverLock);
		HandTo = Receiver.Pin();
	}
	if (HandTo.IsValid())
	{
		HandTo->Arrived(From, Packet + Offset, Count);
		return;
	}
	FScopeLock Locked(&ArrivedLock);
	if (ArrivedCount == PacketsHeld)
	{
		++PacketsDropped;
		return;
	}
	// Made on the relay's thread when the first packet is kept, so a relay whose packets are all handed over has none.
	if (ArrivedBytes.Num() == 0)
	{
		ArrivedBytes.SetNumUninitialized(PacketsHeld * MostBytesInAPacket);
	}
	const int32 Slot = (ArrivedFirst + ArrivedCount) % PacketsHeld;
	FMemory::Memcpy(ArrivedBytes.GetData() + Slot * MostBytesInAPacket, Packet + Offset, Count);
	ArrivedLengths[Slot] = Count;
	ArrivedFrom[Slot] = From;
	++ArrivedCount;
}
