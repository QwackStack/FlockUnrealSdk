// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/Relay/FlockRelayStream.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Multiplayer/FlockSockets.h"
#include "Multiplayer/Relay/FlockRelayFailures.h"
#include "Multiplayer/Relay/FlockRelayTls.h"
#include "Multiplayer/Relay/FlockRelayWire.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

namespace
{
	/** How much is read from, or handed to, the network at a time. */
	constexpr int32 ChunkBytes = 16 * 1024;
	/** How long a wait on the socket lasts at a time, so a stop is seen within it. */
	constexpr double SliceSeconds = 0.01;
	/** How long the sending thread sleeps with nothing to send before it looks again whether it should stop. */
	constexpr uint32 SendingIdleMilliseconds = 100;
	constexpr uint32 SendingThreadStackBytes = 256 * 1024;

	/** Why a read found the connection gone: the relay closed it, or it broke. */
	FString WhyTheReadFailed()
	{
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		const ESocketErrors Error = Sockets != nullptr ? Sockets->GetLastErrorCode() : SE_NO_ERROR;
		// A connection the relay ended leaves no error of its own: what is left is the last read that found nothing yet.
		if (Error == SE_NO_ERROR || Error == SE_EWOULDBLOCK)
		{
			return TEXT("the relay closed the connection");
		}
		return FString::Printf(TEXT("the connection to the relay broke (%s)"), *FFlockRelayFailure::OneLine(Sockets->GetSocketError(Error)));
	}

	/**
	 * Why a connection that ended in an error did: the platform keeps the error for the next call on Unix (a refusal, or a host
	 * or network it could not reach), and Windows answers that call "not connected" whatever the cause, which is a refusal as
	 * often as not.
	 */
	FString WhyTheConnectionFailed(FSocket& Socket)
	{
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		uint8 Byte = 0;
		int32 Read = 0;
		Socket.Recv(&Byte, 1, Read);
		const ESocketErrors Error = Sockets != nullptr ? Sockets->GetLastErrorCode() : SE_NO_ERROR;
		if (Error == SE_ECONNREFUSED || Error == SE_ENOTCONN || Error == SE_NO_ERROR || Error == SE_EWOULDBLOCK)
		{
			return TEXT("refused the connection");
		}
		return FString::Printf(TEXT("could not be connected to (%s)"), *FFlockRelayFailure::OneLine(Sockets->GetSocketError(Error)));
	}

	enum class EConnected : uint8 { Yes, Failed, TimedOut, Stopped };

	EConnected WaitToConnect(FSocket& Socket, double WaitSeconds, TFunctionRef<bool()> ShouldStop)
	{
		const double Until = FPlatformTime::Seconds() + WaitSeconds;
		while (true)
		{
			if (ShouldStop())
			{
				return EConnected::Stopped;
			}
			// A failure shows as an error on the socket (Windows never makes a refused one writable); a connection as writable.
			if (Socket.GetConnectionState() == SCS_ConnectionError)
			{
				return EConnected::Failed;
			}
			if (Socket.Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromSeconds(SliceSeconds)))
			{
				return Socket.GetConnectionState() == SCS_ConnectionError ? EConnected::Failed : EConnected::Yes;
			}
			if (FPlatformTime::Seconds() >= Until)
			{
				return EConnected::TimedOut;
			}
		}
	}

	/** Sends Bytes whole, waiting while the network's buffer is full: false with why once the connection broke, or with no why once ShouldGiveUp says so. */
	bool SendWholeTo(FSocket& Socket, const uint8* Bytes, int32 Count, TFunctionRef<bool()> ShouldGiveUp, FString& OutBroke)
	{
		OutBroke.Reset();
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		int32 Sent = 0;
		while (Sent < Count)
		{
			if (ShouldGiveUp())
			{
				return false;
			}
			int32 Now = 0;
			if (Socket.Send(Bytes + Sent, Count - Sent, Now) && Now > 0)
			{
				Sent += Now;
				continue;
			}
			const ESocketErrors Error = Sockets != nullptr ? Sockets->GetLastErrorCode() : SE_NO_ERROR;
			if (Error != SE_EWOULDBLOCK && Error != SE_NO_ERROR)
			{
				OutBroke = FString::Printf(TEXT("the connection to the relay broke (%s)"), *FFlockRelayFailure::OneLine(Sockets->GetSocketError(Error)));
				return false;
			}
			// The network's buffer is full: wait for room, a slice at a time.
			Socket.Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromSeconds(SliceSeconds));
		}
		return true;
	}

	/** Takes the TLS handshake to its end within WaitSeconds: false with why (none when ShouldStop said so). */
	bool HandshakeWithin(FSocket& Socket, FFlockRelayTls& Tls, double WaitSeconds, TFunctionRef<bool()> ShouldStop, FString& OutCause)
	{
		const double Until = FPlatformTime::Seconds() + WaitSeconds;
		const auto GiveUp = [&ShouldStop, Until]() { return ShouldStop() || FPlatformTime::Seconds() >= Until; };
		TArray<uint8> ForTheNetwork;
		uint8 Chunk[4096];
		while (true)
		{
			if (ShouldStop())
			{
				OutCause.Reset();
				return false;
			}
			FString Why;
			const FFlockRelayTls::EStep Step = Tls.Handshake(Why);
			ForTheNetwork.Reset();
			Tls.TakeForTheNetwork(ForTheNetwork);
			FString Broke;
			if (ForTheNetwork.Num() > 0 && !SendWholeTo(Socket, ForTheNetwork.GetData(), ForTheNetwork.Num(), GiveUp, Broke) && !Broke.IsEmpty())
			{
				OutCause = FString::Printf(TEXT("broke the connection during the TLS handshake (%s)"), *Broke);
				return false;
			}
			if (Step == FFlockRelayTls::EStep::Done)
			{
				return true;
			}
			if (Step == FFlockRelayTls::EStep::Failed)
			{
				OutCause = Why;
				return false;
			}
			if (FPlatformTime::Seconds() >= Until)
			{
				OutCause = ShouldStop() ? FString() : FString::Printf(TEXT("did not finish the TLS handshake within %.1f s"), WaitSeconds);
				return false;
			}
			if (Socket.Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromSeconds(SliceSeconds)))
			{
				int32 Read = 0;
				if (!Socket.Recv(Chunk, UE_ARRAY_COUNT(Chunk), Read))
				{
					OutCause = FString::Printf(TEXT("ended the TLS handshake: %s"), *WhyTheReadFailed());
					return false;
				}
				Tls.PutArrived(Chunk, Read);
			}
		}
	}

	void PutInRing(TArray<uint8>& Ring, int32 First, int32& Count, const uint8* From, int32 Length)
	{
		const int32 Room = Ring.Num();
		const int32 At = (First + Count) % Room;
		const int32 ToEnd = FMath::Min(Length, Room - At);
		FMemory::Memcpy(Ring.GetData() + At, From, ToEnd);
		FMemory::Memcpy(Ring.GetData(), From + ToEnd, Length - ToEnd);
		Count += Length;
	}

	void TakeFromRing(const TArray<uint8>& Ring, int32& First, int32& Count, uint8* Into, int32 Length)
	{
		const int32 Room = Ring.Num();
		const int32 ToEnd = FMath::Min(Length, Room - First);
		FMemory::Memcpy(Into, Ring.GetData() + First, ToEnd);
		FMemory::Memcpy(Into + ToEnd, Ring.GetData(), Length - ToEnd);
		First = (First + Length) % Room;
		Count -= Length;
	}
}

std::atomic<bool> FFlockRelayStream::bHoldsSendsForTesting{ false };

TSharedPtr<FFlockRelayStream, ESPMode::ThreadSafe> FFlockRelayStream::Open(const FString& Address, int32 Port, const FString& Host, bool bOverTls,
	double WaitSeconds, TFunctionRef<bool()> ShouldStop, FString& OutCause, bool& bOutNoThread)
{
	OutCause.Reset();
	bOutNoThread = false;
	FString Why;
	FSocket* Socket = FFlockSockets::StartStreamTo(Address, Port, TEXT("Flock relay stream"), Why);
	if (Socket == nullptr)
	{
		OutCause = FString::Printf(TEXT("could not be connected to (%s)"), *Why);
		return nullptr;
	}
	const EConnected Connected = WaitToConnect(*Socket, WaitSeconds, ShouldStop);
	if (Connected != EConnected::Yes)
	{
		if (Connected == EConnected::Failed)
		{
			OutCause = WhyTheConnectionFailed(*Socket);
		}
		else if (Connected == EConnected::TimedOut)
		{
			OutCause = FString::Printf(TEXT("did not answer within %.1f s"), WaitSeconds);
		}
		FFlockSockets::Close(Socket);
		return nullptr;
	}
	TUniquePtr<FFlockRelayTls> Tls;
	if (bOverTls)
	{
		Tls = FFlockRelayTls::Start(Host, Why);
		if (!Tls.IsValid() || !HandshakeWithin(*Socket, *Tls, WaitSeconds, ShouldStop, Why))
		{
			OutCause = Why;
			FFlockSockets::Close(Socket);
			return nullptr;
		}
	}
	const TSharedRef<FFlockRelayStream, ESPMode::ThreadSafe> Stream = MakeShareable(new FFlockRelayStream(Socket, MoveTemp(Tls)));
	if (!FFlockRelayThreads::Start(Stream, SendingThreadName, SendingThreadStackBytes))
	{
		bOutNoThread = true;
		OutCause = TEXT("could not be used: no thread could be started to send to it, as in a launch of the game without threads");
		Stream->Close();
		return nullptr;
	}
	return Stream;
}

FFlockRelayStream::FFlockRelayStream(FSocket* InSocket, TUniquePtr<FFlockRelayTls> InTls)
	: Socket(InSocket)
	, LocalPort(InSocket->GetPortNo())
	, Protocol(InSocket->GetProtocol())
	, Tls(MoveTemp(InTls))
	, WakeUp(FPlatformProcess::GetSynchEventFromPool(false))
{
	Arrived.SetNumUninitialized(ArrivedRoom);
	ReadChunk.SetNumUninitialized(ChunkBytes);
	Sending.SetNumUninitialized(SendingRoom);
}

FFlockRelayStream::~FFlockRelayStream()
{
	// Only once neither thread uses it: the relay's thread let go of it, and the sending thread ended.
	FFlockSockets::Close(Socket);
	FPlatformProcess::ReturnSynchEventToPool(WakeUp);
}

FString FFlockRelayStream::WhyTheConnectionEnded() const
{
	FScopeLock Locked(&EndedLock);
	return EndedBecause;
}

bool FFlockRelayStream::WaitToRead(double Seconds)
{
	if (WholeMessageLength() != 0)
	{
		return true;
	}
	TakeFromTheNetwork();
	if (WholeMessageLength() != 0)
	{
		return true;
	}
	if (bConnectionEnded || bClosed)
	{
		// A socket that ended reads as ready at once: the relay's thread is not to spin on it while it gives its address back.
		FPlatformProcess::Sleep(static_cast<float>(Seconds));
		return false;
	}
	if (!Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromSeconds(Seconds)))
	{
		return false;
	}
	TakeFromTheNetwork();
	return WholeMessageLength() != 0;
}

bool FFlockRelayStream::Receive(uint8* Into, int32 Size, int32& OutRead)
{
	OutRead = 0;
	while (true)
	{
		int32 Length = WholeMessageLength();
		if (Length == 0)
		{
			TakeFromTheNetwork();
			Length = WholeMessageLength();
			if (Length == 0)
			{
				return false;
			}
		}
		if (Length < 0)
		{
			// Not a relay message: nothing after it can be told apart, so the stream is done with.
			ArrivedCount = 0;
			ArrivedFirst = 0;
			End(TEXT("the relay sent something that is not a relay message"));
			return false;
		}
		if (Length <= Size)
		{
			FMemory::Memcpy(Into, Arrived.GetData() + ArrivedFirst, Length);
			OutRead = Length;
		}
		// A message larger than the reader's room is passed over.
		ArrivedFirst += Length;
		ArrivedCount -= Length;
		if (ArrivedCount == 0)
		{
			ArrivedFirst = 0;
		}
		if (OutRead > 0)
		{
			return true;
		}
	}
}

bool FFlockRelayStream::TrySend(const uint8* Bytes, int32 Count)
{
	if (Bytes == nullptr || Count <= 0)
	{
		return false;
	}
	{
		FScopeLock Locked(&SendingLock);
		// Whole or not at all: part of a message would leave every later one misread.
		if (bClosed || bConnectionEnded || SendingRoom - SendingCount < Count)
		{
			return false;
		}
		PutInRing(Sending, SendingFirst, SendingCount, Bytes, Count);
	}
	WakeUp->Trigger();
	return true;
}

void FFlockRelayStream::Close()
{
	if (bClosed.exchange(true))
	{
		return;
	}
	WakeUp->Trigger();
	// Ends the connection both ways (the relay ends the reservation with it) and keeps the socket's handle until neither thread
	// uses it, so the sending thread's next call fails rather than meets another socket given the same handle.
	Socket->Shutdown(ESocketShutdownMode::ReadWrite);
}

uint32 FFlockRelayStream::Run()
{
	TArray<uint8> Taken;
	Taken.SetNumUninitialized(ChunkBytes);
	TArray<uint8> ForTheNetwork;
	ForTheNetwork.Reserve(2 * ChunkBytes);
	while (!IsStoppingToSend())
	{
		if (bHoldsSendsForTesting)
		{
			WakeUp->Wait(10);
			continue;
		}
		int32 Count = 0;
		{
			FScopeLock Locked(&SendingLock);
			Count = FMath::Min(SendingCount, ChunkBytes);
			TakeFromRing(Sending, SendingFirst, SendingCount, Taken.GetData(), Count);
		}
		ForTheNetwork.Reset();
		if (Tls.IsValid())
		{
			// Encrypted under the lock the relay's thread decrypts under, and taken with whatever TLS has for the relay from that side
			// (its answers to what the relay sent), so records leave in the order TLS made them.
			FString Why;
			bool bBroke = false;
			{
				FScopeLock Locked(&TlsLock);
				bBroke = Count > 0 && !Tls->WritePlain(Taken.GetData(), Count, Why);
				Tls->TakeForTheNetwork(ForTheNetwork);
			}
			if (bBroke)
			{
				End(FString::Printf(TEXT("the connection to the relay broke (%s)"), *Why));
				break;
			}
		}
		else if (Count > 0)
		{
			ForTheNetwork.Append(Taken.GetData(), Count);
		}
		if (ForTheNetwork.Num() > 0)
		{
			FString Broke;
			if (!SendWholeTo(*Socket, ForTheNetwork.GetData(), ForTheNetwork.Num(), [this]() { return IsStoppingToSend(); }, Broke))
			{
				if (!Broke.IsEmpty())
				{
					End(Broke);
				}
				break;
			}
			continue;
		}
		WakeUp->Wait(SendingIdleMilliseconds);
	}
	bSendingDone = true;
	return 0;
}

void FFlockRelayStream::EndNow()
{
	bQuitNow = true;
	WakeUp->Trigger();
}

void FFlockRelayStream::TakeFromTheNetwork()
{
	if (bClosed || bConnectionEnded)
	{
		return;
	}
	for (int32 Turn = 0; Turn < 16; ++Turn)
	{
		// Room after what arrived, made by moving what is unread to the front.
		if (ArrivedFirst > 0 && ArrivedFirst + ArrivedCount + ChunkBytes > ArrivedRoom)
		{
			FMemory::Memmove(Arrived.GetData(), Arrived.GetData() + ArrivedFirst, ArrivedCount);
			ArrivedFirst = 0;
		}
		uint8* Tail = Arrived.GetData() + ArrivedFirst + ArrivedCount;
		const int32 Room = ArrivedRoom - (ArrivedFirst + ArrivedCount);
		if (Room <= 0)
		{
			// A reader behind by the whole room holds the stream back, as TCP holds back its sender; nothing is dropped.
			return;
		}
		if (Tls.IsValid())
		{
			// What TLS decrypted already comes first, then more from the network.
			FString Why;
			int32 Plain = 0;
			bool bHasBytesForTheNetwork = false;
			{
				FScopeLock Locked(&TlsLock);
				Plain = Tls->ReadPlain(Tail, Room, Why);
				bHasBytesForTheNetwork = Tls->HasBytesForTheNetwork();
			}
			if (bHasBytesForTheNetwork)
			{
				WakeUp->Trigger();
			}
			if (Plain < 0)
			{
				End(FString::Printf(TEXT("the connection to the relay broke (%s)"), *Why));
				return;
			}
			if (Plain > 0)
			{
				ArrivedCount += Plain;
				continue;
			}
			int32 Read = 0;
			if (!Socket->Recv(ReadChunk.GetData(), ReadChunk.Num(), Read))
			{
				End(WhyTheReadFailed());
				return;
			}
			if (Read <= 0)
			{
				return;
			}
			FScopeLock Locked(&TlsLock);
			Tls->PutArrived(ReadChunk.GetData(), Read);
			continue;
		}
		int32 Read = 0;
		if (!Socket->Recv(Tail, Room, Read))
		{
			End(WhyTheReadFailed());
			return;
		}
		if (Read <= 0)
		{
			return;
		}
		ArrivedCount += Read;
	}
}

int32 FFlockRelayStream::WholeMessageLength() const
{
	if (ArrivedCount < FFlockRelayWire::ChannelFrameHeaderLength)
	{
		return 0;
	}
	const uint8* At = Arrived.GetData() + ArrivedFirst;
	const int32 Length = (static_cast<int32>(At[2]) << 8) | At[3];
	int32 Whole = 0;
	if ((At[0] & 0xC0) == 0x40)
	{
		// A channel frame: its header, its data, and the padding to 4 bytes a stream carries.
		Whole = FFlockRelayWire::ChannelFrameHeaderLength + Length + (4 - Length % 4) % 4;
	}
	else if ((At[0] & 0xC0) == 0)
	{
		// A STUN message: its header and the length it states.
		Whole = FFlockRelayWire::HeaderLength + Length;
	}
	else
	{
		return -1;
	}
	return ArrivedCount >= Whole ? Whole : 0;
}

void FFlockRelayStream::End(const FString& Reason)
{
	if (bClosed)
	{
		return;
	}
	{
		FScopeLock Locked(&EndedLock);
		if (bConnectionEnded)
		{
			return;
		}
		EndedBecause = Reason;
		bConnectionEnded = true;
	}
	WakeUp->Trigger();
}
