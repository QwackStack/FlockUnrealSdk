// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/Relay/FlockRelayPeer.h"
#include "Multiplayer/FlockDirectAddresses.h"

bool FFlockRelayPeer::TryParse(const FString& Text, FFlockRelayPeer& OutPeer)
{
	OutPeer = FFlockRelayPeer();
	int32 Colon = INDEX_NONE;
	if (!Text.FindLastChar(TEXT(':'), Colon) || Colon <= 0)
	{
		return false;
	}
	const FString PortText = Text.Mid(Colon + 1);
	if (PortText.IsEmpty() || PortText.Len() > 5)
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
	// The address as machines write it: four numbers 0 to 255 and nothing else, read by the one reader of such text.
	const FString Ip = FFlockDirectAddresses::Ipv4Value(Text.Left(Colon));
	if (Port < 1 || Port > 65535 || Ip.IsEmpty())
	{
		return false;
	}
	TArray<FString> Parts;
	Ip.ParseIntoArray(Parts, TEXT("."));
	uint32 Address = 0;
	for (const FString& Part : Parts)
	{
		Address = (Address << 8) | static_cast<uint32>(FCString::Atoi(*Part));
	}
	OutPeer = FFlockRelayPeer(Address, Port);
	return true;
}
