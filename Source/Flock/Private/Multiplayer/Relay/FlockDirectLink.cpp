// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/Relay/FlockDirectLink.h"
#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "Misc/ScopeLock.h"
#include "Multiplayer/FlockDirectAddresses.h"
#include "Multiplayer/FlockUdpSockets.h"
#include "Multiplayer/Relay/FlockRelayWire.h"
#include "Sockets.h"

TOptional<FFlockDirectLink::FTiming> FFlockDirectLink::TimingForTesting;
FCriticalSection FFlockDirectLink::OffersInsteadLock;
TArray<FFlockRelayPeer> FFlockDirectLink::NextOffersInstead;

namespace
{
	/** The most packets read in one turn, so the timers run even while the peer sends without a pause. */
	constexpr int32 MostReadsATurn = 256;
	/** Room for any packet the engine sends (1024 bytes) with plenty to spare; a larger one is not read whole and is dropped. */
	constexpr int32 LargestPacket = 2048;

	/** A dotted IPv4 address as the four bytes of a relay peer, or 0 for anything else. */
	uint32 Ipv4Of(const FString& Text)
	{
		FFlockRelayPeer Parsed;
		return !Text.IsEmpty() && FFlockRelayPeer::TryParse(Text + TEXT(":1"), Parsed) ? Parsed.Address : 0;
	}

	/** Where a packet came from, read as IPv4 whether the socket is dual-stack or not; false for an IPv6 sender. */
	bool HeardAsIpv4(const FInternetAddr& Address, FFlockRelayPeer& Out)
	{
		const TArray<uint8> Raw = Address.GetRawIp();
		int32 At = INDEX_NONE;
		if (Raw.Num() == 4)
		{
			At = 0;
		}
		else if (Raw.Num() == 16 && Raw[10] == 0xFF && Raw[11] == 0xFF)
		{
			At = 12;
			for (int32 Index = 0; Index < 10; ++Index)
			{
				if (Raw[Index] != 0)
				{
					return false;
				}
			}
		}
		if (At == INDEX_NONE)
		{
			return false;
		}
		Out = FFlockRelayPeer((uint32(Raw[At]) << 24) | (uint32(Raw[At + 1]) << 16) | (uint32(Raw[At + 2]) << 8) | uint32(Raw[At + 3]), Address.GetPort());
		return true;
	}

	/** Says Line with its first letter a capital, as it starts a sentence. */
	FString AsSentence(FString Line)
	{
		if (Line.Len() > 0)
		{
			Line[0] = FChar::ToUpper(Line[0]);
		}
		return Line;
	}
}

FFlockDirectLink::FPtr FFlockDirectLink::Start(const FFlockRelayPeer& RelayServer, const TArray<FFlockRelayPeer>& StunServers,
	FSendThroughTheRelay SendThroughTheRelay, FHandToTheEngine HandToTheEngine, const FString& PeerName)
{
	FSocket* Socket = FFlockUdpSockets::Open(ThreadName);
	if (Socket == nullptr)
	{
		return nullptr;
	}
	const FPtr Link = FPtr(new FFlockDirectLink(Socket, TimingForTesting.IsSet() ? TimingForTesting.GetValue() : FTiming(), RelayServer,
		StunServers, MoveTemp(SendThroughTheRelay), MoveTemp(HandToTheEngine), PeerName));
	if (!FFlockRelayThreads::Start(Link.ToSharedRef(), ThreadName, ThreadStackBytes))
	{
		// Its socket closes with it.
		return nullptr;
	}
	return Link;
}

void FFlockDirectLink::OfferInsteadForTesting(const TArray<FFlockRelayPeer>& Addresses)
{
	FScopeLock Locked(&OffersInsteadLock);
	NextOffersInstead = Addresses;
}

FFlockDirectLink::FFlockDirectLink(FSocket* InSocket, const FTiming& InTiming, const FFlockRelayPeer& InRelayServer, const TArray<FFlockRelayPeer>& StunServers,
	FSendThroughTheRelay InSendThroughTheRelay, FHandToTheEngine InHandToTheEngine, const FString& InPeerName)
	: Timing(InTiming)
	, AddressServers(StunServers)
	, SendThroughTheRelay(MoveTemp(InSendThroughTheRelay))
	, HandToTheEngine(MoveTemp(InHandToTheEngine))
	, PeerName(InPeerName)
	, OurKey(FFlockBridgeKey::NewRandom())
	, AddressTransaction(FFlockRelayWire::NewTransaction())
	, AddressRequest(FFlockDirectAddresses::StunRequest(AddressTransaction))
	, LocalPort(FFlockUdpSockets::LocalPortOf(*InSocket))
	, Protocol(InSocket->GetProtocol())
	, From(FFlockUdpSockets::AnySender(*InSocket))
	, Socket(InSocket)
{
	AddressServers.AddUnique(InRelayServer);
	LanAddress = Ipv4Of(FFlockDirectAddresses::LanAddressToward(InRelayServer.Ip()));
	{
		FScopeLock Locked(&OffersInsteadLock);
		OffersInstead = MoveTemp(NextOffersInstead);
		NextOffersInstead.Reset();
	}
	NextAddressAskAt = FPlatformTime::Seconds();
}

FFlockDirectLink::~FFlockDirectLink()
{
	// A link whose thread never ran closes its socket here.
	CloseSocket();
}

bool FFlockDirectLink::IsDirect() const
{
	FScopeLock Locked(&Gate);
	return Stage == EStage::Direct;
}

bool FFlockDirectLink::HasStopped() const
{
	FScopeLock Locked(&Gate);
	return Stage >= EStage::OnTheRelay;
}

bool FFlockDirectLink::HeardAnotherKeyThan(const FFlockBridgeKey& Key) const
{
	FScopeLock Locked(&Gate);
	return TheirKey.IsSet() && TheirKey.GetValue() != Key;
}

TOptional<FFlockRelayPeer> FFlockDirectLink::GetPublicAddressForTesting() const
{
	FScopeLock Locked(&Gate);
	return PublicAddress;
}

bool FFlockDirectLink::IsProbingForTesting() const
{
	FScopeLock Locked(&Gate);
	return Stage == EStage::Probing;
}

TArray<FFlockRelayPeer> FFlockDirectLink::GetTheirAddressesForTesting() const
{
	FScopeLock Locked(&Gate);
	return TheirAddresses;
}

void FFlockDirectLink::HeardOffer(const FFlockBridgeKey& InTheirKey, const TOptional<FFlockBridgeKey>& KeyTheyHold, const TArray<FFlockRelayPeer>& InTheirAddresses)
{
	FScopeLock Locked(&Gate);
	if (Stage >= EStage::OnTheRelay)
	{
		return;
	}
	if (!TheirKey.IsSet())
	{
		TheirKey = InTheirKey;
		TheirAddresses = InTheirAddresses;
		if (Stage == EStage::Offering)
		{
			StartProbing(FPlatformTime::Seconds());
		}
	}
	else if (TheirKey.GetValue() != InTheirKey)
	{
		return;
	}
	// Answered only while the peer lacks this link's key, so two ends never bounce offers at each other.
	bOfferNow = Stage != EStage::LearningItsAddress && (!KeyTheyHold.IsSet() || KeyTheyHold.GetValue() != OurKey);
}

void FFlockDirectLink::HeardBackToTheRelay(const uint8* Data, int32 Count)
{
	FScopeLock Locked(&Gate);
	if (Stage >= EStage::OnTheRelay || !TheirKey.IsSet() || !FFlockBridgeMessages::IsBetween(Data, Count, OurKey, TheirKey.GetValue()))
	{
		return;
	}
	const bool bWasDirect = Stage == EStage::Direct;
	StopGoingDirect(EStage::OnTheRelay);
	if (bWasDirect)
	{
		Say(FString::Printf(TEXT("%s went back to Flock's relay, so packets to it do too."), *PeerName));
	}
}

bool FFlockDirectLink::TrySend(const uint8* Data, int32 Count)
{
	{
		FScopeLock Locked(&Gate);
		if (Stage != EStage::Direct)
		{
			return false;
		}
	}
	FScopeLock Locked(&SocketLock);
	int32 Sent = 0;
	return Socket != nullptr && Socket->Send(Data, Count, Sent) && Sent == Count;
}

void FFlockDirectLink::Close()
{
	FScopeLock Locked(&Gate);
	StopGoingDirect(EStage::Closed);
}

TArray<FString> FFlockDirectLink::TakeLines()
{
	FScopeLock Locked(&LinesLock);
	TArray<FString> Taken = MoveTemp(Lines);
	Lines.Reset();
	bHasLines = false;
	return Taken;
}

uint32 FFlockDirectLink::Run()
{
	uint8 Buffer[LargestPacket];
	while (true)
	{
		double Wait = Timing.TickSeconds;
		{
			FScopeLock Locked(&Gate);
			if (Stage >= EStage::OnTheRelay)
			{
				break;
			}
			// Once direct only the keep-alive and the quiet check are due, so the thread wakes less often.
			if (Stage == EStage::Direct)
			{
				Wait = FMath::Max(Timing.TickSeconds, Timing.KeepAliveEverySeconds / 4.0);
			}
		}
		if (Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromSeconds(Wait)))
		{
			for (int32 Reads = 0; Reads < MostReadsATurn; ++Reads)
			{
				int32 Read = 0;
				// Nothing more waiting, or Windows reporting an earlier send to a closed port; the timers run either way.
				if (!Socket->RecvFrom(Buffer, LargestPacket, Read, *From))
				{
					break;
				}
				FFlockRelayPeer Sender;
				if (Read > 0 && HeardAsIpv4(*From, Sender))
				{
					Heard(Buffer, Read, Sender);
				}
			}
		}
		Tick(FPlatformTime::Seconds());
	}
	CloseSocket();
	bEnded = true;
	return 0;
}

void FFlockDirectLink::Heard(const uint8* Data, int32 Count, const FFlockRelayPeer& Sender)
{
	if (bHearsNothingForTesting)
	{
		return;
	}
	if (bFiltersLikeARouterForTesting)
	{
		FScopeLock Locked(&Gate);
		if (!SentTo.Contains(Sender))
		{
			return;
		}
	}
	if (FFlockBridgeMessages::IsBridgeMessage(Data, Count))
	{
		HeardMessage(Data, Count, Sender);
		return;
	}
	if (AddressServers.Contains(Sender))
	{
		FString Address;
		int32 Port = 0;
		const uint32 Seen = FFlockDirectAddresses::PublicEndpointInStunAnswer(Data, Count, AddressTransaction, Address, Port) ? Ipv4Of(Address) : 0;
		FScopeLock Locked(&Gate);
		if (Seen != 0 && Port > 0 && Stage == EStage::LearningItsAddress)
		{
			PublicAddress = FFlockRelayPeer(Seen, Port);
			AddressesKnown(FPlatformTime::Seconds());
		}
		return;
	}
	bool bFromThePeer = false;
	{
		FScopeLock Locked(&Gate);
		const bool bFromTheDirectPath = Direct.IsSet() && Sender == Direct.GetValue();
		bFromThePeer = bFromTheDirectPath || (HeardAt.IsSet() && Sender == HeardAt.GetValue());
		if (bFromTheDirectPath)
		{
			LastHeardAt = FPlatformTime::Seconds();
		}
	}
	if (bFromThePeer)
	{
		HandToTheEngine(Data, Count);
	}
}

void FFlockDirectLink::HeardMessage(const uint8* Data, int32 Count, const FFlockRelayPeer& Sender)
{
	const uint8 Kind = FFlockBridgeMessages::KindOf(Data, Count);
	FScopeLock Locked(&Gate);
	if (Stage >= EStage::OnTheRelay || !TheirKey.IsSet() || !FFlockBridgeMessages::IsBetween(Data, Count, OurKey, TheirKey.GetValue()))
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	if (Kind == FFlockBridgeMessages::Probe)
	{
		HeardAt = Sender;
		SendTo(FFlockBridgeMessages::WriteBetween(FFlockBridgeMessages::Answer, TheirKey.GetValue(), OurKey), Sender);
		// A router that gives each destination its own port sends the peer's probes from an address it never offered: that one is probed too.
		if (Stage == EStage::Probing && !TheirAddresses.Contains(Sender) && TheirAddresses.Num() < MostAddressesProbed)
		{
			TheirAddresses.Add(Sender);
			SendTo(FFlockBridgeMessages::WriteBetween(FFlockBridgeMessages::Probe, TheirKey.GetValue(), OurKey), Sender);
		}
	}
	else if (Kind == FFlockBridgeMessages::Answer)
	{
		if (bDropsAnswersForTesting || Stage != EStage::Probing)
		{
			return;
		}
		// An answer proves both ways: this link's probe reached the peer, and the peer's answer came back.
		if (!Socket->Connect(*FFlockUdpSockets::Toward(*Socket, Sender.Ip(), Sender.Port)))
		{
			return;
		}
		Direct = Sender;
		HeardAt = Sender;
		Stage = EStage::Direct;
		LastHeardAt = Now;
		NextKeepAliveAt = Now;
		Say(FString::Printf(TEXT("Packets to %s now go directly; Flock's relay stays ready in case the direct path goes quiet."), *PeerName));
	}
	else if (Kind == FFlockBridgeMessages::KeepAlive && Direct.IsSet() && Sender == Direct.GetValue())
	{
		LastHeardAt = Now;
	}
}

void FFlockDirectLink::Tick(double Now)
{
	// What goes through the relay is sent once the lock is let go.
	TArray<TArray<uint8>> ThroughTheRelay;
	{
		FScopeLock Locked(&Gate);
		switch (Stage)
		{
		case EStage::LearningItsAddress:
			if (Now < NextAddressAskAt)
			{
				break;
			}
			if (AddressAsksMade >= Timing.AddressAsks)
			{
				// No server said where it sees this socket: the LAN address alone is offered.
				AddressesKnown(Now);
				break;
			}
			++AddressAsksMade;
			NextAddressAskAt = Now + Timing.AskAddressEverySeconds;
			for (const FFlockRelayPeer& Server : AddressServers)
			{
				SendTo(AddressRequest, Server);
			}
			break;
		case EStage::Offering:
			OfferWhenDue(Now, ThroughTheRelay);
			if (Stage == EStage::Offering && Now >= OfferUntil)
			{
				GiveUp(TEXT("the other end never offered its addresses"));
			}
			break;
		case EStage::Probing:
			OfferWhenDue(Now, ThroughTheRelay);
			if (Now >= NextProbeAt)
			{
				NextProbeAt = Now + Timing.ProbeEverySeconds;
				const TArray<uint8> Probe = FFlockBridgeMessages::WriteBetween(FFlockBridgeMessages::Probe, TheirKey.GetValue(), OurKey);
				for (const FFlockRelayPeer& Address : TheirAddresses)
				{
					SendTo(Probe, Address);
				}
			}
			if (Now >= ProbeUntil)
			{
				GiveUp(TEXT("no probe was answered"));
			}
			break;
		case EStage::Direct:
			if (Now - LastHeardAt >= Timing.QuietForSeconds)
			{
				StopGoingDirect(EStage::OnTheRelay);
				ThroughTheRelay.Add(FFlockBridgeMessages::WriteBetween(FFlockBridgeMessages::BackToTheRelay, TheirKey.GetValue(), OurKey));
				Say(FString::Printf(TEXT("The direct path to %s went quiet, so packets to it go through Flock's relay again."), *PeerName));
				break;
			}
			if (Now >= NextKeepAliveAt)
			{
				NextKeepAliveAt = Now + Timing.KeepAliveEverySeconds;
				// A keep-alive that cannot go out is judged by the quiet check.
				SendTo(FFlockBridgeMessages::WriteBetween(FFlockBridgeMessages::KeepAlive, TheirKey.GetValue(), OurKey), Direct.GetValue());
			}
			break;
		default:
			break;
		}
	}
	for (const TArray<uint8>& Message : ThroughTheRelay)
	{
		SendThroughTheRelay(Message);
	}
}

void FFlockDirectLink::AddressesKnown(double Now)
{
	OurAddresses.Reset();
	if (PublicAddress.IsSet() && PublicAddress.GetValue().Address != 0)
	{
		OurAddresses.Add(PublicAddress.GetValue());
	}
	const FFlockRelayPeer Lan(LanAddress, LocalPort);
	if (LanAddress != 0 && !(PublicAddress.IsSet() && PublicAddress.GetValue() == Lan))
	{
		OurAddresses.Add(Lan);
	}
	if (OffersInstead.Num() > 0)
	{
		OurAddresses = OffersInstead;
	}
	if (OurAddresses.Num() == 0)
	{
		GiveUp(TEXT("this device's address could not be found"));
		return;
	}
	OfferUntil = Now + Timing.OfferForSeconds;
	NextOfferAt = Now;
	Stage = EStage::Offering;
	if (TheirKey.IsSet())
	{
		StartProbing(Now);
	}
}

void FFlockDirectLink::StartProbing(double Now)
{
	Stage = EStage::Probing;
	bOfferNow = true;
	NextProbeAt = Now;
	ProbeUntil = Now + Timing.ProbeForSeconds;
}

void FFlockDirectLink::OfferWhenDue(double Now, TArray<TArray<uint8>>& OutThroughTheRelay)
{
	const bool bDue = bOfferNow || (!TheirKey.IsSet() && Now >= NextOfferAt);
	if (!bDue)
	{
		return;
	}
	bOfferNow = false;
	NextOfferAt = Now + Timing.OfferEverySeconds;
	OutThroughTheRelay.Add(FFlockBridgeMessages::WriteOffer(OurKey, TheirKey, OurAddresses));
}

void FFlockDirectLink::GiveUp(const TCHAR* Why)
{
	StopGoingDirect(EStage::OnTheRelay);
	Say(FString::Printf(TEXT("%s could not be reached directly (%s), so packets to it keep going through Flock's relay."), *PeerName, Why));
}

void FFlockDirectLink::StopGoingDirect(EStage To)
{
	if (Stage == EStage::Closed || (Stage >= EStage::OnTheRelay && To != EStage::Closed))
	{
		return;
	}
	Stage = To;
}

void FFlockDirectLink::SendTo(const TArray<uint8>& Bytes, const FFlockRelayPeer& To)
{
	SentTo.Add(To);
	int32 Sent = 0;
	if (!Direct.IsSet())
	{
		// A probe that could not go out; the next one may.
		Socket->SendTo(Bytes.GetData(), Bytes.Num(), Sent, *FFlockUdpSockets::Toward(*Socket, To.Ip(), To.Port));
	}
	else if (To == Direct.GetValue())
	{
		Socket->Send(Bytes.GetData(), Bytes.Num(), Sent);
	}
}

void FFlockDirectLink::Say(const FString& Line)
{
	FScopeLock Locked(&LinesLock);
	Lines.Add(AsSentence(Line));
	bHasLines = true;
}

void FFlockDirectLink::CloseSocket()
{
	FScopeLock Locked(&SocketLock);
	if (Socket != nullptr)
	{
		FFlockUdpSockets::Close(Socket);
		Socket = nullptr;
	}
}
