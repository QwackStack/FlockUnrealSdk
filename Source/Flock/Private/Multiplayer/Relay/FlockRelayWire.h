// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Multiplayer/Relay/FlockRelayPeer.h"

/** One attribute of a message to write: its kind and value. */
using FFlockRelayAttribute = TPair<uint16, TArray<uint8>>;

/** A STUN message as read: its type, transaction, first attribute of each kind, and where its signature and fingerprint start (INDEX_NONE when absent). */
struct FFlockRelayMessage
{
	uint16 Type = 0;
	TArray<uint8> Transaction;
	TMap<uint16, TArray<uint8>> Attributes;
	int32 IntegrityAt = INDEX_NONE;
	int32 FingerprintAt = INDEX_NONE;
	/** The message itself, up to its stated length. */
	TArray<uint8> Bytes;

	bool TryGetAddress(uint16 Kind, FFlockRelayPeer& OutPeer) const;
	/** An attribute's value as text (UTF-8); false when the message has none of that kind. */
	bool TryGetText(uint16 Kind, FString& OutText) const;
};

/**
 * STUN and TURN messages as Flock's relay speaks them: requests signed with a relay login, answers checked before they are
 * believed, and the four-byte frames a channel carries. Any thread.
 */
class FFlockRelayWire
{
public:
	static constexpr uint32 MagicCookie = 0x2112A442;
	static constexpr int32 HeaderLength = 20;
	static constexpr int32 TransactionLength = 12;
	static constexpr int32 ChannelFrameHeaderLength = 4;
	/** What a send indication adds to a packet: its header, the peer's address and the data attribute's own header. */
	static constexpr int32 SendIndicationOverhead = HeaderLength + 12 + 4;

	static constexpr uint16 BindingMethod = 0x001;
	static constexpr uint16 AllocateMethod = 0x003;
	static constexpr uint16 RefreshMethod = 0x004;
	static constexpr uint16 SendMethod = 0x006;
	static constexpr uint16 DataMethod = 0x007;
	static constexpr uint16 CreatePermissionMethod = 0x008;
	static constexpr uint16 ChannelBindMethod = 0x009;

	static constexpr uint16 RequestClass = 0x000;
	static constexpr uint16 IndicationClass = 0x010;
	static constexpr uint16 SuccessClass = 0x100;
	static constexpr uint16 ErrorClass = 0x110;

	static constexpr uint16 UsernameAttribute = 0x0006;
	static constexpr uint16 MessageIntegrityAttribute = 0x0008;
	static constexpr uint16 ErrorCodeAttribute = 0x0009;
	static constexpr uint16 ChannelNumberAttribute = 0x000C;
	static constexpr uint16 LifetimeAttribute = 0x000D;
	static constexpr uint16 XorPeerAddressAttribute = 0x0012;
	static constexpr uint16 DataAttribute = 0x0013;
	static constexpr uint16 RealmAttribute = 0x0014;
	static constexpr uint16 NonceAttribute = 0x0015;
	static constexpr uint16 XorRelayedAddressAttribute = 0x0016;
	static constexpr uint16 RequestedTransportAttribute = 0x0019;
	static constexpr uint16 XorMappedAddressAttribute = 0x0020;
	static constexpr uint16 FingerprintAttribute = 0x8028;

	/** Channel numbers a client may bind, as the standard sets them. */
	static constexpr uint16 FirstChannel = 0x4000;
	static constexpr uint16 LastChannel = 0x4FFF;

	/** A transaction id no one can guess (from the platform's random source), so only the relay can answer a request. */
	static TArray<uint8> NewTransaction();

	/** A whole message: its attributes, then a signature when Key holds one, then a fingerprint. */
	static TArray<uint8> Write(uint16 Type, const TArray<uint8>& Transaction, const TArray<FFlockRelayAttribute>& Attributes, const TArray<uint8>& Key);

	static TArray<uint8> UInt32Value(uint32 Value);
	static TArray<uint8> UdpTransportValue();
	static TArray<uint8> ChannelValue(uint16 Channel);
	static TArray<uint8> AddressValue(const FFlockRelayPeer& Peer);

	/** A send indication carrying Count bytes to To, written into Into (room for SendIndicationOverhead + Count + 3) without allocating; its length. */
	static int32 WriteSendIndication(uint8* Into, const uint8* Transaction, const FFlockRelayPeer& To, const uint8* Data, int32 Count);

	/**
	 * A channel frame carrying Count bytes, written into Into without allocating, padded to 4 bytes when bPadded (on a TCP or TLS
	 * stream, as the standard asks); its length.
	 */
	static int32 WriteChannelFrame(uint8* Into, uint16 Channel, const uint8* Data, int32 Count, bool bPadded = false);

	/** A channel frame's number and how many of its bytes are data (they start after its four-byte header); false when the packet is not a whole channel frame. */
	static bool TryReadChannelFrame(const uint8* Packet, int32 Length, uint16& OutChannel, int32& OutDataLength);

	/** The type of a STUN message whose header and length hold; false for anything else. */
	static bool TryReadType(const uint8* Packet, int32 Length, uint16& OutType);

	static uint16 ClassOf(uint16 Type) { return Type & 0x110; }

	/** Who a data indication is from and where its data lies in the packet, read without allocating; false when it is not one. */
	static bool TryReadDataIndication(const uint8* Packet, int32 Length, FFlockRelayPeer& OutFrom, int32& OutDataOffset, int32& OutDataLength);

	/** An IPv4 XOR address attribute's value at At; false for any other family or size. */
	static bool TryReadAddress(const uint8* Packet, int32 At, int32 Size, FFlockRelayPeer& OutPeer);

	/** A whole STUN message, its attributes kept by kind (the first of each), with where its signature and fingerprint start; false when the packet is not one. */
	static bool Read(const uint8* Packet, int32 Length, FFlockRelayMessage& OutMessage);

	/** True when the message's signature was made with Key; false when it has none. */
	static bool SignatureHolds(const FFlockRelayMessage& Message, const TArray<uint8>& Key);

	/** The key a relay login signs with: MD5 of "username:realm:password", as the standard's long-term logins have it. */
	static TArray<uint8> LoginKey(const FString& Username, const FString& Realm, const FString& Password);

	/** An error answer's code (such as 401 or 438) and its words; 0 and empty when the value is missing or too short. */
	static int32 ErrorCodeOf(const TArray<uint8>* Value, FString& OutWords);

	/** The standard CRC-32 of Count bytes, which a fingerprint carries (masked). */
	static uint32 FingerprintChecksum(const uint8* Data, int32 Count);

	static uint16 ReadUInt16(const uint8* Data, int32 At) { return static_cast<uint16>((Data[At] << 8) | Data[At + 1]); }
	static uint32 ReadUInt32(const uint8* Data, int32 At)
	{
		return (static_cast<uint32>(Data[At]) << 24) | (static_cast<uint32>(Data[At + 1]) << 16) | (static_cast<uint32>(Data[At + 2]) << 8) | Data[At + 3];
	}
	static void WriteUInt16(uint8* Data, int32 At, uint16 Value)
	{
		Data[At] = static_cast<uint8>(Value >> 8);
		Data[At + 1] = static_cast<uint8>(Value);
	}
	static void WriteUInt32(uint8* Data, int32 At, uint32 Value)
	{
		Data[At] = static_cast<uint8>(Value >> 24);
		Data[At + 1] = static_cast<uint8>(Value >> 16);
		Data[At + 2] = static_cast<uint8>(Value >> 8);
		Data[At + 3] = static_cast<uint8>(Value);
	}

	/** Text as the wire carries it: UTF-8 bytes, no terminator. */
	static TArray<uint8> Utf8(const FString& Text);
	static FString FromUtf8(const uint8* Bytes, int32 Count);
};
