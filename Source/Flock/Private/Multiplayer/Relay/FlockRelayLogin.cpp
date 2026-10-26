// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/Relay/FlockRelayLogin.h"
#include "Dom/JsonObject.h"
#include "Multiplayer/FlockDirectAddresses.h"
#include "Multiplayer/FlockWireFields.h"



bool FFlockRelayLoginList::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockRelayLoginList& OutStruct, FString& OutError)
{
	OutStruct = FFlockRelayLoginList();
	const TSharedPtr<FJsonValue> Servers = FlockWireFields::SpelledExactly(*Object, TEXT("ice_servers"));
	if (!Servers.IsValid() || Servers->Type != EJson::Array)
	{
		OutError = TEXT("The server's answer lists no servers");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Server : Servers->AsArray())
	{
		if (!Server.IsValid() || Server->Type != EJson::Object)
		{
			continue;
		}
		const TSharedPtr<FJsonObject> ServerObject = Server->AsObject();
		const TSharedPtr<FJsonValue> Urls = FlockWireFields::SpelledExactly(*ServerObject, TEXT("urls"));
		if (!Urls.IsValid() || Urls->Type != EJson::Array)
		{
			continue;
		}
		FEntry Entry;
		for (const TSharedPtr<FJsonValue>& Url : Urls->AsArray())
		{
			const FString Text = FlockWireFields::TextOf(Url);
			if (!Text.IsEmpty())
			{
				Entry.Urls.Add(Text);
			}
		}
		// Null for a STUN server, which needs no login.
		Entry.Username = FlockWireFields::TextOf(FlockWireFields::SpelledExactly(*ServerObject, TEXT("username")));
		Entry.Credential = FlockWireFields::TextOf(FlockWireFields::SpelledExactly(*ServerObject, TEXT("credential")));
		OutStruct.Entries.Add(MoveTemp(Entry));
	}
	const TSharedPtr<FJsonValue> Ttl = FlockWireFields::SpelledExactly(*Object, TEXT("ttl"));
	OutStruct.TtlSeconds = Ttl.IsValid() && Ttl->Type == EJson::Number ? static_cast<int32>(Ttl->AsNumber()) : 0;
	const TSharedPtr<FJsonValue> Paused = FlockWireFields::SpelledExactly(*Object, TEXT("relay_paused"));
	OutStruct.bRelayPaused = Paused.IsValid() && Paused->Type == EJson::Boolean && Paused->AsBool();
	return true;
}

TArray<FFlockRelayLogin> FFlockRelayLogin::InAnswer(const FFlockRelayLoginList& Answer)
{
	TArray<FFlockRelayLogin> Logins;
	for (const FFlockRelayLoginList::FEntry& Entry : Answer.Entries)
	{
		if (Entry.Username.IsEmpty() || Entry.Credential.IsEmpty())
		{
			continue;
		}
		for (const FString& Url : Entry.Urls)
		{
			FFlockRelayLogin Login;
			if (TryReadServer(Url, Login.Host, Login.Port, Login.Transport))
			{
				Login.Username = Entry.Username;
				Login.Password = Entry.Credential;
				Logins.Add(MoveTemp(Login));
			}
		}
	}
	return Logins;
}

FString FFlockRelayLogin::GetWay() const
{
	const TCHAR* Over = Transport == EFlockRelayTransport::Tls ? TEXT("TLS") : Transport == EFlockRelayTransport::Tcp ? TEXT("TCP") : TEXT("UDP");
	return FString::Printf(TEXT("%s over %s%s"), *ToString(), Over, bOverIpv6 ? TEXT(" on IPv6") : TEXT(""));
}

namespace
{
	/** The name in "name:port", or the whole text when it ends in no port. */
	FString NameOf(const FString& Server)
	{
		int32 Colon = INDEX_NONE;
		if (Server.FindLastChar(TEXT(':'), Colon) && Colon > 0 && Server.Mid(Colon + 1).IsNumeric())
		{
			return Server.Left(Colon);
		}
		return Server;
	}
}

bool FFlockRelayLogin::SameServer(const FString& One, const FString& Other)
{
	return !One.IsEmpty() && !Other.IsEmpty() && NameOf(One).Equals(NameOf(Other), ESearchCase::IgnoreCase);
}

TArray<FFlockRelayLogin> FFlockRelayLogin::InTheOrderTried(const TArray<FFlockRelayLogin>& Logins)
{
	struct FTurn
	{
		EFlockRelayTransport Transport;
		bool bOverIpv6;
	};
	// TCP and TLS are for a network that blocks UDP to the relay; IPv6 for one that has IPv6 alone.
	static const FTurn TheOrder[] = {
		{ EFlockRelayTransport::Udp, false }, { EFlockRelayTransport::Udp, true },
		{ EFlockRelayTransport::Tcp, false }, { EFlockRelayTransport::Tls, false },
		{ EFlockRelayTransport::Tcp, true }, { EFlockRelayTransport::Tls, true },
	};
	TArray<FFlockRelayLogin> Ordered;
	Ordered.Reserve(Logins.Num() * 2);
	for (const FTurn& Turn : TheOrder)
	{
		for (const FFlockRelayLogin& Login : Logins)
		{
			if (Login.Transport != Turn.Transport)
			{
				continue;
			}
			if (!Turn.bOverIpv6)
			{
				Ordered.Add(Login);
			}
			else if (FFlockDirectAddresses::Ipv4Value(Login.Host).IsEmpty())
			{
				// A server Flock names: its IPv6 address is looked up for this way; one given as an address has no other.
				FFlockRelayLogin OverIpv6 = Login;
				OverIpv6.bOverIpv6 = true;
				Ordered.Add(MoveTemp(OverIpv6));
			}
		}
	}
	return Ordered;
}

bool FFlockRelayLogin::TryReadServer(const FString& Url, FString& OutHost, int32& OutPort, EFlockRelayTransport& OutTransport)
{
	OutHost.Reset();
	OutPort = 0;
	OutTransport = EFlockRelayTransport::Udp;
	bool bOverTls = false;
	FString Rest;
	if (Url.StartsWith(TEXT("turns:"), ESearchCase::IgnoreCase))
	{
		bOverTls = true;
		Rest = Url.Mid(6);
	}
	else if (Url.StartsWith(TEXT("turn:"), ESearchCase::IgnoreCase))
	{
		Rest = Url.Mid(5);
	}
	else
	{
		return false;
	}
	FString Asked;
	bool bNamesTransport = false;
	int32 Query = INDEX_NONE;
	if (Rest.FindChar(TEXT('?'), Query))
	{
		TArray<FString> Parts;
		Rest.Mid(Query + 1).ParseIntoArray(Parts, TEXT("&"));
		for (const FString& Part : Parts)
		{
			if (Part.StartsWith(TEXT("transport="), ESearchCase::IgnoreCase))
			{
				bNamesTransport = true;
				Asked = Part.Mid(10);
			}
		}
		Rest.LeftInline(Query);
	}
	EFlockRelayTransport Transport = EFlockRelayTransport::Udp;
	if (bOverTls)
	{
		// TLS rides on TCP; "turns:" over UDP is DTLS, which this client does not speak.
		if (bNamesTransport && !Asked.Equals(TEXT("tcp"), ESearchCase::IgnoreCase))
		{
			return false;
		}
		Transport = EFlockRelayTransport::Tls;
	}
	else if (!bNamesTransport || Asked.Equals(TEXT("udp"), ESearchCase::IgnoreCase))
	{
		Transport = EFlockRelayTransport::Udp;
	}
	else if (Asked.Equals(TEXT("tcp"), ESearchCase::IgnoreCase))
	{
		Transport = EFlockRelayTransport::Tcp;
	}
	else
	{
		return false;
	}
	if (Rest.IsEmpty() || Rest.StartsWith(TEXT("[")))
	{
		return false;
	}
	int32 Colon = INDEX_NONE;
	if (!Rest.FindLastChar(TEXT(':'), Colon))
	{
		OutHost = Rest;
		OutPort = bOverTls ? DefaultTlsPort : DefaultPort;
		OutTransport = Transport;
		return true;
	}
	const FString PortText = Rest.Mid(Colon + 1);
	if (Colon == 0 || PortText.IsEmpty() || PortText.Len() > 5)
	{
		return false;
	}
	for (const TCHAR Character : PortText)
	{
		if (Character < TEXT('0') || Character > TEXT('9'))
		{
			return false;
		}
	}
	const int32 Port = FCString::Atoi(*PortText);
	if (Port < 1 || Port > 65535)
	{
		return false;
	}
	OutHost = Rest.Left(Colon);
	OutPort = Port;
	OutTransport = Transport;
	return true;
}

TArray<FFlockRelayStunServer> FFlockRelayStunServer::InAnswer(const FFlockRelayLoginList& Answer)
{
	TArray<FFlockRelayStunServer> Servers;
	for (const FFlockRelayLoginList::FEntry& Entry : Answer.Entries)
	{
		for (const FString& Url : Entry.Urls)
		{
			FFlockRelayStunServer Server;
			if (FFlockDirectAddresses::ParseStunUrl(Url, Server.Host, Server.Port))
			{
				Servers.Add(MoveTemp(Server));
			}
		}
	}
	return Servers;
}
