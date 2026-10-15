// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/FlockDirectAddresses.h"
#include "Dom/JsonObject.h"
#include "IPAddress.h"
#include "Misc/Guid.h"
#include "Multiplayer/FlockMultiplayerSessionRequests.h"
#include "Multiplayer/FlockUdpSockets.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

namespace
{
	constexpr uint32 StunMagicCookie = 0x2112A442;
	constexpr uint16 StunBindingRequest = 0x0001;
	constexpr uint16 StunBindingSuccess = 0x0101;
	constexpr uint16 StunMappedAddress = 0x0001;
	constexpr uint16 StunXorMappedAddress = 0x0020;
	/** Where the LAN address is routed toward when no STUN server is known: an address on the internet, sent nothing. */
	const TCHAR* const InternetAddress = TEXT("8.8.8.8");
	/** The most datagrams a search reads in one frame; a STUN server sends one answer. */
	constexpr int32 MostReadsAFrame = 16;

	uint16 ReadUInt16(const uint8* Bytes, int32 At)
	{
		return static_cast<uint16>((Bytes[At] << 8) | Bytes[At + 1]);
	}

	uint32 ReadUInt32(const uint8* Bytes, int32 At)
	{
		return (static_cast<uint32>(Bytes[At]) << 24) | (static_cast<uint32>(Bytes[At + 1]) << 16) | (static_cast<uint32>(Bytes[At + 2]) << 8)
			| static_cast<uint32>(Bytes[At + 3]);
	}

	/** A key spelled exactly so (letter case counts), as a descriptor's own writer wrote it. */
	TSharedPtr<FJsonValue> FieldSpelledExactly(const FJsonObject& Object, const TCHAR* Key)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object.Values)
		{
			if (Pair.Key.Equals(Key, ESearchCase::CaseSensitive))
			{
				return Pair.Value;
			}
		}
		return nullptr;
	}

	FString TextOf(const TSharedPtr<FJsonValue>& Value)
	{
		FString Text;
		if (Value.IsValid() && Value->Type == EJson::String)
		{
			Value->TryGetString(Text);
		}
		return Text;
	}

	/** A whole port from 1 to 65535, sent as a number or as digits; 0 for anything else. */
	int32 PortOf(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return 0;
		}
		if (Value->Type == EJson::Number)
		{
			const double Number = Value->AsNumber();
			return Number >= 1.0 && Number <= 65535.0 && Number == FMath::FloorToDouble(Number) ? static_cast<int32>(Number) : 0;
		}
		const FString Text = TextOf(Value);
		if (Text.IsEmpty() || Text.Len() > 10)
		{
			return 0;
		}
		for (const TCHAR Character : Text)
		{
			if (!FChar::IsDigit(Character))
			{
				return 0;
			}
		}
		const int64 Port = FCString::Atoi64(*Text);
		return Port >= 1 && Port <= 65535 ? static_cast<int32>(Port) : 0;
	}
}

FFlockDirectAddresses::FFlockDirectAddresses(const TSharedRef<FFlockMultiplayerSessionRequests>& InRequests, TFunction<double()> InNow,
	TFunction<int32()> InCurrentSignInNumber, const TSharedRef<IFlockLogger>& InLogger)
	: Requests(InRequests)
	, Now(MoveTemp(InNow))
	, CurrentSignInNumber(MoveTemp(InCurrentSignInNumber))
	, Logger(InLogger)
{
}

FFlockDirectAddresses::~FFlockDirectAddresses()
{
	for (const FSearchRef& Search : Searches)
	{
		CloseSocket(*Search);
	}
}

FFlockRequestHandle FFlockDirectAddresses::Find(int32 SignInNumber, TFunction<void(TFlockResult<FFlockDeviceAddresses>)> OnFound)
{
	const FSearchRef Search = MakeShared<FSearch>();
	Search->SignInNumber = SignInNumber;
	Search->OnFound = MoveTemp(OnFound);
	Searches.Add(Search);
	const TWeakPtr<FFlockDirectAddresses> WeakSelf = AsShared();
	const TWeakPtr<FSearch> WeakSearch = Search;
	Search->Token->Canceller = [WeakSelf, WeakSearch]()
	{
		const TSharedPtr<FFlockDirectAddresses> Self = WeakSelf.Pin();
		const TSharedPtr<FSearch> Pinned = WeakSearch.Pin();
		if (Self.IsValid() && Pinned.IsValid())
		{
			Self->EndWith(Pinned.ToSharedRef(), FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Request cancelled")));
		}
	};
	const FFlockRequestHandle Handle(Search->Token);
	if (StunServers.IsSet())
	{
		StartWithServers(Search, StunServers.GetValue());
		return Handle;
	}
	FFlockRequestHandle Read = Requests->ReadStunServers(SignInNumber, [WeakSelf, WeakSearch](TFlockResult<FFlockStunServerList> Answered)
	{
		const TSharedPtr<FFlockDirectAddresses> Self = WeakSelf.Pin();
		const TSharedPtr<FSearch> Pinned = WeakSearch.Pin();
		if (!Self.IsValid() || !Pinned.IsValid() || Pinned->Step != EStep::ReadingServers)
		{
			return;
		}
		if (!Answered.bSuccess)
		{
			// The sign-in the search acts for ended, or its caller gave up: nothing more is asked for it.
			if (Answered.Error.Type == EFlockErrorType::Cancelled)
			{
				Self->EndWith(Pinned.ToSharedRef(), Answered.Error);
				return;
			}
			Self->Logger->LogWarning(FString::Printf(TEXT("The STUN servers could not be read from Flock: %s. This device's public address stays unknown this time."),
				*Answered.Error.Message));
			Self->StartWithServers(Pinned.ToSharedRef(), TArray<FStunServer>());
			return;
		}
		// The list is the game's, kept whoever asked; a search for a sign-in that ended meanwhile asks nothing more.
		const TArray<FStunServer> Listed = ServersIn(Answered.Value);
		Self->StunServers = Listed;
		if (Self->CurrentSignInNumber() != Pinned->SignInNumber)
		{
			Self->EndWith(Pinned.ToSharedRef(), FFlockError::Make(EFlockErrorType::Cancelled,
				TEXT("The player signed out or another player signed in while this device's addresses were being found")));
			return;
		}
		Self->StartWithServers(Pinned.ToSharedRef(), Listed);
	});
	if (Search->Step == EStep::ReadingServers)
	{
		Search->ServersRead = Read;
	}
	return Handle;
}

void FFlockDirectAddresses::Tick()
{
	// A search that ends here leaves the list, so the frame walks a copy.
	const TArray<FSearchRef> Running = Searches;
	for (const FSearchRef& Search : Running)
	{
		Advance(Search);
	}
}

void FFlockDirectAddresses::StopForShutdown()
{
	const TArray<FSearchRef> Running = Searches;
	for (const FSearchRef& Search : Running)
	{
		EndWith(Search, FFlockError::Make(EFlockErrorType::Cancelled, TEXT("Flock shut down while this device's addresses were being found")));
	}
}

TArray<FFlockDirectAddresses::FStunServer> FFlockDirectAddresses::ServersIn(const FFlockStunServerList& List)
{
	TArray<FStunServer> Found;
	for (const FString& Url : List.Urls)
	{
		FStunServer Server;
		if (ParseStunUrl(Url, Server.Host, Server.Port))
		{
			Found.Add(Server);
		}
	}
	return Found;
}

void FFlockDirectAddresses::StartWithServers(const FSearchRef& Search, const TArray<FStunServer>& Servers)
{
	Search->ServersRead = FFlockRequestHandle();
	Search->Servers = Servers;
	Search->Index = -1;
	NextServer(Search);
}

void FFlockDirectAddresses::NextServer(const FSearchRef& Search)
{
	CloseSocket(*Search);
	Search->LookUp.Reset();
	++Search->Index;
	if (!Search->Servers.IsValidIndex(Search->Index))
	{
		Finish(Search);
		return;
	}
	const FString Host = Search->Servers[Search->Index].Host;
	const FString Written = Ipv4Value(Host);
	if (!Written.IsEmpty())
	{
		// An address as written is taken as it is.
		Search->ServerAddress = Written;
		Ask(Search);
		return;
	}
	if (Host.Contains(TEXT(":")) || Host.Contains(TEXT(" ")))
	{
		// An IPv6 address, or no name at all: the request is plain UDP to IPv4.
		NextServer(Search);
		return;
	}
	Search->Step = EStep::LookingUp;
	// A lookup can hang for as long as the network's resolver does, so it gets the STUN wait too.
	Search->Deadline = Now() + StunWaitSeconds;
	const TSharedRef<FLookUpCell, ESPMode::ThreadSafe> Cell = MakeShared<FLookUpCell, ESPMode::ThreadSafe>();
	Search->LookUp = Cell;
	TFunction<void(const FString&)> Answer = [Cell](const FString& Ipv4)
	{
		FScopeLock Locked(&Cell->Lock);
		Cell->bDone = true;
		Cell->Ipv4 = Ipv4;
	};
	if (NameLookUpForTesting)
	{
		NameLookUpForTesting(Host, MoveTemp(Answer));
		return;
	}
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (Sockets == nullptr)
	{
		NextServer(Search);
		return;
	}
	// Answered on one of the engine's threads, into the cell the game thread reads each frame.
	Sockets->GetAddressInfoAsync([Answer](FAddressInfoResult Result)
	{
		FString Ipv4;
		for (const FAddressInfoResultData& Found : Result.Results)
		{
			Ipv4 = Ipv4Value(Found.Address->ToString(false));
			if (!Ipv4.IsEmpty())
			{
				break;
			}
		}
		Answer(Ipv4);
	}, *Host, nullptr, EAddressInfoFlags::Default, FNetworkProtocolTypes::IPv4, SOCKTYPE_Datagram);
}

void FFlockDirectAddresses::Ask(const FSearchRef& Search)
{
	Search->Step = EStep::Asking;
	if (Search->FirstServerAddress.IsEmpty())
	{
		Search->FirstServerAddress = Search->ServerAddress;
	}
	const FGuid Random = FGuid::NewGuid();
	Search->Transaction.Reset();
	for (const uint32 Part : { Random.A, Random.B, Random.C })
	{
		Search->Transaction.Add(static_cast<uint8>(Part >> 24));
		Search->Transaction.Add(static_cast<uint8>(Part >> 16));
		Search->Transaction.Add(static_cast<uint8>(Part >> 8));
		Search->Transaction.Add(static_cast<uint8>(Part));
	}
	Search->Socket = FFlockUdpSockets::Open(TEXT("Flock STUN request"));
	if (Search->Socket == nullptr || !SendRequest(Search))
	{
		NextServer(Search);
		return;
	}
	Search->Deadline = Now() + StunWaitSeconds;
}

bool FFlockDirectAddresses::SendRequest(const FSearchRef& Search)
{
	const TArray<uint8> Request = StunRequest(Search->Transaction);
	const int32 Port = Search->Servers[Search->Index].Port;
	int32 Sent = 0;
	AskedFrom = Search->Socket->GetProtocol();
	const bool bSendRefused = FFlockUdpSockets::bDualStackSendRefusedForTesting && FFlockUdpSockets::IsDualStack(*Search->Socket);
	if (!bSendRefused && Search->Socket->SendTo(Request.GetData(), Request.Num(), Sent,
		*FFlockUdpSockets::Toward(*Search->Socket, Search->ServerAddress, Port)) && Sent == Request.Num())
	{
		return true;
	}
	if (!FFlockUdpSockets::IsDualStack(*Search->Socket))
	{
		return false;
	}
	// A platform that makes a dual-stack socket but cannot send IPv4 through it gets a plain IPv4 socket instead.
	CloseSocket(*Search);
	Search->Socket = FFlockUdpSockets::OpenIpv4(TEXT("Flock STUN request"));
	if (Search->Socket == nullptr)
	{
		return false;
	}
	AskedFrom = Search->Socket->GetProtocol();
	return Search->Socket->SendTo(Request.GetData(), Request.Num(), Sent,
		*FFlockUdpSockets::Toward(*Search->Socket, Search->ServerAddress, Port)) && Sent == Request.Num();
}

void FFlockDirectAddresses::Advance(const FSearchRef& Search)
{
	if (Search->Step == EStep::LookingUp)
	{
		bool bDone = false;
		FString Ipv4;
		{
			FScopeLock Locked(&Search->LookUp->Lock);
			bDone = Search->LookUp->bDone;
			Ipv4 = Search->LookUp->Ipv4;
		}
		if (bDone)
		{
			Search->ServerAddress = Ipv4Value(Ipv4);
			if (Search->ServerAddress.IsEmpty())
			{
				NextServer(Search);
			}
			else
			{
				Ask(Search);
			}
		}
		else if (Now() >= Search->Deadline)
		{
			NextServer(Search);
		}
		return;
	}
	if (Search->Step != EStep::Asking || Search->Socket == nullptr)
	{
		return;
	}
	uint8 Buffer[1500];
	for (int32 Reads = 0; Reads < MostReadsAFrame; ++Reads)
	{
		int32 Received = 0;
		const TSharedRef<FInternetAddr> Sender = FFlockUdpSockets::AnySender(*Search->Socket);
		if (!Search->Socket->RecvFrom(Buffer, UE_ARRAY_COUNT(Buffer), Received, *Sender) || Received <= 0)
		{
			break;
		}
		FString Address;
		int32 PublicPort = 0;
		if (PublicEndpointInStunAnswer(Buffer, Received, Search->Transaction, Address, PublicPort))
		{
			Search->Public = Address;
			Finish(Search);
			return;
		}
	}
	if (Now() >= Search->Deadline)
	{
		NextServer(Search);
	}
}

void FFlockDirectAddresses::Finish(const FSearchRef& Search)
{
	if (Search->Step == EStep::Done)
	{
		return;
	}
	Search->Step = EStep::Done;
	CloseSocket(*Search);
	Searches.Remove(Search);
	FFlockDeviceAddresses Found;
	Found.Public = Search->Public;
	Found.Lan = LanAddressToward(Search->FirstServerAddress.IsEmpty() ? FString(InternetAddress) : Search->FirstServerAddress);
	if (Found.Public.IsEmpty() && !bToldPublicAddressUnknown)
	{
		bToldPublicAddressUnknown = true;
		Logger->LogWarning(TEXT("This device's public address could not be found (no STUN server answered, or none could be read from Flock), so a "
			"direct connection offers its LAN address alone, which only players on the same network can reach."));
	}
	if (Search->OnFound)
	{
		Search->OnFound(TFlockResult<FFlockDeviceAddresses>::Ok(Found));
	}
}

void FFlockDirectAddresses::EndWith(const FSearchRef& Search, const FFlockError& Error)
{
	if (Search->Step == EStep::Done)
	{
		return;
	}
	Search->Step = EStep::Done;
	CloseSocket(*Search);
	Search->ServersRead.Cancel();
	Searches.Remove(Search);
	if (Search->OnFound)
	{
		Search->OnFound(TFlockResult<FFlockDeviceAddresses>::Fail(Error));
	}
}

void FFlockDirectAddresses::CloseSocket(FSearch& Search)
{
	FFlockUdpSockets::Close(Search.Socket);
	Search.Socket = nullptr;
}

TArray<uint8> FFlockDirectAddresses::StunRequest(const TArray<uint8>& Transaction)
{
	TArray<uint8> Request;
	Request.SetNumZeroed(20);
	Request[0] = static_cast<uint8>(StunBindingRequest >> 8);
	Request[1] = static_cast<uint8>(StunBindingRequest);
	Request[4] = static_cast<uint8>(StunMagicCookie >> 24);
	Request[5] = static_cast<uint8>(StunMagicCookie >> 16);
	Request[6] = static_cast<uint8>(StunMagicCookie >> 8);
	Request[7] = static_cast<uint8>(StunMagicCookie);
	for (int32 Index = 0; Index < 12 && Index < Transaction.Num(); ++Index)
	{
		Request[8 + Index] = Transaction[Index];
	}
	return Request;
}

bool FFlockDirectAddresses::PublicEndpointInStunAnswer(const uint8* Answer, int32 Count, const TArray<uint8>& Transaction, FString& OutAddress,
	int32& OutPort)
{
	OutAddress.Reset();
	OutPort = 0;
	if (Answer == nullptr || Count < 20 || Transaction.Num() != 12 || ReadUInt16(Answer, 0) != StunBindingSuccess
		|| ReadUInt32(Answer, 4) != StunMagicCookie)
	{
		return false;
	}
	for (int32 Index = 0; Index < 12; ++Index)
	{
		if (Answer[8 + Index] != Transaction[Index])
		{
			return false;
		}
	}
	const int32 End = FMath::Min(Count, 20 + static_cast<int32>(ReadUInt16(Answer, 2)));
	FString Plain;
	int32 PlainPort = 0;
	int32 Position = 20;
	while (Position + 4 <= End)
	{
		const uint16 Kind = ReadUInt16(Answer, Position);
		const int32 Length = ReadUInt16(Answer, Position + 2);
		const int32 Value = Position + 4;
		if (Value + Length > End)
		{
			break;
		}
		// Family 0x01 is IPv4: one reserved byte, the family, the port, then four address bytes.
		if (Length >= 8 && Answer[Value + 1] == 0x01)
		{
			if (Kind == StunXorMappedAddress)
			{
				const uint32 Masked = ReadUInt32(Answer, Value + 4) ^ StunMagicCookie;
				OutPort = ReadUInt16(Answer, Value + 2) ^ static_cast<int32>(StunMagicCookie >> 16);
				OutAddress = FString::Printf(TEXT("%u.%u.%u.%u"), (Masked >> 24) & 0xFF, (Masked >> 16) & 0xFF, (Masked >> 8) & 0xFF, Masked & 0xFF);
				return true;
			}
			if (Kind == StunMappedAddress && Plain.IsEmpty())
			{
				PlainPort = ReadUInt16(Answer, Value + 2);
				Plain = FString::Printf(TEXT("%u.%u.%u.%u"), Answer[Value + 4], Answer[Value + 5], Answer[Value + 6], Answer[Value + 7]);
			}
		}
		Position = Value + Length + ((4 - Length % 4) % 4);
	}
	OutAddress = Plain;
	OutPort = PlainPort;
	return !Plain.IsEmpty();
}

FString FFlockDirectAddresses::PublicAddressInStunAnswer(const TArray<uint8>& Answer, const TArray<uint8>& Transaction)
{
	FString Address;
	int32 Port = 0;
	PublicEndpointInStunAnswer(Answer.GetData(), Answer.Num(), Transaction, Address, Port);
	return Address;
}

FString FFlockDirectAddresses::LanAddressToward(const FString& Ipv4)
{
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (Sockets == nullptr || Ipv4Value(Ipv4).IsEmpty())
	{
		return FString();
	}
	FSocket* Socket = Sockets->CreateSocket(NAME_DGram, TEXT("Flock LAN address"), FNetworkProtocolTypes::IPv4);
	if (Socket == nullptr)
	{
		return FString();
	}
	FString Found;
	const TSharedRef<FInternetAddr> Target = Sockets->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
	bool bValid = false;
	Target->SetIp(*Ipv4, bValid);
	Target->SetPort(DefaultStunPort);
	if (bValid && Socket->Connect(*Target))
	{
		const TSharedRef<FInternetAddr> Local = Sockets->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
		Socket->GetAddress(*Local);
		Found = Ipv4Value(Local->ToString(false));
		if (Found.Equals(TEXT("0.0.0.0"), ESearchCase::CaseSensitive))
		{
			Found.Reset();
		}
	}
	Socket->Close();
	Sockets->DestroySocket(Socket);
	return Found;
}

FString FFlockDirectAddresses::Ipv4Value(const FString& Text)
{
	TArray<FString> Parts;
	Text.ParseIntoArray(Parts, TEXT("."), /*bCullEmpty*/ false);
	if (Parts.Num() != 4)
	{
		return FString();
	}
	TArray<FString> Numbers;
	for (const FString& Part : Parts)
	{
		if (Part.IsEmpty() || Part.Len() > 3)
		{
			return FString();
		}
		for (const TCHAR Character : Part)
		{
			if (!FChar::IsDigit(Character))
			{
				return FString();
			}
		}
		const int32 Number = FCString::Atoi(*Part);
		if (Number > 255)
		{
			return FString();
		}
		Numbers.Add(FString::FromInt(Number));
	}
	return FString::Join(Numbers, TEXT("."));
}

bool FFlockDirectAddresses::ParseStunUrl(const FString& Url, FString& OutHost, int32& OutPort)
{
	if (!Url.StartsWith(TEXT("stun:"), ESearchCase::IgnoreCase))
	{
		return false;
	}
	FString Rest = Url.Mid(5);
	int32 Query = INDEX_NONE;
	if (Rest.FindChar(TEXT('?'), Query))
	{
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
		OutPort = DefaultStunPort;
		return true;
	}
	const FString PortText = Rest.Mid(Colon + 1);
	if (Colon == 0 || PortText.IsEmpty() || PortText.Len() > 5)
	{
		return false;
	}
	for (const TCHAR Character : PortText)
	{
		if (!FChar::IsDigit(Character))
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

bool FFlockDirectAddresses::ReadPublished(const FFlockMultiplayerSessionConnection& Connection, FFlockPublishedDirectAddresses& OutAddresses)
{
	OutAddresses = FFlockPublishedDirectAddresses();
	if (!Connection.Mode.Equals(FlockMultiplayerConnectionModes::Direct, ESearchCase::CaseSensitive))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Values;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Connection.Values.ToJsonString()), Values) || !Values.IsValid())
	{
		return false;
	}
	OutAddresses.Port = PortOf(FieldSpelledExactly(*Values, FlockDirectConnectionValues::Port));
	if (OutAddresses.Port == 0)
	{
		return false;
	}
	OutAddresses.Address = Ipv4Value(TextOf(FieldSpelledExactly(*Values, FlockDirectConnectionValues::Address)));
	OutAddresses.Lan = Ipv4Value(TextOf(FieldSpelledExactly(*Values, FlockDirectConnectionValues::LanAddress)));
	OutAddresses.Public = Ipv4Value(TextOf(FieldSpelledExactly(*Values, FlockDirectConnectionValues::PublicAddress)));
	return true;
}

FFlockCommandData FFlockDirectAddresses::ValuesToPublish(const FFlockDeviceAddresses& Found, int32 Port)
{
	FFlockCommandData Values;
	if (Found.Lan.IsEmpty() && Found.Public.IsEmpty())
	{
		return Values;
	}
	Values.Set(FlockDirectConnectionValues::Address, Found.Public.IsEmpty() ? Found.Lan : Found.Public);
	Values.Set(FlockDirectConnectionValues::Port, Port);
	if (!Found.Lan.IsEmpty())
	{
		Values.Set(FlockDirectConnectionValues::LanAddress, Found.Lan);
	}
	if (!Found.Public.IsEmpty())
	{
		Values.Set(FlockDirectConnectionValues::PublicAddress, Found.Public);
	}
	return Values;
}
