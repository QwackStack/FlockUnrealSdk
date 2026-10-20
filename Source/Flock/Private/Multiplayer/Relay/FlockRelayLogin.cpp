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
			if (TryReadUdpServer(Url, Login.Host, Login.Port))
			{
				Login.Username = Entry.Username;
				Login.Password = Entry.Credential;
				Logins.Add(MoveTemp(Login));
			}
		}
	}
	return Logins;
}

bool FFlockRelayLogin::TryReadUdpServer(const FString& Url, FString& OutHost, int32& OutPort)
{
	OutHost.Reset();
	OutPort = 0;
	if (!Url.StartsWith(TEXT("turn:"), ESearchCase::IgnoreCase))
	{
		return false;
	}
	FString Rest = Url.Mid(5);
	int32 Query = INDEX_NONE;
	if (Rest.FindChar(TEXT('?'), Query))
	{
		TArray<FString> Parts;
		Rest.Mid(Query + 1).ParseIntoArray(Parts, TEXT("&"));
		FString Transport;
		bool bNamesTransport = false;
		for (const FString& Part : Parts)
		{
			if (Part.StartsWith(TEXT("transport="), ESearchCase::IgnoreCase))
			{
				bNamesTransport = true;
				Transport = Part.Mid(10);
			}
		}
		if (bNamesTransport && !Transport.Equals(TEXT("udp"), ESearchCase::IgnoreCase))
		{
			return false;
		}
		Rest.LeftInline(Query);
	}
	if (Rest.IsEmpty() || Rest.StartsWith(TEXT("[")))
	{
		return false;
	}
	int32 Colon = INDEX_NONE;
	if (!Rest.FindLastChar(TEXT(':'), Colon))
	{
		OutHost = Rest;
		OutPort = DefaultPort;
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
