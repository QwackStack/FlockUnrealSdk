// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/FlockUdpSockets.h"
#include "IPAddress.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

std::atomic<bool> FFlockUdpSockets::bDualStackRefusedForTesting{ false };
std::atomic<bool> FFlockUdpSockets::bDualStackSendRefusedForTesting{ false };
std::atomic<bool> FFlockUdpSockets::bDualStackConnectRefusedForTesting{ false };
std::atomic<bool> FFlockUdpSockets::bDualStackRefused{ false };

namespace
{
	ISocketSubsystem* FlockSockets()
	{
		return ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	}

	/** Binds to any address on a port of the system's choosing and stops the socket blocking; false leaves the socket to be closed. */
	bool BindAnywhere(ISocketSubsystem& Sockets, FSocket& Socket)
	{
		const TSharedRef<FInternetAddr> Anywhere = Sockets.CreateInternetAddr(Socket.GetProtocol());
		Anywhere->SetAnyAddress();
		Anywhere->SetPort(0);
		return Socket.Bind(*Anywhere) && Socket.SetNonBlocking(true);
	}

	bool ConnectTo(FSocket& Socket, const FString& Ipv4Address, int32 Port)
	{
		if (FFlockUdpSockets::bDualStackConnectRefusedForTesting && FFlockUdpSockets::IsDualStack(Socket))
		{
			return false;
		}
		return Socket.Connect(*FFlockUdpSockets::Toward(Socket, Ipv4Address, Port));
	}
}

void FFlockUdpSockets::ResetForTesting()
{
	bDualStackRefusedForTesting = false;
	bDualStackSendRefusedForTesting = false;
	bDualStackConnectRefusedForTesting = false;
	bDualStackRefused = false;
}

FSocket* FFlockUdpSockets::Open(const TCHAR* Description)
{
	ISocketSubsystem* Sockets = FlockSockets();
	if (Sockets == nullptr)
	{
		return nullptr;
	}
	if (!bDualStackRefused)
	{
		// The desktop and phone socket layers make every IPv6 socket dual-stack (Windows, Unix and iOS say so when making it;
		// Android's IPv6 sockets are dual-stack unless told otherwise).
		FSocket* DualStack = bDualStackRefusedForTesting ? nullptr : Sockets->CreateSocket(NAME_DGram, Description, FNetworkProtocolTypes::IPv6);
		if (DualStack == nullptr)
		{
			// No IPv6 here: plain IPv4 sockets, as before, for the rest of the launch.
			bDualStackRefused = true;
		}
		else if (BindAnywhere(*Sockets, *DualStack))
		{
			return DualStack;
		}
		else
		{
			// A bind that failed this once says nothing of the platform: this socket alone is plain IPv4.
			Sockets->DestroySocket(DualStack);
		}
	}
	return OpenIpv4(Description);
}

FSocket* FFlockUdpSockets::OpenIpv4(const TCHAR* Description)
{
	ISocketSubsystem* Sockets = FlockSockets();
	if (Sockets == nullptr)
	{
		return nullptr;
	}
	FSocket* Ipv4 = Sockets->CreateSocket(NAME_DGram, Description, FNetworkProtocolTypes::IPv4);
	if (Ipv4 == nullptr)
	{
		return nullptr;
	}
	if (!BindAnywhere(*Sockets, *Ipv4))
	{
		Sockets->DestroySocket(Ipv4);
		return nullptr;
	}
	return Ipv4;
}

FSocket* FFlockUdpSockets::OpenOnLoopback(const TCHAR* Description)
{
	ISocketSubsystem* Sockets = FlockSockets();
	FSocket* Socket = Sockets != nullptr ? Sockets->CreateSocket(NAME_DGram, Description, FNetworkProtocolTypes::IPv4) : nullptr;
	if (Socket == nullptr)
	{
		return nullptr;
	}
	bool bValid = false;
	const TSharedRef<FInternetAddr> Loopback = Sockets->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
	Loopback->SetIp(LoopbackIpv4, bValid);
	Loopback->SetPort(0);
	int32 BufferBytes = 0;
	// Room for a burst of the engine's packets while the thread forwarding them waits for its turn.
	Socket->SetReceiveBufferSize(LoopbackBufferBytes, BufferBytes);
	if (!bValid || !Socket->Bind(*Loopback) || !Socket->SetNonBlocking(true))
	{
		Sockets->DestroySocket(Socket);
		return nullptr;
	}
	return Socket;
}

TSharedRef<FInternetAddr> FFlockUdpSockets::OnLoopback(int32 Port)
{
	bool bValid = false;
	const TSharedRef<FInternetAddr> Address = FlockSockets()->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
	Address->SetIp(LoopbackIpv4, bValid);
	Address->SetPort(Port);
	return Address;
}

bool FFlockUdpSockets::IsLoopback(const FInternetAddr& Address)
{
	// 127.0.0.0/8, written as IPv4 or as IPv4 inside IPv6 (::ffff:127.x.y.z), as a dual-stack socket reads it.
	const TArray<uint8> Raw = Address.GetRawIp();
	if (Raw.Num() == 4)
	{
		return Raw[0] == 127;
	}
	if (Raw.Num() == 16)
	{
		for (int32 Index = 0; Index < 10; ++Index)
		{
			if (Raw[Index] != 0)
			{
				return false;
			}
		}
		return Raw[10] == 0xFF && Raw[11] == 0xFF && Raw[12] == 127;
	}
	return false;
}

FSocket* FFlockUdpSockets::OpenConnectedTo(const FString& Ipv4Address, int32 Port, const TCHAR* Description)
{
	FSocket* Socket = Open(Description);
	if (Socket == nullptr)
	{
		return nullptr;
	}
	if (ConnectTo(*Socket, Ipv4Address, Port))
	{
		return Socket;
	}
	const bool bWasDualStack = IsDualStack(*Socket);
	Close(Socket);
	if (!bWasDualStack)
	{
		return nullptr;
	}
	// A platform that makes a dual-stack socket but cannot connect it to an IPv4 address gets a plain IPv4 socket instead.
	FSocket* Ipv4 = OpenIpv4(Description);
	if (Ipv4 != nullptr && !ConnectTo(*Ipv4, Ipv4Address, Port))
	{
		Close(Ipv4);
		return nullptr;
	}
	return Ipv4;
}

void FFlockUdpSockets::Close(FSocket* Socket)
{
	if (Socket == nullptr)
	{
		return;
	}
	Socket->Close();
	if (ISocketSubsystem* Sockets = FlockSockets())
	{
		Sockets->DestroySocket(Socket);
	}
}

bool FFlockUdpSockets::IsDualStack(const FSocket& Socket)
{
	return Socket.GetProtocol() == FNetworkProtocolTypes::IPv6;
}

TSharedRef<FInternetAddr> FFlockUdpSockets::Toward(const FSocket& Socket, const FString& Ipv4Address, int32 Port)
{
	ISocketSubsystem* Sockets = FlockSockets();
	const TSharedRef<FInternetAddr> Address = Sockets->CreateInternetAddr(Socket.GetProtocol());
	bool bValid = false;
	Address->SetIp(*(IsDualStack(Socket) ? TEXT("::ffff:") + Ipv4Address : Ipv4Address), bValid);
	Address->SetPort(Port);
	return Address;
}

TSharedRef<FInternetAddr> FFlockUdpSockets::AnySender(const FSocket& Socket)
{
	return FlockSockets()->CreateInternetAddr(Socket.GetProtocol());
}

int32 FFlockUdpSockets::LocalPortOf(FSocket& Socket)
{
	return Socket.GetPortNo();
}
