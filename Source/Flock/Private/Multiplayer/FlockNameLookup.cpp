// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/FlockNameLookup.h"
#include "IPAddress.h"
#include "Multiplayer/FlockDirectAddresses.h"
#include "SocketSubsystem.h"

TSharedRef<FFlockNameLookup, ESPMode::ThreadSafe> FFlockNameLookup::Start(const FString& Host, const FLookUp& LookUpForTesting)
{
	const TSharedRef<FFlockNameLookup, ESPMode::ThreadSafe> LookUp = MakeShared<FFlockNameLookup, ESPMode::ThreadSafe>();
	const FString Written = FFlockDirectAddresses::Ipv4Value(Host);
	if (!Written.IsEmpty())
	{
		// An address as written is taken as it is.
		LookUp->Answer(Written);
		return LookUp;
	}
	if (Host.IsEmpty() || Host.Contains(TEXT(":")) || Host.Contains(TEXT(" ")))
	{
		// An IPv6 address, or no name at all: the request is plain UDP to IPv4.
		LookUp->Answer(FString());
		return LookUp;
	}
	TFunction<void(const FString&)> Answer = [LookUp](const FString& Ipv4)
	{
		LookUp->Answer(FFlockDirectAddresses::Ipv4Value(Ipv4));
	};
	if (LookUpForTesting)
	{
		LookUpForTesting(Host, MoveTemp(Answer));
		return LookUp;
	}
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (Sockets == nullptr)
	{
		LookUp->Answer(FString());
		return LookUp;
	}
	// Answered on one of the engine's threads.
	Sockets->GetAddressInfoAsync([Answer](FAddressInfoResult Result)
	{
		FString Ipv4;
		for (const FAddressInfoResultData& Found : Result.Results)
		{
			Ipv4 = FFlockDirectAddresses::Ipv4Value(Found.Address->ToString(false));
			if (!Ipv4.IsEmpty())
			{
				break;
			}
		}
		Answer(Ipv4);
	}, *Host, nullptr, EAddressInfoFlags::Default, FNetworkProtocolTypes::IPv4, SOCKTYPE_Datagram);
	return LookUp;
}

bool FFlockNameLookup::IsDone() const
{
	FScopeLock Locked(&Lock);
	return bDone;
}

FString FFlockNameLookup::GetIpv4() const
{
	FScopeLock Locked(&Lock);
	return Ipv4;
}

void FFlockNameLookup::Answer(const FString& Found)
{
	FScopeLock Locked(&Lock);
	if (bDone)
	{
		return;
	}
	bDone = true;
	Ipv4 = Found;
}
