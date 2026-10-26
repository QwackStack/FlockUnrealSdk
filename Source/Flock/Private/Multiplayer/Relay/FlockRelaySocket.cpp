// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/Relay/FlockRelaySocket.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Multiplayer/FlockDirectAddresses.h"
#include "Multiplayer/FlockSockets.h"
#include "Multiplayer/Relay/FlockRelayStream.h"
#include "Multiplayer/Relay/FlockRelayWire.h"
#include "Sockets.h"

namespace
{
	/** How often the search looks at its sockets. */
	constexpr float SearchPollSeconds = 0.01f;

	void SendBindings(const TArray<FSocket*>& Sockets, const TArray<TArray<uint8>>& Transactions)
	{
		for (int32 Index = 0; Index < Sockets.Num(); ++Index)
		{
			const TArray<uint8> Request = FFlockDirectAddresses::StunRequest(Transactions[Index]);
			int32 Sent = 0;
			// A socket that refuses the send is not heard; the others may be.
			Sockets[Index]->Send(Request.GetData(), Request.Num(), Sent);
		}
	}
}

FFlockRelaySocket::~FFlockRelaySocket()
{
	Close();
}

FSocket* FFlockRelaySocket::FindOneTheServerAnswers(const FString& ServerAddress, int32 Port, int32 SocketsAtOnce, int32 Rounds, double RoundWaitSeconds,
	TFunctionRef<bool()> ShouldStop)
{
	uint8 Received[2048];
	for (int32 Round = 0; Round < Rounds; ++Round)
	{
		TArray<FSocket*> Trying;
		TArray<TArray<uint8>> Transactions;
		for (int32 Index = 0; Index < SocketsAtOnce; ++Index)
		{
			// A socket that cannot be made or aimed is left out; with none, the caller's own socket says why.
			if (FSocket* Socket = FFlockSockets::OpenConnectedTo(ServerAddress, Port, TEXT("Flock relay search")))
			{
				Trying.Add(Socket);
				Transactions.Add(FFlockRelayWire::NewTransaction());
			}
		}
		if (Trying.Num() == 0)
		{
			return nullptr;
		}
		FSocket* Kept = nullptr;
		const double Started = FPlatformTime::Seconds();
		SendBindings(Trying, Transactions);
		bool bSentAgain = false;
		while (Kept == nullptr && FPlatformTime::Seconds() - Started < RoundWaitSeconds && !ShouldStop())
		{
			if (!bSentAgain && FPlatformTime::Seconds() - Started >= RoundWaitSeconds / 2)
			{
				// A request or its answer may be lost on the way.
				SendBindings(Trying, Transactions);
				bSentAgain = true;
			}
			FPlatformProcess::Sleep(SearchPollSeconds);
			for (FSocket* Socket : Trying)
			{
				int32 Read = 0;
				// A connected socket hears the server alone, so anything that arrives is its answer; a failed read is an earlier
				// send that met a closed port.
				if (Socket->Recv(Received, UE_ARRAY_COUNT(Received), Read) && Read > 0)
				{
					Kept = Socket;
					break;
				}
			}
		}
		for (FSocket* Socket : Trying)
		{
			if (Socket != Kept)
			{
				FFlockSockets::Close(Socket);
			}
		}
		if (Kept != nullptr || ShouldStop())
		{
			return Kept;
		}
	}
	return nullptr;
}

void FFlockRelaySocket::Attach(FSocket* Connected)
{
	FScopeLock Locked(&Lock);
	if (bClosed || Connected == nullptr)
	{
		FFlockSockets::Close(Connected);
		return;
	}
	Socket = Connected;
	Protocol = Socket->GetProtocol();
	LocalPort = FFlockSockets::LocalPortOf(*Socket);
}

void FFlockRelaySocket::AttachStream(const TSharedRef<FFlockRelayStream, ESPMode::ThreadSafe>& Opened)
{
	FScopeLock Locked(&Lock);
	if (bClosed)
	{
		Opened->Close();
		return;
	}
	Stream = Opened;
	Protocol = Opened->GetProtocol();
	LocalPort = Opened->GetLocalPort();
}

bool FFlockRelaySocket::HasTheConnectionEnded() const
{
	FScopeLock Locked(&Lock);
	return Stream.IsValid() && Stream->HasTheConnectionEnded();
}

FString FFlockRelaySocket::WhyTheConnectionEnded() const
{
	FScopeLock Locked(&Lock);
	return Stream.IsValid() ? Stream->WhyTheConnectionEnded() : FString();
}

bool FFlockRelaySocket::TrySend(const uint8* Bytes, int32 Count)
{
	FScopeLock Locked(&Lock);
	if (Stream.IsValid())
	{
		return Stream->TrySend(Bytes, Count);
	}
	if (Socket == nullptr)
	{
		return false;
	}
	int32 Sent = 0;
	if (!Socket->Send(Bytes, Count, Sent) || Sent != Count)
	{
		return false;
	}
	return true;
}

bool FFlockRelaySocket::WaitToRead(double Seconds)
{
	FSocket* Reading = nullptr;
	TSharedPtr<FFlockRelayStream, ESPMode::ThreadSafe> ReadingStream;
	{
		FScopeLock Locked(&Lock);
		Reading = Socket;
		ReadingStream = Stream;
	}
	if (ReadingStream.IsValid())
	{
		return ReadingStream->WaitToRead(Seconds);
	}
	if (Reading == nullptr)
	{
		FPlatformProcess::Sleep(static_cast<float>(Seconds));
		return false;
	}
	// Closed only on this same thread, so the socket outlives the wait.
	return Reading->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromSeconds(Seconds));
}

bool FFlockRelaySocket::Receive(uint8* Into, int32 Size, int32& OutRead)
{
	OutRead = 0;
	FSocket* Reading = nullptr;
	TSharedPtr<FFlockRelayStream, ESPMode::ThreadSafe> ReadingStream;
	{
		FScopeLock Locked(&Lock);
		Reading = Socket;
		ReadingStream = Stream;
	}
	if (ReadingStream.IsValid())
	{
		return ReadingStream->Receive(Into, Size, OutRead);
	}
	return Reading != nullptr && Reading->Recv(Into, Size, OutRead) && OutRead > 0;
}

FName FFlockRelaySocket::GetProtocolForTesting() const
{
	FScopeLock Locked(&Lock);
	return Protocol;
}

void FFlockRelaySocket::Close()
{
	FScopeLock Locked(&Lock);
	bClosed = true;
	FFlockSockets::Close(Socket);
	Socket = nullptr;
	if (Stream.IsValid())
	{
		// Its socket is given back once its sending thread let go of it too.
		Stream->Close();
		Stream.Reset();
	}
}
