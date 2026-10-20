// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "IPAddress.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

/**
 * A STUN server on this machine's loopback: answers each binding request with the public address a test chooses, or stays
 * silent. Served when the test calls Serve, between the SDK's frames, so nothing runs on another thread. Include only from
 * guarded test files.
 */
class FFlockFakeStunServer
{
public:
	/** The address this device is seen from; the port the request came from is answered as it is. */
	FString PublicAddress = TEXT("203.0.113.7");
	bool bSilent = false;
	int32 Requests = 0;

	FFlockFakeStunServer()
	{
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		Socket = Sockets->CreateSocket(NAME_DGram, TEXT("Fake STUN server"), FNetworkProtocolTypes::IPv4);
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
		const TSharedRef<FInternetAddr> Bound = Sockets->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
		Socket->GetAddress(*Bound);
		Port = Bound->GetPort();
	}

	~FFlockFakeStunServer()
	{
		if (Socket != nullptr)
		{
			Socket->Close();
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		}
	}

	bool IsReady() const { return Socket != nullptr && Port > 0; }
	int32 GetPort() const { return Port; }
	FString Url() const { return FString::Printf(TEXT("stun:127.0.0.1:%d"), Port); }

	/** Reads every request waiting and answers each (unless silent): a binding success holding one XOR-MAPPED-ADDRESS. */
	void Serve()
	{
		if (Socket == nullptr)
		{
			return;
		}
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		uint8 Request[1500];
		for (int32 Reads = 0; Reads < 32; ++Reads)
		{
			int32 Received = 0;
			const TSharedRef<FInternetAddr> From = Sockets->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
			if (!Socket->RecvFrom(Request, UE_ARRAY_COUNT(Request), Received, *From) || Received <= 0)
			{
				return;
			}
			++Requests;
			if (bSilent || Received < 20)
			{
				continue;
			}
			const TArray<uint8> Answer = AnswerTo(Request, From->GetPort());
			int32 Sent = 0;
			Socket->SendTo(Answer.GetData(), Answer.Num(), Sent, *From);
		}
	}

private:
	TArray<uint8> AnswerTo(const uint8* Request, int32 FromPort) const
	{
		TArray<uint8> Answer;
		Answer.SetNumZeroed(32);
		Answer[0] = 0x01;
		Answer[1] = 0x01;
		Answer[3] = 12;
		FMemory::Memcpy(&Answer[4], &Request[4], 16);
		Answer[21] = 0x20;
		Answer[23] = 8;
		Answer[25] = 0x01;
		const int32 MaskedPort = FromPort ^ 0x2112;
		Answer[26] = static_cast<uint8>(MaskedPort >> 8);
		Answer[27] = static_cast<uint8>(MaskedPort);
		TArray<FString> Parts;
		PublicAddress.ParseIntoArray(Parts, TEXT("."));
		const uint8 Cookie[] = { 0x21, 0x12, 0xA4, 0x42 };
		for (int32 Index = 0; Index < 4 && Index < Parts.Num(); ++Index)
		{
			Answer[28 + Index] = static_cast<uint8>(FCString::Atoi(*Parts[Index])) ^ Cookie[Index];
		}
		return Answer;
	}

	FSocket* Socket = nullptr;
	int32 Port = 0;
};
