// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Multiplayer/Relay/FlockRelayLogin.h"
#include "Multiplayer/Relay/FlockRelayPeer.h"
#include "Multiplayer/Relay/FlockRelayWire.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/Support/FlockTestSpelling.h"

// The relay's messages: a signed request matching one Python's own hmac, md5 and crc wrote (Libraries/multiplayer-relay/
// make_wire_vector.py, the form coturn_p2p accepted), answers checked before they are believed, addresses, channel frames, and
// the relay servers read from Flock's answer.

#define FLOCK_RELAY_WIRE_TEST(TestClass, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(TestClass, "Flock.Multiplayer.Relay.Wire." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

namespace FlockRelayWireTestValues
{
	const TCHAR* const Username = TEXT("1791570268:game:player");
	const TCHAR* const Realm = TEXT("qwacks");
	const TCHAR* const Password = TEXT("secret-password");
	const TCHAR* const Nonce = TEXT("abcdef0123456789");
	const TCHAR* const PythonKey = TEXT("890f3679a1a303612a7bd7ace7816e4b");
	const TCHAR* const PythonMessage =
		TEXT("000300642112a4420102030405060708090a0b0c001900041100000000060016313739313537303236383a67616d653a706c6179657200000014")
		TEXT("0006717761636b730000001500106162636465663031323334353637383900080014424fe9fdfa1f96a2af4d5447bedfb282db32f10480280004a19d7a0b");

	FString Hex(const TArray<uint8>& Bytes)
	{
		return BytesToHex(Bytes.GetData(), Bytes.Num()).ToLower();
	}

	TArray<uint8> Bytes(const FString& Text)
	{
		TArray<uint8> Out;
		Out.SetNumUninitialized(Text.Len() / 2);
		HexToBytes(Text, Out.GetData());
		return Out;
	}

	TArray<uint8> Transaction()
	{
		TArray<uint8> Out;
		for (uint8 Value = 1; Value <= 12; ++Value)
		{
			Out.Add(Value);
		}
		return Out;
	}

	TArray<uint8> SignedAllocate(const TArray<uint8>& Key)
	{
		TArray<FFlockRelayAttribute> Attributes;
		Attributes.Emplace(FFlockRelayWire::RequestedTransportAttribute, FFlockRelayWire::UdpTransportValue());
		Attributes.Emplace(FFlockRelayWire::UsernameAttribute, FFlockRelayWire::Utf8(Username));
		Attributes.Emplace(FFlockRelayWire::RealmAttribute, FFlockRelayWire::Utf8(Realm));
		Attributes.Emplace(FFlockRelayWire::NonceAttribute, FFlockRelayWire::Utf8(Nonce));
		return FFlockRelayWire::Write(FFlockRelayWire::AllocateMethod | FFlockRelayWire::RequestClass, Transaction(), Attributes, Key);
	}

	FFlockRelayPeer Peer(const TCHAR* Text)
	{
		FFlockRelayPeer Out;
		FFlockRelayPeer::TryParse(Text, Out);
		return Out;
	}
}

using namespace FlockRelayWireTestValues;

FLOCK_RELAY_WIRE_TEST(FFlockRelayWireLoginKeyTest, "LoginKeyMatchesAnotherImplementation")
bool FFlockRelayWireLoginKeyTest::RunTest(const FString& Parameters)
{
	TestEqualSensitive(TEXT("MD5 of username:realm:password"), Hex(FFlockRelayWire::LoginKey(Username, Realm, Password)), FString(PythonKey));
	return true;
}

FLOCK_RELAY_WIRE_TEST(FFlockRelayWireSignedRequestTest, "WriteSignedRequestMatchesAnotherImplementationByteForByte")
bool FFlockRelayWireSignedRequestTest::RunTest(const FString& Parameters)
{
	TestEqualSensitive(TEXT("the signed, fingerprinted reservation"), Hex(SignedAllocate(FFlockRelayWire::LoginKey(Username, Realm, Password))),
		FString(PythonMessage));
	TestEqual(TEXT("the fingerprint's CRC is the standard one"), FFlockRelayWire::FingerprintChecksum(reinterpret_cast<const uint8*>("123456789"), 9), 0xCBF43926u);
	return true;
}

FLOCK_RELAY_WIRE_TEST(FFlockRelayWireSignatureTest, "ReadSignatureHoldsOnlyForTheBytesAndKeyItWasMadeWith")
bool FFlockRelayWireSignatureTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Key = Bytes(PythonKey);
	const TArray<uint8> Packet = Bytes(PythonMessage);
	FFlockRelayMessage Message;
	if (!TestTrue(TEXT("read"), FFlockRelayWire::Read(Packet.GetData(), Packet.Num(), Message)))
	{
		return false;
	}
	TestTrue(TEXT("signed with the login's key"), FFlockRelayWire::SignatureHolds(Message, Key));
	FString Name;
	TestTrue(TEXT("the username is there"), Message.TryGetText(FFlockRelayWire::UsernameAttribute, Name));
	TestEqualSensitive(TEXT("the username"), Name, FString(Username));
	TestFalse(TEXT("another key"), FFlockRelayWire::SignatureHolds(Message, FFlockRelayWire::LoginKey(Username, Realm, TEXT("another-password"))));
	TArray<uint8> Changed = Packet;
	Changed[40] ^= 0x01;
	FFlockRelayMessage Tampered;
	FFlockRelayWire::Read(Changed.GetData(), Changed.Num(), Tampered);
	TestFalse(TEXT("a byte changed under the signature"), FFlockRelayWire::SignatureHolds(Tampered, Key));
	return true;
}

FLOCK_RELAY_WIRE_TEST(FFlockRelayWireUnsignedTest, "ReadUnsignedMessageHasNoSignatureToHold")
bool FFlockRelayWireUnsignedTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Packet = SignedAllocate(TArray<uint8>());
	FFlockRelayMessage Message;
	FFlockRelayWire::Read(Packet.GetData(), Packet.Num(), Message);
	TestEqual(TEXT("no signature"), Message.IntegrityAt, static_cast<int32>(INDEX_NONE));
	TestTrue(TEXT("control: the message was read to its fingerprint"), Message.FingerprintAt > 0);
	TestFalse(TEXT("nothing to hold"), FFlockRelayWire::SignatureHolds(Message, Bytes(PythonKey)));
	return true;
}

FLOCK_RELAY_WIRE_TEST(FFlockRelayWireAfterSignatureTest, "ReadAttributesAfterTheSignatureAreNotTaken")
bool FFlockRelayWireAfterSignatureTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Packet = Bytes(PythonMessage);
	FFlockRelayMessage Message;
	FFlockRelayWire::Read(Packet.GetData(), Packet.Num(), Message);
	TestFalse(TEXT("only the fingerprint follows a signature, and it is read apart"), Message.Attributes.Contains(FFlockRelayWire::FingerprintAttribute));
	TestTrue(TEXT("the fingerprint follows the signature"), Message.FingerprintAt > Message.IntegrityAt);

	// An attribute slipped in after the signature, which the signature does not cover, is never taken.
	TArray<uint8> Slipped = Packet;
	Slipped.SetNum(Packet.Num() - 8);
	const uint8 Software[] = { 0x80, 0x22, 0x00, 0x01, 'x', 0, 0, 0 };
	Slipped.Append(Software, 8);
	FFlockRelayWire::WriteUInt16(Slipped.GetData(), 2, static_cast<uint16>(Slipped.Num() - FFlockRelayWire::HeaderLength));
	FFlockRelayMessage Read;
	TestTrue(TEXT("read"), FFlockRelayWire::Read(Slipped.GetData(), Slipped.Num(), Read));
	TestFalse(TEXT("what follows the signature is not taken"), Read.Attributes.Contains(0x8022));
	TestTrue(TEXT("control: what precedes it is"), Read.Attributes.Contains(FFlockRelayWire::NonceAttribute));
	return true;
}

FLOCK_RELAY_WIRE_TEST(FFlockRelayWireNotStunTest, "ReadNotAStunMessageIsNothing")
bool FFlockRelayWireNotStunTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Packet = Bytes(PythonMessage);
	FFlockRelayMessage Message;
	TestFalse(TEXT("shorter than a header"), FFlockRelayWire::Read(Packet.GetData(), 19, Message));
	TArray<uint8> NoCookie = Packet;
	NoCookie[4] = 0;
	TestFalse(TEXT("no magic cookie"), FFlockRelayWire::Read(NoCookie.GetData(), NoCookie.Num(), Message));
	TestFalse(TEXT("a length longer than the packet"), FFlockRelayWire::Read(Packet.GetData(), Packet.Num() - 4, Message));
	return true;
}

FLOCK_RELAY_WIRE_TEST(FFlockRelayWireAddressTest, "AddressIsWrittenAndReadBackForIpv4Only")
bool FFlockRelayWireAddressTest::RunTest(const FString& Parameters)
{
	const FFlockRelayPeer Written = Peer(TEXT("203.0.113.50:50000"));
	TArray<uint8> Value = FFlockRelayWire::AddressValue(Written);
	FFlockRelayPeer Read;
	TestTrue(TEXT("read back"), FFlockRelayWire::TryReadAddress(Value.GetData(), 0, Value.Num(), Read));
	TestTrue(TEXT("the same address"), Read == Written);
	TestNotEqual(TEXT("the address is masked with the magic cookie, not written plain"), static_cast<int32>(Value[4]), 0xCB);
	Value[1] = 0x02;
	TestFalse(TEXT("an IPv6 address"), FFlockRelayWire::TryReadAddress(Value.GetData(), 0, Value.Num(), Read));
	uint8 Wide[20] = {};
	TestFalse(TEXT("an IPv6-sized value"), FFlockRelayWire::TryReadAddress(Wide, 0, 20, Read));
	return true;
}

FLOCK_RELAY_WIRE_TEST(FFlockRelayWireDataIndicationTest, "DataIndicationIsReadInPlace")
bool FFlockRelayWireDataIndicationTest::RunTest(const FString& Parameters)
{
	const FFlockRelayPeer From = Peer(TEXT("203.0.113.50:50001"));
	const TArray<uint8> Data = { 9, 8, 7, 6, 5 };
	TArray<FFlockRelayAttribute> Attributes;
	Attributes.Emplace(FFlockRelayWire::XorPeerAddressAttribute, FFlockRelayWire::AddressValue(From));
	Attributes.Emplace(FFlockRelayWire::DataAttribute, Data);
	const TArray<uint8> Packet = FFlockRelayWire::Write(FFlockRelayWire::DataMethod | FFlockRelayWire::IndicationClass, Transaction(), Attributes, TArray<uint8>());
	FFlockRelayPeer Heard;
	int32 Offset = 0;
	int32 Count = 0;
	TestTrue(TEXT("read"), FFlockRelayWire::TryReadDataIndication(Packet.GetData(), Packet.Num(), Heard, Offset, Count));
	TestTrue(TEXT("from the peer"), Heard == From);
	TestTrue(TEXT("its data, where it lies"), Count == Data.Num() && FMemory::Memcmp(Packet.GetData() + Offset, Data.GetData(), Count) == 0);

	TArray<FFlockRelayAttribute> NoPeer;
	NoPeer.Emplace(FFlockRelayWire::DataAttribute, Data);
	const TArray<uint8> Anonymous = FFlockRelayWire::Write(FFlockRelayWire::DataMethod | FFlockRelayWire::IndicationClass, Transaction(), NoPeer, TArray<uint8>());
	TestFalse(TEXT("no peer address"), FFlockRelayWire::TryReadDataIndication(Anonymous.GetData(), Anonymous.Num(), Heard, Offset, Count));
	const TArray<uint8> Request = Bytes(PythonMessage);
	TestFalse(TEXT("another kind of message"), FFlockRelayWire::TryReadDataIndication(Request.GetData(), Request.Num(), Heard, Offset, Count));
	return true;
}

FLOCK_RELAY_WIRE_TEST(FFlockRelayWireSendIndicationTest, "SendIndicationIsWhatTheRelayReadsPaddedToFourBytes")
bool FFlockRelayWireSendIndicationTest::RunTest(const FString& Parameters)
{
	const FFlockRelayPeer To = Peer(TEXT("203.0.113.50:50002"));
	const uint8 Data[] = { 1, 2, 3, 4, 5 };
	uint8 Buffer[64] = {};
	const int32 Length = FFlockRelayWire::WriteSendIndication(Buffer, Transaction().GetData(), To, Data, 5);
	TestEqual(TEXT("padded to four bytes"), Length % 4, 0);
	FFlockRelayMessage Message;
	if (!TestTrue(TEXT("read"), FFlockRelayWire::Read(Buffer, Length, Message)))
	{
		return false;
	}
	TestEqual(TEXT("a send indication"), static_cast<int32>(Message.Type), static_cast<int32>(FFlockRelayWire::SendMethod | FFlockRelayWire::IndicationClass));
	FFlockRelayPeer Named;
	TestTrue(TEXT("names the peer"), Message.TryGetAddress(FFlockRelayWire::XorPeerAddressAttribute, Named) && Named == To);
	const TArray<uint8>* Carried = Message.Attributes.Find(FFlockRelayWire::DataAttribute);
	TestTrue(TEXT("carries the data"), Carried != nullptr && *Carried == TArray<uint8>(Data, 5));
	return true;
}

FLOCK_RELAY_WIRE_TEST(FFlockRelayWireChannelFrameTest, "ChannelFrameIsWrittenAndReadBackAndAShortOneRefused")
bool FFlockRelayWireChannelFrameTest::RunTest(const FString& Parameters)
{
	const uint8 Data[] = { 1, 2, 3 };
	uint8 Buffer[16] = {};
	const int32 Length = FFlockRelayWire::WriteChannelFrame(Buffer, 0x4001, Data, 3);
	TestEqual(TEXT("four bytes before the data, nothing after over UDP"), Length, 7);
	uint16 Channel = 0;
	int32 Count = 0;
	TestTrue(TEXT("read back"), FFlockRelayWire::TryReadChannelFrame(Buffer, Length, Channel, Count));
	TestEqual(TEXT("its channel"), static_cast<int32>(Channel), 0x4001);
	TestEqual(TEXT("its length"), Count, 3);
	TestFalse(TEXT("a frame claiming more than it holds"), FFlockRelayWire::TryReadChannelFrame(Buffer, Length - 1, Channel, Count));
	const TArray<uint8> Request = Bytes(PythonMessage);
	TestFalse(TEXT("a STUN message is no channel frame"), FFlockRelayWire::TryReadChannelFrame(Request.GetData(), Request.Num(), Channel, Count));
	return true;
}

FLOCK_RELAY_WIRE_TEST(FFlockRelayWireErrorCodeTest, "ErrorCodeReadsClassAndNumber")
bool FFlockRelayWireErrorCodeTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Unauthorized = { 0, 0, 4, 1 };
	Unauthorized.Append(FFlockRelayWire::Utf8(TEXT("Unauthorized")));
	FString Words;
	TestEqual(TEXT("401"), FFlockRelayWire::ErrorCodeOf(&Unauthorized, Words), 401);
	TestEqualSensitive(TEXT("its words"), Words, FString(TEXT("Unauthorized")));
	const TArray<uint8> Stale = { 0, 0, 4, 38 };
	TestEqual(TEXT("438"), FFlockRelayWire::ErrorCodeOf(&Stale, Words), 438);
	const TArray<uint8> Full = { 0, 0, 5, 8 };
	TestEqual(TEXT("508"), FFlockRelayWire::ErrorCodeOf(&Full, Words), 508);
	const TArray<uint8> Short = { 0, 4 };
	TestEqual(TEXT("too short"), FFlockRelayWire::ErrorCodeOf(&Short, Words), 0);
	TestEqual(TEXT("none"), FFlockRelayWire::ErrorCodeOf(nullptr, Words), 0);
	return true;
}

FLOCK_RELAY_WIRE_TEST(FFlockRelayWireClassTest, "ClassIsReadFromTheType")
bool FFlockRelayWireClassTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("error"), static_cast<int32>(FFlockRelayWire::ClassOf(0x0113)), static_cast<int32>(FFlockRelayWire::ErrorClass));
	TestEqual(TEXT("success"), static_cast<int32>(FFlockRelayWire::ClassOf(0x0109)), static_cast<int32>(FFlockRelayWire::SuccessClass));
	TestEqual(TEXT("indication"), static_cast<int32>(FFlockRelayWire::ClassOf(0x0017)), static_cast<int32>(FFlockRelayWire::IndicationClass));
	TestEqual(TEXT("request"), static_cast<int32>(FFlockRelayWire::ClassOf(0x0003)), static_cast<int32>(FFlockRelayWire::RequestClass));
	return true;
}

FLOCK_RELAY_WIRE_TEST(FFlockRelayWireUdpServersTest, "RelayServerUdpAddressesAreRead")
bool FFlockRelayWireUdpServersTest::RunTest(const FString& Parameters)
{
	struct FCase { const TCHAR* Url; const TCHAR* Host; int32 Port; };
	const FCase Cases[] = {
		{ TEXT("turn:relay.test:3479?transport=udp"), TEXT("relay.test"), 3479 },
		{ TEXT("turn:relay.test:3479"), TEXT("relay.test"), 3479 },
		{ TEXT("turn:relay.test"), TEXT("relay.test"), 3478 },
		{ TEXT("TURN:Relay.Test:5000?Transport=UDP"), TEXT("Relay.Test"), 5000 },
		{ TEXT("turn:203.0.113.9:3479?transport=udp"), TEXT("203.0.113.9"), 3479 },
	};
	for (const FCase& Case : Cases)
	{
		FString Host;
		int32 Port = 0;
		TestTrue(FString::Printf(TEXT("%s is read"), Case.Url), FFlockRelayLogin::TryReadUdpServer(Case.Url, Host, Port));
		TestEqualSensitive(FString::Printf(TEXT("%s: its host as written"), Case.Url), Host, FString(Case.Host));
		TestEqual(FString::Printf(TEXT("%s: its port"), Case.Url), Port, Case.Port);
	}
	return true;
}

FLOCK_RELAY_WIRE_TEST(FFlockRelayWireNotUdpServersTest, "RelayServerAnythingElseIsLeftOut")
bool FFlockRelayWireNotUdpServersTest::RunTest(const FString& Parameters)
{
	const TCHAR* Cases[] = { TEXT("turn:relay.test:3479?transport=tcp"), TEXT("turns:relay.test:5349?transport=tcp"), TEXT("turn:[::1]:3478"),
		TEXT("turn::3478"), TEXT("turn:relay.test:70000"), TEXT("turn:relay.test:port"), TEXT("stun:stun.l.google.com:19302"), TEXT("") };
	for (const TCHAR* Url : Cases)
	{
		FString Host;
		int32 Port = 0;
		TestFalse(FString::Printf(TEXT("\"%s\" is left out"), Url), FFlockRelayLogin::TryReadUdpServer(Url, Host, Port));
	}
	return true;
}

FLOCK_RELAY_WIRE_TEST(FFlockRelayWireLoginsTest, "LoginsAreFlocksUdpRelaysInFlocksOrder")
bool FFlockRelayWireLoginsTest::RunTest(const FString& Parameters)
{
	FFlockRelayLoginList Answer;
	Answer.Entries.Add({ { TEXT("turn:main.test:3479?transport=udp"), TEXT("turn:main.test:3479?transport=tcp") }, TEXT("u1"), TEXT("p1") });
	Answer.Entries.Add({ { TEXT("turn:no-login.test:3479") }, FString(), FString() });
	Answer.Entries.Add({ { TEXT("turn:next.test:3480") }, TEXT("u2"), TEXT("p2") });
	Answer.Entries.Add({ { TEXT("stun:stun.test:19302") }, FString(), FString() });
	const TArray<FFlockRelayLogin> Logins = FFlockRelayLogin::InAnswer(Answer);
	if (!TestEqual(TEXT("two UDP relays with a login"), Logins.Num(), 2))
	{
		return false;
	}
	TestEqualSensitive(TEXT("Flock's main relay first"), Logins[0].ToString(), FString(TEXT("main.test:3479")));
	TestEqualSensitive(TEXT("then the next"), Logins[1].ToString(), FString(TEXT("next.test:3480")));
	TestEqualSensitive(TEXT("the first's login"), Logins[0].Username, FString(TEXT("u1")));
	TestEqualSensitive(TEXT("the second's password"), Logins[1].Password, FString(TEXT("p2")));
	TestEqual(TEXT("no servers listed"), FFlockRelayLogin::InAnswer(FFlockRelayLoginList()).Num(), 0);
	const TArray<FFlockRelayStunServer> Stun = FFlockRelayStunServer::InAnswer(Answer);
	TestTrue(TEXT("the STUN server Flock listed"), Stun.Num() == 1 && Stun[0].Host.Equals(TEXT("stun.test"), ESearchCase::CaseSensitive) && Stun[0].Port == 19302);
	return true;
}

FLOCK_RELAY_WIRE_TEST(FFlockRelayWireLoginsAnswerTest, "TheRouteAnswerIsReadAsMeasured")
bool FFlockRelayWireLoginsAnswerTest::RunTest(const FString& Parameters)
{
	// The local backend's answer for a seat, measured 2026-10-17 (results/relay-rules.txt), with the login hidden.
	const FString Measured = TEXT("{\"ttl\": 600, \"ice_servers\": [{\"urls\": [\"turn:localhost:3479?transport=udp\", \"turn:localhost:3479?transport=tcp\"], ")
		TEXT("\"username\": \"1797000000:game:player\", \"credential\": \"hidden\"}, {\"urls\": [\"stun:stun.l.google.com:19302\"], \"username\": null, ")
		TEXT("\"credential\": null}], \"relay_paused\": false}");
	TSharedPtr<FJsonObject> Object;
	if (!TestTrue(TEXT("precondition: JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Measured), Object) && Object.IsValid()))
	{
		return false;
	}
	FFlockRelayLoginList Read;
	FString Error;
	TestTrue(TEXT("read"), FFlockRelayLoginList::FromWireObject(Object.ToSharedRef(), Read, Error));
	TestEqual(TEXT("its ttl"), Read.TtlSeconds, 600);
	TestFalse(TEXT("not paused"), Read.bRelayPaused);
	const TArray<FFlockRelayLogin> Logins = FFlockRelayLogin::InAnswer(Read);
	TestTrue(TEXT("one UDP relay, its TCP form left out"), Logins.Num() == 1 && Logins[0].ToString().Equals(TEXT("localhost:3479"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("the STUN entry's null login is no login"), Read.Entries.Num() == 2 && Read.Entries[1].Username.IsEmpty());

	TSharedPtr<FJsonObject> NoList;
	FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(TEXT("{\"ttl\": 600}")), NoList);
	TestFalse(TEXT("an answer listing no servers is not the route's answer"), FFlockRelayLoginList::FromWireObject(NoList.ToSharedRef(), Read, Error));
	TSharedPtr<FJsonObject> OtherSpelling;
	FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(TEXT("{\"ICE_SERVERS\": []}")), OtherSpelling);
	TestFalse(TEXT("keys are read as the backend spells them"), FFlockRelayLoginList::FromWireObject(OtherSpelling.ToSharedRef(), Read, Error));
	return true;
}

FLOCK_RELAY_WIRE_TEST(FFlockRelayWirePeerTest, "PeerIsReadFromTextAndWrittenBack")
bool FFlockRelayWirePeerTest::RunTest(const FString& Parameters)
{
	FFlockRelayPeer Read;
	TestTrue(TEXT("read"), FFlockRelayPeer::TryParse(TEXT("203.0.113.50:50000"), Read));
	TestEqualSensitive(TEXT("written back"), Read.ToString(), FString(TEXT("203.0.113.50:50000")));
	TestEqual(TEXT("its address"), Read.Address, 0xCB007132u);
	TestEqual(TEXT("its port"), Read.Port, 50000);
	return true;
}

FLOCK_RELAY_WIRE_TEST(FFlockRelayWireNotPeerTest, "PeerAnythingButDigitsIsRefused")
bool FFlockRelayWireNotPeerTest::RunTest(const FString& Parameters)
{
	const TCHAR* Cases[] = { TEXT("203.0.113.50"), TEXT("203.0.113:5"), TEXT("256.0.113.50:5"), TEXT("203.0.113.50:0"), TEXT("203.0.113.50:65536"),
		TEXT("+203.0.113.50:5"), TEXT("+1.2.3.4:5"), TEXT("1.2.3. 4:5"), TEXT("203.0.113.50:+5"), TEXT(" 203.0.113.50:5"), TEXT("203.0.113.50: 5"),
		TEXT("203.0. 113.50:5"), TEXT("[::1]:5"), TEXT("") };
	for (const TCHAR* Text : Cases)
	{
		FFlockRelayPeer Read;
		TestFalse(FString::Printf(TEXT("\"%s\" is refused"), Text), FFlockRelayPeer::TryParse(Text, Read));
	}
	return true;
}

#endif
