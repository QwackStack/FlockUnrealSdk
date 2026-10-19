// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/Relay/FlockBridgeMessages.h"
#include "Misc/Guid.h"

namespace
{
	constexpr int32 AddressLength = 6;
	/** An offer's two keys, then how many addresses follow. */
	constexpr int32 OfferAddressesAt = FFlockBridgeMessages::HeaderLength + 2 * FFlockBridgeKey::Length + 1;
	constexpr int32 BetweenLength = FFlockBridgeMessages::HeaderLength + 2 * FFlockBridgeKey::Length;
	constexpr int32 NewConnectionLength = FFlockBridgeMessages::HeaderLength + 4;

	bool HasHeader(const uint8* Data, int32 Count, uint8 Kind)
	{
		return FFlockBridgeMessages::KindOf(Data, Count) == Kind;
	}

	bool SameKeyAt(const uint8* Data, const FFlockBridgeKey& Key)
	{
		return FMemory::Memcmp(Data, Key.Bytes, FFlockBridgeKey::Length) == 0;
	}
}

FFlockBridgeKey FFlockBridgeKey::NewRandom()
{
	// Each platform makes a GUID from its own random source (Windows, Unix and Apple read the system's); its first eight bytes
	// carry four fixed bits at most.
	const FGuid Random = FGuid::NewGuid();
	FFlockBridgeKey Key;
	const uint32 Parts[2] = { Random.A, Random.B ^ Random.D };
	for (int32 Index = 0; Index < Length; ++Index)
	{
		Key.Bytes[Index] = static_cast<uint8>(Parts[Index / 4] >> (8 * (3 - Index % 4)));
	}
	return Key;
}

bool FFlockBridgeMessages::IsBridgeMessage(const uint8* Data, int32 Count)
{
	return Data != nullptr && Count >= 4 && Data[0] == Marker && Data[1] == 'L' && Data[2] == 'N' && Data[3] == 'K';
}

uint8 FFlockBridgeMessages::KindOf(const uint8* Data, int32 Count)
{
	return IsBridgeMessage(Data, Count) && Count >= HeaderLength && Data[4] == Version ? Data[5] : 0;
}

void FFlockBridgeMessages::WriteHeader(TArray<uint8>& Message, uint8 Kind)
{
	Message[0] = Marker;
	Message[1] = 'L';
	Message[2] = 'N';
	Message[3] = 'K';
	Message[4] = Version;
	Message[5] = Kind;
}

TArray<uint8> FFlockBridgeMessages::WriteOffer(const FFlockBridgeKey& SendersKey, const TOptional<FFlockBridgeKey>& ReceiversKey,
	const TArray<FFlockRelayPeer>& Addresses)
{
	const int32 Offered = FMath::Min(Addresses.Num(), MostAddressesOffered);
	TArray<uint8> Message;
	Message.SetNumZeroed(OfferAddressesAt + Offered * AddressLength);
	WriteHeader(Message, Offer);
	FMemory::Memcpy(Message.GetData() + HeaderLength, SendersKey.Bytes, FFlockBridgeKey::Length);
	if (ReceiversKey.IsSet())
	{
		FMemory::Memcpy(Message.GetData() + HeaderLength + FFlockBridgeKey::Length, ReceiversKey.GetValue().Bytes, FFlockBridgeKey::Length);
	}
	Message[OfferAddressesAt - 1] = static_cast<uint8>(Offered);
	for (int32 Index = 0; Index < Offered; ++Index)
	{
		uint8* At = Message.GetData() + OfferAddressesAt + Index * AddressLength;
		const uint32 Address = Addresses[Index].Address;
		At[0] = static_cast<uint8>(Address >> 24);
		At[1] = static_cast<uint8>(Address >> 16);
		At[2] = static_cast<uint8>(Address >> 8);
		At[3] = static_cast<uint8>(Address);
		At[4] = static_cast<uint8>(Addresses[Index].Port >> 8);
		At[5] = static_cast<uint8>(Addresses[Index].Port);
	}
	return Message;
}

bool FFlockBridgeMessages::TryReadOffer(const uint8* Data, int32 Count, FFlockBridgeKey& OutSendersKey, TOptional<FFlockBridgeKey>& OutReceiversKey,
	TArray<FFlockRelayPeer>& OutAddresses)
{
	OutReceiversKey.Reset();
	OutAddresses.Reset();
	if (!HasHeader(Data, Count, Offer) || Count < OfferAddressesAt)
	{
		return false;
	}
	const int32 Offered = Data[OfferAddressesAt - 1];
	if (Offered < 1 || Offered > MostAddressesOffered || Count < OfferAddressesAt + Offered * AddressLength)
	{
		return false;
	}
	FMemory::Memcpy(OutSendersKey.Bytes, Data + HeaderLength, FFlockBridgeKey::Length);
	const uint8* Held = Data + HeaderLength + FFlockBridgeKey::Length;
	for (int32 Index = 0; Index < FFlockBridgeKey::Length; ++Index)
	{
		if (Held[Index] != 0)
		{
			FFlockBridgeKey Receivers;
			FMemory::Memcpy(Receivers.Bytes, Held, FFlockBridgeKey::Length);
			OutReceiversKey = Receivers;
			break;
		}
	}
	for (int32 Index = 0; Index < Offered; ++Index)
	{
		const uint8* At = Data + OfferAddressesAt + Index * AddressLength;
		const int32 Port = (At[4] << 8) | At[5];
		if (Port == 0)
		{
			OutAddresses.Reset();
			OutReceiversKey.Reset();
			return false;
		}
		OutAddresses.Emplace((uint32(At[0]) << 24) | (uint32(At[1]) << 16) | (uint32(At[2]) << 8) | uint32(At[3]), Port);
	}
	return true;
}

TArray<uint8> FFlockBridgeMessages::WriteBetween(uint8 Kind, const FFlockBridgeKey& ReceiversKey, const FFlockBridgeKey& SendersKey)
{
	TArray<uint8> Message;
	Message.SetNumZeroed(BetweenLength);
	WriteHeader(Message, Kind);
	FMemory::Memcpy(Message.GetData() + HeaderLength, ReceiversKey.Bytes, FFlockBridgeKey::Length);
	FMemory::Memcpy(Message.GetData() + HeaderLength + FFlockBridgeKey::Length, SendersKey.Bytes, FFlockBridgeKey::Length);
	return Message;
}

bool FFlockBridgeMessages::IsBetween(const uint8* Data, int32 Count, const FFlockBridgeKey& ReceiversKey, const FFlockBridgeKey& SendersKey)
{
	return KindOf(Data, Count) != 0 && Count >= BetweenLength && SameKeyAt(Data + HeaderLength, ReceiversKey)
		&& SameKeyAt(Data + HeaderLength + FFlockBridgeKey::Length, SendersKey);
}

TArray<uint8> FFlockBridgeMessages::WriteNewConnection(uint32 Number)
{
	TArray<uint8> Message;
	Message.SetNumZeroed(NewConnectionLength);
	WriteHeader(Message, NewConnection);
	Message[HeaderLength] = static_cast<uint8>(Number >> 24);
	Message[HeaderLength + 1] = static_cast<uint8>(Number >> 16);
	Message[HeaderLength + 2] = static_cast<uint8>(Number >> 8);
	Message[HeaderLength + 3] = static_cast<uint8>(Number);
	return Message;
}

bool FFlockBridgeMessages::TryReadNewConnection(const uint8* Data, int32 Count, uint32& OutNumber)
{
	if (!HasHeader(Data, Count, NewConnection) || Count < NewConnectionLength)
	{
		return false;
	}
	const uint8* At = Data + HeaderLength;
	OutNumber = (uint32(At[0]) << 24) | (uint32(At[1]) << 16) | (uint32(At[2]) << 8) | uint32(At[3]);
	return true;
}
