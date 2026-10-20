// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "IPAddress.h"
#include "Misc/Guid.h"
#include "Misc/SecureHash.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include <atomic>

/**
 * A TURN relay on this machine's loopback, on a thread of its own, with coturn_p2p's answers as measured (2026-10-09, again from
 * this engine's sockets on 2026-10-17): an unsigned reservation is challenged (401 with realm and nonce), a wrong password is
 * refused again (401), a second reservation from one socket is 437, a repeat of a request under its transaction is answered
 * again, an opening to a refused address is 403, a channel taken twice or out of range is 400, a release (lifetime 0) ends a
 * reservation and a renewal after it is 437, and data is relayed between its own reservations (channel frames to a peer that
 * bound a channel back, data indications otherwise). Every success to a signed request is signed and every answer
 * fingerprinted. A test chooses silences, refusals, stale nonces and lifetimes, and can make it answer as Cloudflare's relay
 * did on 2026-10-12: an IP of its own for each reservation, and only some client ports answered. Its own codec, so it checks
 * the SDK's rather than sharing it. Include only from guarded test files.
 */
class FFlockFakeRelayServer : public FRunnable
{
public:
	static constexpr const TCHAR* Realm = TEXT("test-realm");
	static constexpr const TCHAR* RelayIp = TEXT("203.0.113.50");
	static constexpr uint16 MethodBinding = 0x001;
	static constexpr uint16 MethodAllocate = 0x003;
	static constexpr uint16 MethodRefresh = 0x004;
	static constexpr uint16 MethodSend = 0x006;
	static constexpr uint16 MethodCreatePermission = 0x008;
	static constexpr uint16 MethodChannelBind = 0x009;
	/** What Seen.Method holds for a channel frame. */
	static constexpr uint16 ChannelFrame = 0xFFFF;

	struct FSeen
	{
		uint16 Method = 0;
		uint16 Class = 0;
		bool bSigned = false;
		FString Transaction;
		TOptional<uint32> LifetimeAsked;
		int32 ClientPort = 0;
	};

	/** The password every login is checked against. */
	FString Password = TEXT("minted-for-the-test");
	std::atomic<bool> bSilent{ false };
	double ReservationLifetimeSeconds = 600.0;
	double OpeningLifetimeSeconds = 300.0;
	double ChannelLifetimeSeconds = 600.0;
	/** An error code to answer every signed reservation with (486 full, 442 and so on), or 0. */
	std::atomic<int32> RefuseReservationsWith{ 0 };
	/** Openings to these IPs are refused with 403. */
	TSet<FString> RefusedOpenings;
	/** Openings are refused with an error that names no code. */
	std::atomic<bool> bRefuseOpeningsNamingNoCode{ false };
	/** Every answer leaves this much later, as a farther relay's would. */
	std::atomic<double> AnswerDelaySeconds{ 0.0 };
	/** The next signed reservation is answered first with a success it did not sign, naming another address. */
	std::atomic<bool> bForgeNextReservation{ false };
	/** Answers to signed reservations are held while this is set, and sent when ReleaseHeldAnswers clears it. */
	std::atomic<bool> bHoldReservationAnswers{ false };
	/** The IPs reservations are given in turn: one, as coturn does, or several, as Cloudflare's relay gives addresses. */
	TArray<FString> RelayIps = { RelayIp };
	/**
	 * Answers only the client ports first heard at these places (1 for the first port heard, and so on); every other port is
	 * ignored whole, as Cloudflare's relay answered some ports every time and others never. Unset answers every port.
	 */
	TOptional<TSet<int32>> AnswersOnlyPortsHeard;
	/** Binding requests answered with the address they came from; off for a relay that ignores them. */
	std::atomic<bool> bAnswersBindings{ true };

	FFlockFakeRelayServer()
	{
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		Socket = Sockets->CreateSocket(NAME_DGram, TEXT("Fake relay server"), FNetworkProtocolTypes::IPv4);
		if (Socket == nullptr)
		{
			return;
		}
		const TSharedRef<FInternetAddr> Loopback = Sockets->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
		bool bValid = false;
		Loopback->SetIp(TEXT("127.0.0.1"), bValid);
		Loopback->SetPort(0);
		if (!Socket->Bind(*Loopback) || !Socket->SetNonBlocking(true))
		{
			Sockets->DestroySocket(Socket);
			Socket = nullptr;
			return;
		}
		Port = Socket->GetPortNo();
		Thread = FRunnableThread::Create(this, TEXT("Fake relay server"));
	}

	virtual ~FFlockFakeRelayServer() override
	{
		bStopped = true;
		if (Thread != nullptr)
		{
			Thread->WaitForCompletion();
			delete Thread;
		}
		if (Socket != nullptr)
		{
			Socket->Close();
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		}
	}

	bool IsReady() const { return Socket != nullptr && Port > 0; }
	int32 GetPort() const { return Port; }
	FString Url() const { return FString::Printf(TEXT("turn:127.0.0.1:%d?transport=udp"), Port); }

	void DropNext(uint16 Method, int32 Times)
	{
		FScopeLock Locked(&Lock);
		DropsLeft.Add(Method, Times);
	}

	/** Nothing more from this client port reaches the relay, as a router that moved a device to another port leaves it; packets for it still go out. */
	void StopHearing(int32 ClientPort)
	{
		FScopeLock Locked(&Lock);
		PortsNoLongerHeard.Add(ClientPort);
	}

	/** The next signed requests are answered 438 with a new nonce; -1 answers every one so. */
	void AnswerStaleNonce(int32 Times)
	{
		FScopeLock Locked(&Lock);
		StaleNonceAnswersLeft = Times;
	}

	TArray<FSeen> SeenRequests(uint16 Method) const
	{
		FScopeLock Locked(&Lock);
		return Seen.FilterByPredicate([Method](const FSeen& Each) { return Each.Method == Method && Each.Class == 0x000; });
	}

	/** Everything heard, in order: requests, indications and channel frames. */
	TArray<FSeen> AllSeen() const
	{
		FScopeLock Locked(&Lock);
		return Seen;
	}

	int32 Count(uint16 Method, uint16 Class) const
	{
		FScopeLock Locked(&Lock);
		return Seen.FilterByPredicate([Method, Class](const FSeen& Each) { return Each.Method == Method && Each.Class == Class; }).Num();
	}

	/** Reservations not yet given back or lapsed. */
	int32 Reservations() const
	{
		FScopeLock Locked(&Lock);
		const double Now = FPlatformTime::Seconds();
		return Kept.FilterByPredicate([Now](const FReservation& Each) { return Each.ExpiresAt > Now; }).Num();
	}

	/** Signed renewals asking for 0 s: releases. */
	int32 Releases() const
	{
		FScopeLock Locked(&Lock);
		return Seen.FilterByPredicate([](const FSeen& Each)
		{
			return Each.Method == MethodRefresh && Each.Class == 0x000 && Each.bSigned && Each.LifetimeAsked.IsSet() && Each.LifetimeAsked.GetValue() == 0;
		}).Num();
	}

	/** Forgets every reservation, as a relay that restarted would: the next renewal is 437. */
	void ForgetEveryReservation()
	{
		FScopeLock Locked(&Lock);
		Kept.Reset();
	}

	void ReleaseHeldAnswers()
	{
		bHoldReservationAnswers = false;
		TArray<TPair<TArray<uint8>, int32>> Held;
		{
			FScopeLock Locked(&Lock);
			Held = MoveTemp(HeldAnswers);
			HeldAnswers.Reset();
		}
		for (const TPair<TArray<uint8>, int32>& Answer : Held)
		{
			SendNow(Answer.Key, Answer.Value);
		}
	}

	virtual uint32 Run() override
	{
		uint8 Packet[2048];
		while (!bStopped)
		{
			SendDue();
			if (!Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(5)))
			{
				continue;
			}
			for (int32 Reads = 0; Reads < 64; ++Reads)
			{
				int32 Read = 0;
				const TSharedRef<FInternetAddr> From = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
				if (!Socket->RecvFrom(Packet, UE_ARRAY_COUNT(Packet), Read, *From) || Read <= 0)
				{
					break;
				}
				Handle(Packet, Read, From->GetPort());
			}
		}
		return 0;
	}

private:
	struct FReservation
	{
		int32 ClientPort = 0;
		FString Ip;
		int32 RelayedPort = 0;
		FString AllocateTransaction;
		double ExpiresAt = 0.0;
		TMap<FString, double> Openings;
		/** Channel number to the peer ("ip:port") and when it lapses. */
		TMap<uint16, TPair<FString, double>> Channels;
	};

	static uint16 Read16(const uint8* Bytes, int32 At) { return static_cast<uint16>((Bytes[At] << 8) | Bytes[At + 1]); }
	static uint32 Read32(const uint8* Bytes, int32 At)
	{
		return (static_cast<uint32>(Bytes[At]) << 24) | (static_cast<uint32>(Bytes[At + 1]) << 16) | (static_cast<uint32>(Bytes[At + 2]) << 8) | Bytes[At + 3];
	}
	static void Add16(TArray<uint8>& Out, uint16 Value) { Out.Add(static_cast<uint8>(Value >> 8)); Out.Add(static_cast<uint8>(Value)); }
	static void Add32(TArray<uint8>& Out, uint32 Value) { Add16(Out, static_cast<uint16>(Value >> 16)); Add16(Out, static_cast<uint16>(Value)); }

	static TArray<uint8> Attribute(uint16 Kind, const TArray<uint8>& Value)
	{
		TArray<uint8> Out;
		Add16(Out, Kind);
		Add16(Out, static_cast<uint16>(Value.Num()));
		Out.Append(Value);
		while (Out.Num() % 4 != 0)
		{
			Out.Add(0);
		}
		return Out;
	}

	static TArray<uint8> Header(uint16 Type, int32 Length, const TArray<uint8>& Transaction)
	{
		TArray<uint8> Out;
		Add16(Out, Type);
		Add16(Out, static_cast<uint16>(Length));
		Add32(Out, 0x2112A442);
		Out.Append(Transaction);
		return Out;
	}

	static TArray<uint8> Utf8(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	}

	static FString TextOf(const TArray<uint8>& Bytes)
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Converted.Length(), Converted.Get());
	}

	static uint32 Crc(const TArray<uint8>& Data)
	{
		uint32 Value = 0xFFFFFFFF;
		for (const uint8 Byte : Data)
		{
			Value ^= Byte;
			for (int32 Bit = 0; Bit < 8; ++Bit)
			{
				Value = (Value & 1) != 0 ? 0xEDB88320 ^ (Value >> 1) : Value >> 1;
			}
		}
		return ~Value;
	}

	static TArray<uint8> Key(const FString& Username, const FString& Secret)
	{
		const TArray<uint8> Text = Utf8(Username + TEXT(":") + Realm + TEXT(":") + Secret);
		FMD5 Md5;
		Md5.Update(Text.GetData(), Text.Num());
		TArray<uint8> Out;
		Out.SetNumZeroed(16);
		Md5.Final(Out.GetData());
		return Out;
	}

	static TArray<uint8> Address(const FString& Ip, int32 AddressPort)
	{
		TArray<FString> Parts;
		Ip.ParseIntoArray(Parts, TEXT("."));
		uint32 Value = 0;
		for (const FString& Part : Parts)
		{
			Value = (Value << 8) | static_cast<uint32>(FCString::Atoi(*Part));
		}
		TArray<uint8> Out;
		Out.Add(0);
		Out.Add(1);
		Add16(Out, static_cast<uint16>(AddressPort ^ 0x2112));
		Add32(Out, Value ^ 0x2112A442);
		return Out;
	}

	static FString ReadAddress(const TArray<uint8>& Value, int32& OutPort)
	{
		OutPort = Read16(Value.GetData(), 2) ^ 0x2112;
		const uint32 Ip = Read32(Value.GetData(), 4) ^ 0x2112A442;
		return FString::Printf(TEXT("%u.%u.%u.%u"), (Ip >> 24) & 0xFF, (Ip >> 16) & 0xFF, (Ip >> 8) & 0xFF, Ip & 0xFF);
	}

	static FString Hex(const TArray<uint8>& Bytes)
	{
		return BytesToHex(Bytes.GetData(), Bytes.Num());
	}

	/** An answer of the given class to a request's type, its attributes, signed when a key is given, and fingerprinted. */
	static TArray<uint8> Answer(uint16 RequestType, const TArray<uint8>& Transaction, uint16 AnswerClass, const TArray<TArray<uint8>>& Attributes,
		const TArray<uint8>& SignWith, bool bFingerprint = true)
	{
		const uint16 Type = static_cast<uint16>((RequestType & ~0x0110) | AnswerClass);
		TArray<uint8> Body;
		for (const TArray<uint8>& Each : Attributes)
		{
			Body.Append(Each);
		}
		if (SignWith.Num() > 0)
		{
			TArray<uint8> Signed = Header(Type, Body.Num() + 24, Transaction);
			Signed.Append(Body);
			uint8 Hash[20];
			FSHA1::HMACBuffer(SignWith.GetData(), SignWith.Num(), Signed.GetData(), Signed.Num(), Hash);
			Body.Append(Attribute(0x0008, TArray<uint8>(Hash, 20)));
		}
		if (bFingerprint)
		{
			TArray<uint8> Fingerprinted = Header(Type, Body.Num() + 8, Transaction);
			Fingerprinted.Append(Body);
			TArray<uint8> Value;
			Add32(Value, Crc(Fingerprinted) ^ 0x5354554E);
			Body.Append(Attribute(0x8028, Value));
		}
		TArray<uint8> Whole = Header(Type, Body.Num(), Transaction);
		Whole.Append(Body);
		return Whole;
	}

	TArray<uint8> Error(uint16 Type, const TArray<uint8>& Transaction, int32 Code, const FString& Words, bool bWithChallenge, const TArray<uint8>& SignWith)
	{
		TArray<uint8> Value = { 0, 0, static_cast<uint8>(Code / 100), static_cast<uint8>(Code % 100) };
		Value.Append(Utf8(Words));
		TArray<TArray<uint8>> Attributes = { Attribute(0x0009, Value) };
		if (bWithChallenge || Code == 438)
		{
			Attributes.Add(Attribute(0x0014, Utf8(Realm)));
			Attributes.Add(Attribute(0x0015, Utf8(CurrentNonce())));
		}
		return Answer(Type, Transaction, 0x0110, Attributes, bWithChallenge ? TArray<uint8>() : SignWith);
	}

	FString CurrentNonce() const { return FString::Printf(TEXT("nonce-%d"), NonceNumber); }

	FReservation* ReservationOf(int32 ClientPort)
	{
		const double Now = FPlatformTime::Seconds();
		return Kept.FindByPredicate([ClientPort, Now](const FReservation& Each) { return Each.ClientPort == ClientPort && Each.ExpiresAt > Now; });
	}

	void Handle(const uint8* Packet, int32 Length, int32 ClientPort)
	{
		FScopeLock Locked(&Lock);
		if (PortsNoLongerHeard.Contains(ClientPort))
		{
			return;
		}
		if (!PortsHeard.Contains(ClientPort))
		{
			PortsHeard.Add(ClientPort, PortsHeard.Num() + 1);
		}
		if (AnswersOnlyPortsHeard.IsSet() && !AnswersOnlyPortsHeard->Contains(PortsHeard[ClientPort]))
		{
			return;
		}
		if (Length >= 4 && (Packet[0] & 0xC0) == 0x40)
		{
			FSeen Frame;
			Frame.Method = ChannelFrame;
			Frame.ClientPort = ClientPort;
			Seen.Add(Frame);
			if (bSilent)
			{
				return;
			}
			const uint16 Number = Read16(Packet, 0);
			const int32 DataLength = FMath::Min(static_cast<int32>(Read16(Packet, 2)), Length - 4);
			FReservation* Sender = ReservationOf(ClientPort);
			const TPair<FString, double>* Bound = Sender != nullptr ? Sender->Channels.Find(Number) : nullptr;
			if (Bound == nullptr || Bound->Value < FPlatformTime::Seconds())
			{
				return;
			}
			int32 PeerPort = 0;
			FString PeerIp;
			Bound->Key.Split(TEXT(":"), &PeerIp, nullptr);
			PeerPort = FCString::Atoi(*Bound->Key.RightChop(PeerIp.Len() + 1));
			Relay(*Sender, PeerIp, PeerPort, TArray<uint8>(Packet + 4, DataLength));
			return;
		}
		if (Length < 20 || Read32(Packet, 4) != 0x2112A442)
		{
			return;
		}
		const uint16 Type = Read16(Packet, 0);
		const uint16 Method = Type & 0x000F;
		const uint16 Class = Type & 0x0110;
		const TArray<uint8> Transaction(Packet + 8, 12);
		TMap<uint16, TArray<uint8>> Attributes;
		int32 IntegrityAt = INDEX_NONE;
		const int32 End = FMath::Min(Length, 20 + static_cast<int32>(Read16(Packet, 2)));
		for (int32 Position = 20; Position + 4 <= End;)
		{
			const uint16 Kind = Read16(Packet, Position);
			const int32 Size = Read16(Packet, Position + 2);
			if (Kind == 0x0008)
			{
				IntegrityAt = Position;
			}
			if (!Attributes.Contains(Kind) && Position + 4 + Size <= End)
			{
				Attributes.Add(Kind, TArray<uint8>(Packet + Position + 4, Size));
			}
			Position += 4 + Size + (4 - Size % 4) % 4;
		}
		const bool bSigned = Attributes.Contains(0x0006) && IntegrityAt != INDEX_NONE;
		FSeen Request;
		Request.Method = Method;
		Request.Class = Class;
		Request.bSigned = bSigned;
		Request.Transaction = Hex(Transaction);
		if (const TArray<uint8>* Lifetime = Attributes.Find(0x000D))
		{
			Request.LifetimeAsked = Read32(Lifetime->GetData(), 0);
		}
		Request.ClientPort = ClientPort;
		Seen.Add(Request);
		if (bSilent)
		{
			return;
		}
		if (Class == 0x010)
		{
			if (Method == MethodSend && Attributes.Contains(0x0012) && Attributes.Contains(0x0013))
			{
				if (FReservation* Sender = ReservationOf(ClientPort))
				{
					int32 PeerPort = 0;
					const FString PeerIp = ReadAddress(Attributes[0x0012], PeerPort);
					Relay(*Sender, PeerIp, PeerPort, Attributes[0x0013]);
				}
			}
			return;
		}
		if (Class != 0x000)
		{
			return;
		}
		if (Method == MethodBinding)
		{
			if (bAnswersBindings)
			{
				SendTo(Answer(Type, Transaction, 0x0100, { Attribute(0x0020, Address(TEXT("127.0.0.1"), ClientPort)) }, TArray<uint8>()), ClientPort);
			}
			return;
		}
		if (bSigned)
		{
			int32* Drops = DropsLeft.Find(Method);
			if (Drops != nullptr && *Drops > 0)
			{
				--*Drops;
				return;
			}
		}
		if (!bSigned)
		{
			SendTo(Error(Type, Transaction, 401, TEXT("Unauthorized"), true, TArray<uint8>()), ClientPort);
			return;
		}
		const FString Username = TextOf(Attributes[0x0006]);
		const TArray<uint8> SignWith = Key(Username, Password);
		// The signature covers the message up to it, with a length that counts the signature.
		TArray<uint8> Signed(Packet, IntegrityAt);
		Signed[2] = static_cast<uint8>((IntegrityAt - 20 + 24) >> 8);
		Signed[3] = static_cast<uint8>(IntegrityAt - 20 + 24);
		uint8 Expected[20];
		FSHA1::HMACBuffer(SignWith.GetData(), SignWith.Num(), Signed.GetData(), Signed.Num(), Expected);
		if (IntegrityAt + 24 > Length || FMemory::Memcmp(Packet + IntegrityAt + 4, Expected, 20) != 0)
		{
			SendTo(Error(Type, Transaction, 401, TEXT("Unauthorized"), true, TArray<uint8>()), ClientPort);
			return;
		}
		if (StaleNonceAnswersLeft != 0)
		{
			if (StaleNonceAnswersLeft > 0)
			{
				--StaleNonceAnswersLeft;
			}
			++NonceNumber;
			SendTo(Error(Type, Transaction, 438, TEXT("Stale Nonce"), true, TArray<uint8>()), ClientPort);
			return;
		}
		const TArray<uint8>* Nonce = Attributes.Find(0x0015);
		if (Nonce != nullptr && TextOf(*Nonce) != CurrentNonce())
		{
			SendTo(Error(Type, Transaction, 438, TEXT("Stale Nonce"), true, TArray<uint8>()), ClientPort);
			return;
		}
		const TArray<uint8> Reply = AnswerTo(Method, Type, Transaction, Attributes, ClientPort, SignWith);
		if (Reply.Num() == 0)
		{
			return;
		}
		if (Method == MethodAllocate && bHoldReservationAnswers)
		{
			HeldAnswers.Emplace(Reply, ClientPort);
			return;
		}
		SendTo(Reply, ClientPort);
	}

	TArray<uint8> AnswerTo(uint16 Method, uint16 Type, const TArray<uint8>& Transaction, const TMap<uint16, TArray<uint8>>& Attributes, int32 ClientPort,
		const TArray<uint8>& SignWith)
	{
		FReservation* Reservation = ReservationOf(ClientPort);
		const FString Id = Hex(Transaction);
		const double Now = FPlatformTime::Seconds();
		switch (Method)
		{
		case MethodAllocate:
		{
			if (RefuseReservationsWith != 0)
			{
				return Error(Type, Transaction, RefuseReservationsWith, TEXT("Refused"), false, SignWith);
			}
			if (Reservation != nullptr && Reservation->AllocateTransaction != Id)
			{
				return Error(Type, Transaction, 437, TEXT("Allocation Mismatch"), false, SignWith);
			}
			if (Reservation == nullptr)
			{
				FReservation Made;
				Made.ClientPort = ClientPort;
				Made.Ip = RelayIps[ReservationsMade++ % RelayIps.Num()];
				Made.RelayedPort = NextPort++;
				Made.AllocateTransaction = Id;
				Made.ExpiresAt = Now + ReservationLifetimeSeconds;
				Kept.Add(Made);
				Reservation = &Kept.Last();
			}
			if (bForgeNextReservation)
			{
				bForgeNextReservation = false;
				SendTo(Answer(Type, Transaction, 0x0100, { Attribute(0x0016, Address(TEXT("198.51.100.66"), 4444)) }, Key(TEXT("someone-else"), TEXT("forged"))),
					ClientPort);
			}
			TArray<uint8> Lifetime;
			Add32(Lifetime, static_cast<uint32>(ReservationLifetimeSeconds));
			return Answer(Type, Transaction, 0x0100, { Attribute(0x0016, Address(Reservation->Ip, Reservation->RelayedPort)),
				Attribute(0x0020, Address(TEXT("127.0.0.1"), ClientPort)), Attribute(0x000D, Lifetime) }, SignWith);
		}
		case MethodRefresh:
		{
			if (Reservation == nullptr)
			{
				return Error(Type, Transaction, 437, TEXT("Allocation Mismatch"), false, SignWith);
			}
			const TArray<uint8>* Value = Attributes.Find(0x000D);
			const uint32 Asked = Value != nullptr ? Read32(Value->GetData(), 0) : 600;
			TArray<uint8> Lifetime;
			if (Asked == 0)
			{
				const int32 Released = ClientPort;
				Kept.RemoveAll([Released](const FReservation& Each) { return Each.ClientPort == Released; });
				Add32(Lifetime, 0);
				return Answer(Type, Transaction, 0x0100, { Attribute(0x000D, Lifetime) }, SignWith);
			}
			Reservation->ExpiresAt = Now + ReservationLifetimeSeconds;
			Add32(Lifetime, static_cast<uint32>(ReservationLifetimeSeconds));
			return Answer(Type, Transaction, 0x0100, { Attribute(0x000D, Lifetime) }, SignWith);
		}
		case MethodCreatePermission:
		{
			if (Reservation == nullptr)
			{
				return Error(Type, Transaction, 437, TEXT("Allocation Mismatch"), false, SignWith);
			}
			int32 PeerPort = 0;
			const FString PeerIp = ReadAddress(Attributes[0x0012], PeerPort);
			if (bRefuseOpeningsNamingNoCode)
			{
				return Answer(Type, Transaction, 0x0110, {}, SignWith);
			}
			if (RefusedOpenings.Contains(PeerIp))
			{
				return Error(Type, Transaction, 403, TEXT("Forbidden IP"), false, SignWith);
			}
			Reservation->Openings.Add(PeerIp, Now + OpeningLifetimeSeconds);
			return Answer(Type, Transaction, 0x0100, {}, SignWith);
		}
		case MethodChannelBind:
		{
			if (Reservation == nullptr)
			{
				return Error(Type, Transaction, 437, TEXT("Allocation Mismatch"), false, SignWith);
			}
			const uint16 Number = Read16(Attributes[0x000C].GetData(), 0);
			int32 PeerPort = 0;
			const FString PeerIp = ReadAddress(Attributes[0x0012], PeerPort);
			const FString Peer = FString::Printf(TEXT("%s:%d"), *PeerIp, PeerPort);
			if (Number < 0x4000 || Number > 0x4FFF)
			{
				return Error(Type, Transaction, 400, TEXT("Bad Request"), false, SignWith);
			}
			for (const TPair<uint16, TPair<FString, double>>& Channel : Reservation->Channels)
			{
				if ((Channel.Key == Number) != Channel.Value.Key.Equals(Peer, ESearchCase::CaseSensitive))
				{
					return Error(Type, Transaction, 400, TEXT("Bad Request"), false, SignWith);
				}
			}
			Reservation->Channels.Add(Number, TPair<FString, double>(Peer, Now + ChannelLifetimeSeconds));
			Reservation->Openings.Add(PeerIp, Now + OpeningLifetimeSeconds);
			return Answer(Type, Transaction, 0x0100, {}, SignWith);
		}
		default:
			return Error(Type, Transaction, 400, TEXT("Bad Request"), false, SignWith);
		}
	}

	/** Data from one reservation to another: the sender needs an opening to the peer's IP, the receiver one to the sender's. */
	void Relay(const FReservation& Sender, const FString& PeerIp, int32 PeerPort, const TArray<uint8>& Data)
	{
		const double Now = FPlatformTime::Seconds();
		const double* SenderOpen = Sender.Openings.Find(PeerIp);
		if (Sender.ExpiresAt < Now || SenderOpen == nullptr || *SenderOpen < Now)
		{
			return;
		}
		const FReservation* Receiver = Kept.FindByPredicate([&PeerIp, PeerPort, Now](const FReservation& Each)
		{
			return Each.Ip == PeerIp && Each.RelayedPort == PeerPort && Each.ExpiresAt > Now;
		});
		const double* ReceiverOpen = Receiver != nullptr ? Receiver->Openings.Find(Sender.Ip) : nullptr;
		if (ReceiverOpen == nullptr || *ReceiverOpen < Now)
		{
			return;
		}
		const FString SenderAddress = FString::Printf(TEXT("%s:%d"), *Sender.Ip, Sender.RelayedPort);
		for (const TPair<uint16, TPair<FString, double>>& Channel : Receiver->Channels)
		{
			if (Channel.Value.Key.Equals(SenderAddress, ESearchCase::CaseSensitive) && Channel.Value.Value > Now)
			{
				TArray<uint8> Frame;
				Add16(Frame, Channel.Key);
				Add16(Frame, static_cast<uint16>(Data.Num()));
				Frame.Append(Data);
				while (Frame.Num() % 4 != 0)
				{
					Frame.Add(0);
				}
				SendTo(Frame, Receiver->ClientPort);
				return;
			}
		}
		const FGuid Random = FGuid::NewGuid();
		TArray<uint8> Transaction;
		Add32(Transaction, Random.A);
		Add32(Transaction, Random.B);
		Add32(Transaction, Random.C);
		SendTo(Answer(0x0017, Transaction, 0x0010, { Attribute(0x0012, Address(Sender.Ip, Sender.RelayedPort)), Attribute(0x0013, Data) }, TArray<uint8>(), false),
			Receiver->ClientPort);
	}

	void SendTo(const TArray<uint8>& Bytes, int32 ClientPort)
	{
		const double Delay = AnswerDelaySeconds;
		if (Delay > 0.0)
		{
			Later.Add({ FPlatformTime::Seconds() + Delay, Bytes, ClientPort });
			return;
		}
		SendNow(Bytes, ClientPort);
	}

	void SendDue()
	{
		TArray<FLater> Due;
		{
			FScopeLock Locked(&Lock);
			const double Now = FPlatformTime::Seconds();
			for (int32 Index = Later.Num() - 1; Index >= 0; --Index)
			{
				if (Later[Index].At <= Now)
				{
					Due.Insert(Later[Index], 0);
					Later.RemoveAt(Index);
				}
			}
		}
		for (const FLater& Each : Due)
		{
			SendNow(Each.Bytes, Each.ClientPort);
		}
	}

	void SendNow(const TArray<uint8>& Bytes, int32 ClientPort)
	{
		const TSharedRef<FInternetAddr> To = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
		bool bValid = false;
		To->SetIp(TEXT("127.0.0.1"), bValid);
		To->SetPort(ClientPort);
		int32 Sent = 0;
		// A client already gone.
		Socket->SendTo(Bytes.GetData(), Bytes.Num(), Sent, *To);
	}

	struct FLater
	{
		double At = 0.0;
		TArray<uint8> Bytes;
		int32 ClientPort = 0;
	};

	FSocket* Socket = nullptr;
	int32 Port = 0;
	FRunnableThread* Thread = nullptr;
	std::atomic<bool> bStopped{ false };
	mutable FCriticalSection Lock;
	TArray<FReservation> Kept;
	TArray<FSeen> Seen;
	TMap<uint16, int32> DropsLeft;
	int32 StaleNonceAnswersLeft = 0;
	int32 NonceNumber = 1;
	int32 NextPort = 50000;
	int32 ReservationsMade = 0;
	TMap<int32, int32> PortsHeard;
	TSet<int32> PortsNoLongerHeard;
	TArray<TPair<TArray<uint8>, int32>> HeldAnswers;
	TArray<FLater> Later;
};

#endif
