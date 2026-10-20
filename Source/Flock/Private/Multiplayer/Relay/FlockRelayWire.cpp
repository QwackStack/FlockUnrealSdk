// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/Relay/FlockRelayWire.h"
#include "Misc/Guid.h"
#include "Misc/SecureHash.h"

namespace
{
	constexpr uint8 UdpTransport = 17;
	constexpr uint8 Ipv4Family = 0x01;
	constexpr int32 IntegrityLength = 20;
	constexpr uint32 FingerprintMask = 0x5354554E;

	struct FCrcTable
	{
		uint32 Values[256];

		FCrcTable()
		{
			for (uint32 Index = 0; Index < 256; ++Index)
			{
				uint32 Value = Index;
				for (int32 Bit = 0; Bit < 8; ++Bit)
				{
					Value = (Value & 1) != 0 ? 0xEDB88320 ^ (Value >> 1) : Value >> 1;
				}
				Values[Index] = Value;
			}
		}
	};

	const FCrcTable& CrcTable()
	{
		static const FCrcTable Table;
		return Table;
	}

	void AddUInt16(TArray<uint8>& Out, uint16 Value)
	{
		Out.Add(static_cast<uint8>(Value >> 8));
		Out.Add(static_cast<uint8>(Value));
	}

	void AddAttribute(TArray<uint8>& Body, uint16 Kind, const uint8* Value, int32 Count)
	{
		AddUInt16(Body, Kind);
		AddUInt16(Body, static_cast<uint16>(Count));
		Body.Append(Value, Count);
		for (int32 Pad = 0; Pad < (4 - Count % 4) % 4; ++Pad)
		{
			Body.Add(0);
		}
	}

	/** A header and a body as one message; the header's length is BodyLength, which may count what is yet to be added. */
	TArray<uint8> Joined(uint16 Type, int32 BodyLength, const TArray<uint8>& Transaction, const TArray<uint8>& Body)
	{
		TArray<uint8> Whole;
		Whole.SetNumUninitialized(FFlockRelayWire::HeaderLength);
		FFlockRelayWire::WriteUInt16(Whole.GetData(), 0, Type);
		FFlockRelayWire::WriteUInt16(Whole.GetData(), 2, static_cast<uint16>(BodyLength));
		FFlockRelayWire::WriteUInt32(Whole.GetData(), 4, FFlockRelayWire::MagicCookie);
		FMemory::Memcpy(Whole.GetData() + 8, Transaction.GetData(), FFlockRelayWire::TransactionLength);
		Whole.Append(Body);
		return Whole;
	}

	void WriteAddress(uint8* Into, int32 At, const FFlockRelayPeer& Peer)
	{
		Into[At] = 0;
		Into[At + 1] = Ipv4Family;
		FFlockRelayWire::WriteUInt16(Into, At + 2, static_cast<uint16>(Peer.Port ^ static_cast<int32>(FFlockRelayWire::MagicCookie >> 16)));
		FFlockRelayWire::WriteUInt32(Into, At + 4, Peer.Address ^ FFlockRelayWire::MagicCookie);
	}
}

bool FFlockRelayMessage::TryGetAddress(uint16 Kind, FFlockRelayPeer& OutPeer) const
{
	const TArray<uint8>* Value = Attributes.Find(Kind);
	return Value != nullptr && FFlockRelayWire::TryReadAddress(Value->GetData(), 0, Value->Num(), OutPeer);
}

bool FFlockRelayMessage::TryGetText(uint16 Kind, FString& OutText) const
{
	const TArray<uint8>* Value = Attributes.Find(Kind);
	if (Value == nullptr)
	{
		return false;
	}
	OutText = FFlockRelayWire::FromUtf8(Value->GetData(), Value->Num());
	return true;
}

TArray<uint8> FFlockRelayWire::NewTransaction()
{
	// Each platform makes a GUID from its own random source (Windows, Unix and Apple read the system's); its first twelve
	// bytes carry four fixed bits at most.
	const FGuid Random = FGuid::NewGuid();
	TArray<uint8> Transaction;
	Transaction.SetNumUninitialized(TransactionLength);
	WriteUInt32(Transaction.GetData(), 0, Random.A);
	WriteUInt32(Transaction.GetData(), 4, Random.B ^ Random.D);
	WriteUInt32(Transaction.GetData(), 8, Random.C);
	return Transaction;
}

TArray<uint8> FFlockRelayWire::Write(uint16 Type, const TArray<uint8>& Transaction, const TArray<FFlockRelayAttribute>& Attributes, const TArray<uint8>& Key)
{
	TArray<uint8> Body;
	for (const FFlockRelayAttribute& Attribute : Attributes)
	{
		AddAttribute(Body, Attribute.Key, Attribute.Value.GetData(), Attribute.Value.Num());
	}
	if (Key.Num() > 0)
	{
		// The signature covers the header with a length that already counts the signature itself.
		const TArray<uint8> Signed = Joined(Type, Body.Num() + 4 + IntegrityLength, Transaction, Body);
		uint8 Signature[IntegrityLength];
		FSHA1::HMACBuffer(Key.GetData(), Key.Num(), Signed.GetData(), Signed.Num(), Signature);
		AddAttribute(Body, MessageIntegrityAttribute, Signature, IntegrityLength);
	}
	const TArray<uint8> BeforeFingerprint = Joined(Type, Body.Num() + 8, Transaction, Body);
	uint8 Fingerprint[4];
	WriteUInt32(Fingerprint, 0, FingerprintChecksum(BeforeFingerprint.GetData(), BeforeFingerprint.Num()) ^ FingerprintMask);
	AddAttribute(Body, FingerprintAttribute, Fingerprint, 4);
	return Joined(Type, Body.Num(), Transaction, Body);
}

TArray<uint8> FFlockRelayWire::UInt32Value(uint32 Value)
{
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(4);
	WriteUInt32(Bytes.GetData(), 0, Value);
	return Bytes;
}

TArray<uint8> FFlockRelayWire::UdpTransportValue()
{
	return TArray<uint8>({ UdpTransport, 0, 0, 0 });
}

TArray<uint8> FFlockRelayWire::ChannelValue(uint16 Channel)
{
	return TArray<uint8>({ static_cast<uint8>(Channel >> 8), static_cast<uint8>(Channel), 0, 0 });
}

TArray<uint8> FFlockRelayWire::AddressValue(const FFlockRelayPeer& Peer)
{
	TArray<uint8> Value;
	Value.SetNumUninitialized(8);
	WriteAddress(Value.GetData(), 0, Peer);
	return Value;
}

int32 FFlockRelayWire::WriteSendIndication(uint8* Into, const uint8* Transaction, const FFlockRelayPeer& To, const uint8* Data, int32 Count)
{
	const int32 Padding = (4 - Count % 4) % 4;
	WriteUInt16(Into, 0, SendMethod | IndicationClass);
	WriteUInt16(Into, 2, static_cast<uint16>(12 + 4 + Count + Padding));
	WriteUInt32(Into, 4, MagicCookie);
	FMemory::Memcpy(Into + 8, Transaction, TransactionLength);
	WriteUInt16(Into, 20, XorPeerAddressAttribute);
	WriteUInt16(Into, 22, 8);
	WriteAddress(Into, 24, To);
	WriteUInt16(Into, 32, DataAttribute);
	WriteUInt16(Into, 34, static_cast<uint16>(Count));
	FMemory::Memcpy(Into + SendIndicationOverhead, Data, Count);
	FMemory::Memzero(Into + SendIndicationOverhead + Count, Padding);
	return SendIndicationOverhead + Count + Padding;
}

int32 FFlockRelayWire::WriteChannelFrame(uint8* Into, uint16 Channel, const uint8* Data, int32 Count)
{
	WriteUInt16(Into, 0, Channel);
	WriteUInt16(Into, 2, static_cast<uint16>(Count));
	FMemory::Memcpy(Into + ChannelFrameHeaderLength, Data, Count);
	return ChannelFrameHeaderLength + Count;
}

bool FFlockRelayWire::TryReadChannelFrame(const uint8* Packet, int32 Length, uint16& OutChannel, int32& OutDataLength)
{
	OutChannel = 0;
	OutDataLength = 0;
	if (Length < ChannelFrameHeaderLength || (Packet[0] & 0xC0) != 0x40)
	{
		return false;
	}
	OutChannel = ReadUInt16(Packet, 0);
	OutDataLength = ReadUInt16(Packet, 2);
	return ChannelFrameHeaderLength + OutDataLength <= Length;
}

bool FFlockRelayWire::TryReadType(const uint8* Packet, int32 Length, uint16& OutType)
{
	OutType = 0;
	if (Length < HeaderLength || (Packet[0] & 0xC0) != 0 || ReadUInt32(Packet, 4) != MagicCookie)
	{
		return false;
	}
	const int32 MessageLength = ReadUInt16(Packet, 2);
	if (MessageLength % 4 != 0 || HeaderLength + MessageLength > Length)
	{
		return false;
	}
	OutType = ReadUInt16(Packet, 0);
	return true;
}

bool FFlockRelayWire::TryReadDataIndication(const uint8* Packet, int32 Length, FFlockRelayPeer& OutFrom, int32& OutDataOffset, int32& OutDataLength)
{
	OutFrom = FFlockRelayPeer();
	OutDataOffset = 0;
	OutDataLength = 0;
	uint16 Type = 0;
	if (!TryReadType(Packet, Length, Type) || Type != (DataMethod | IndicationClass))
	{
		return false;
	}
	bool bFoundPeer = false;
	bool bFoundData = false;
	const int32 End = HeaderLength + ReadUInt16(Packet, 2);
	int32 Position = HeaderLength;
	while (Position + 4 <= End)
	{
		const uint16 Kind = ReadUInt16(Packet, Position);
		const int32 Size = ReadUInt16(Packet, Position + 2);
		const int32 Value = Position + 4;
		if (Value + Size > End)
		{
			return false;
		}
		if (Kind == XorPeerAddressAttribute && !bFoundPeer)
		{
			bFoundPeer = TryReadAddress(Packet, Value, Size, OutFrom);
		}
		else if (Kind == DataAttribute && !bFoundData)
		{
			bFoundData = true;
			OutDataOffset = Value;
			OutDataLength = Size;
		}
		Position = Value + Size + (4 - Size % 4) % 4;
	}
	return bFoundPeer && bFoundData;
}

bool FFlockRelayWire::TryReadAddress(const uint8* Packet, int32 At, int32 Size, FFlockRelayPeer& OutPeer)
{
	OutPeer = FFlockRelayPeer();
	if (Size != 8 || Packet[At + 1] != Ipv4Family)
	{
		return false;
	}
	const int32 Port = ReadUInt16(Packet, At + 2) ^ static_cast<int32>(MagicCookie >> 16);
	OutPeer = FFlockRelayPeer(ReadUInt32(Packet, At + 4) ^ MagicCookie, Port);
	return true;
}

bool FFlockRelayWire::Read(const uint8* Packet, int32 Length, FFlockRelayMessage& OutMessage)
{
	OutMessage = FFlockRelayMessage();
	uint16 Type = 0;
	if (!TryReadType(Packet, Length, Type))
	{
		return false;
	}
	OutMessage.Type = Type;
	OutMessage.Transaction = TArray<uint8>(Packet + 8, TransactionLength);
	const int32 End = HeaderLength + ReadUInt16(Packet, 2);
	int32 Position = HeaderLength;
	while (Position + 4 <= End)
	{
		const uint16 Kind = ReadUInt16(Packet, Position);
		const int32 Size = ReadUInt16(Packet, Position + 2);
		const int32 Value = Position + 4;
		if (Value + Size > End)
		{
			return false;
		}
		if (Kind == FingerprintAttribute)
		{
			OutMessage.FingerprintAt = Position;
			break;
		}
		// Only the fingerprint may follow a signature.
		if (OutMessage.IntegrityAt == INDEX_NONE)
		{
			if (Kind == MessageIntegrityAttribute)
			{
				OutMessage.IntegrityAt = Position;
			}
			if (!OutMessage.Attributes.Contains(Kind))
			{
				OutMessage.Attributes.Add(Kind, TArray<uint8>(Packet + Value, Size));
			}
		}
		Position = Value + Size + (4 - Size % 4) % 4;
	}
	OutMessage.Bytes = TArray<uint8>(Packet, End);
	return true;
}

bool FFlockRelayWire::SignatureHolds(const FFlockRelayMessage& Message, const TArray<uint8>& Key)
{
	if (Message.IntegrityAt == INDEX_NONE || Key.Num() == 0 || Message.IntegrityAt + 4 + IntegrityLength > Message.Bytes.Num())
	{
		return false;
	}
	TArray<uint8> Signed(Message.Bytes.GetData(), Message.IntegrityAt);
	WriteUInt16(Signed.GetData(), 2, static_cast<uint16>(Message.IntegrityAt - HeaderLength + 4 + IntegrityLength));
	uint8 Expected[IntegrityLength];
	FSHA1::HMACBuffer(Key.GetData(), Key.Num(), Signed.GetData(), Signed.Num(), Expected);
	return FMemory::Memcmp(Message.Bytes.GetData() + Message.IntegrityAt + 4, Expected, IntegrityLength) == 0;
}

TArray<uint8> FFlockRelayWire::LoginKey(const FString& Username, const FString& Realm, const FString& Password)
{
	const TArray<uint8> Text = Utf8(Username + TEXT(":") + Realm + TEXT(":") + Password);
	FMD5 Md5;
	Md5.Update(Text.GetData(), Text.Num());
	TArray<uint8> Key;
	Key.SetNumUninitialized(16);
	Md5.Final(Key.GetData());
	return Key;
}

int32 FFlockRelayWire::ErrorCodeOf(const TArray<uint8>* Value, FString& OutWords)
{
	OutWords.Reset();
	if (Value == nullptr || Value->Num() < 4)
	{
		return 0;
	}
	OutWords = FromUtf8(Value->GetData() + 4, Value->Num() - 4);
	return ((*Value)[2] & 0x7) * 100 + (*Value)[3];
}

uint32 FFlockRelayWire::FingerprintChecksum(const uint8* Data, int32 Count)
{
	const FCrcTable& Table = CrcTable();
	uint32 Crc = 0xFFFFFFFF;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Crc = Table.Values[(Crc ^ Data[Index]) & 0xFF] ^ (Crc >> 8);
	}
	return Crc ^ 0xFFFFFFFF;
}

TArray<uint8> FFlockRelayWire::Utf8(const FString& Text)
{
	const FTCHARToUTF8 Converted(*Text);
	return TArray<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
}

FString FFlockRelayWire::FromUtf8(const uint8* Bytes, int32 Count)
{
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes), Count);
	return FString(Converted.Length(), Converted.Get());
}
