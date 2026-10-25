// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/FlockNameLookup.h"
#include "Async/Async.h"
#include "IPAddress.h"
#include "Multiplayer/FlockDirectAddresses.h"
#include "SocketSubsystem.h"

#if PLATFORM_HAS_BSD_IPV6_SOCKETS
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
THIRD_PARTY_INCLUDES_START
#include <winsock2.h>
#include <ws2tcpip.h>
THIRD_PARTY_INCLUDES_END
#include "Windows/HideWindowsPlatformTypes.h"
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#endif

namespace
{
	/**
	 * Every IPv6 address of Host, from the platform's own resolver. The engine's lookup drops every IPv6 answer while its
	 * net.DisableIPv6 is 1, its default on every engine from 5.4 to 5.8 (iOS's config alone sets 0), so a relay server's IPv6
	 * address is asked of the platform here; the game's setting and the engine's own networking are left as they are. Blocks.
	 */
	TArray<FString> Ipv6AddressesOf(const FString& Host)
	{
		TArray<FString> Found;
#if PLATFORM_HAS_BSD_IPV6_SOCKETS
		addrinfo Hints;
		FMemory::Memzero(&Hints, sizeof(Hints));
		Hints.ai_family = AF_INET6;
		Hints.ai_socktype = SOCK_DGRAM;
		addrinfo* Results = nullptr;
		if (getaddrinfo(TCHAR_TO_UTF8(*Host), "0", &Hints, &Results) == 0)
		{
			for (const addrinfo* Each = Results; Each != nullptr; Each = Each->ai_next)
			{
				if (Each->ai_family != AF_INET6 || Each->ai_addr == nullptr)
				{
					continue;
				}
				char Text[INET6_ADDRSTRLEN] = {};
				const sockaddr_in6* Address = reinterpret_cast<const sockaddr_in6*>(Each->ai_addr);
				if (inet_ntop(AF_INET6, &Address->sin6_addr, Text, sizeof(Text)) != nullptr)
				{
					Found.Add(UTF8_TO_TCHAR(Text));
				}
			}
			freeaddrinfo(Results);
		}
#endif
		return Found;
	}

	/** Address as text of the family asked, or empty: IPv4 as a.b.c.d, IPv6 as written (an IPv4 one inside IPv6 is IPv4, not IPv6). */
	FString OfTheFamily(const FString& Address, bool bIpv6)
	{
		if (!bIpv6)
		{
			return FFlockDirectAddresses::Ipv4Value(Address);
		}
		return Address.Contains(TEXT(":")) && !Address.StartsWith(TEXT("::ffff:"), ESearchCase::IgnoreCase) && !Address.Contains(TEXT(" "))
			? Address
			: FString();
	}
}

TSharedRef<FFlockNameLookup, ESPMode::ThreadSafe> FFlockNameLookup::Start(const FString& Host, const FLookUp& LookUpForTesting, bool bIpv6)
{
	const TSharedRef<FFlockNameLookup, ESPMode::ThreadSafe> LookUp = MakeShareable(new FFlockNameLookup(bIpv6));
	if (!FFlockDirectAddresses::Ipv4Value(Host).IsEmpty())
	{
		// An address as written is taken as it is: an IPv4 server has no IPv6 address to find.
		LookUp->Answer(bIpv6 ? FString() : FFlockDirectAddresses::Ipv4Value(Host));
		return LookUp;
	}
	if (Host.IsEmpty() || Host.Contains(TEXT(":")) || Host.Contains(TEXT(" ")))
	{
		// An IPv6 address as the host (Flock lists names, and its relay addresses are read without one), or no name at all.
		LookUp->Answer(FString());
		return LookUp;
	}
	TFunction<void(const TArray<FString>&)> Answer = [LookUp](const TArray<FString>& Addresses)
	{
		LookUp->AnswerWithTheFirstOf(Addresses);
	};
	if (LookUpForTesting)
	{
		LookUpForTesting(Host, MoveTemp(Answer));
		return LookUp;
	}
	if (bIpv6)
	{
		// On one of the engine's threads, answered from there; whoever waits on it bounds the wait, as for the engine's lookup.
		const FString Name = Host;
		Async(EAsyncExecution::ThreadPool, [Name, Answer]() { Answer(Ipv6AddressesOf(Name)); });
		return LookUp;
	}
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (Sockets == nullptr)
	{
		LookUp->Answer(FString());
		return LookUp;
	}
	// Answered on one of the engine's threads. The service is a number, never absent: the engine's lookup on another thread passes
	// an absent one as empty, which Android's resolver refuses for a datagram lookup (EAI_SERVICE), so no name was looked up there.
	Sockets->GetAddressInfoAsync([Answer](FAddressInfoResult Result)
	{
		TArray<FString> Addresses;
		for (const FAddressInfoResultData& Found : Result.Results)
		{
			Addresses.Add(Found.Address->ToString(false));
		}
		Answer(Addresses);
	}, *Host, TEXT("0"), EAddressInfoFlags::Default, FNetworkProtocolTypes::IPv4, SOCKTYPE_Datagram);
	return LookUp;
}

bool FFlockNameLookup::IsDone() const
{
	FScopeLock Locked(&Lock);
	return bDone;
}

FString FFlockNameLookup::GetAddress() const
{
	FScopeLock Locked(&Lock);
	return Address;
}

void FFlockNameLookup::AnswerWithTheFirstOf(const TArray<FString>& Found)
{
	for (const FString& Each : Found)
	{
		const FString First = OfTheFamily(Each, bIpv6);
		if (!First.IsEmpty())
		{
			Answer(First);
			return;
		}
	}
	Answer(FString());
}

void FFlockNameLookup::Answer(const FString& Found)
{
	FScopeLock Locked(&Lock);
	if (bDone)
	{
		return;
	}
	bDone = true;
	Address = Found;
}
