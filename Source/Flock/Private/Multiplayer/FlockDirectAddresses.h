// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockLogger.h"
#include "Http/FlockHttpAdapter.h"
#include "Http/FlockResult.h"
#include "Models/FlockCommandModels.h"
#include "Models/FlockMultiplayerModels.h"
#include "Multiplayer/FlockNameLookup.h"

class FFlockMultiplayerSessionRequests;
class FSocket;

/** This device's addresses for a direct connection, each empty when it cannot be found. */
struct FFlockDeviceAddresses
{
	FString Lan;
	FString Public;
};

/** What a host published in a direct connection, read the way players use it: dotted IPv4 addresses and a whole port, or nothing. */
struct FFlockPublishedDirectAddresses
{
	FString Address;
	FString Lan;
	FString Public;
	int32 Port = 0;
};

/**
 * This device's addresses for a direct connection: its LAN address, from the route toward the internet, and its public address,
 * from one request to a STUN server Flock lists. The STUN servers are read once a launch, asked with no session; a read that
 * failed is not kept. Name lookups run on the engine's own threads; everything else runs on the game thread, once a frame.
 */
class FFlockDirectAddresses : public TSharedFromThis<FFlockDirectAddresses>
{
public:
	/** A binding request answers in about 100 ms; a server silent this long is taken as out of reach. */
	static constexpr double DefaultStunWaitSeconds = 2.0;
	static constexpr int32 DefaultStunPort = 3478;

	/** Stands in for the system's name lookup: hands Answer the host's first IPv4 address (empty for none), on any thread, or never. */
	using FNameLookUp = FFlockNameLookup::FLookUp;

	/** @param InCurrentSignInNumber The sign-in now current: a search whose own one ended while the servers were read ends Cancelled. */
	FFlockDirectAddresses(const TSharedRef<FFlockMultiplayerSessionRequests>& InRequests, TFunction<double()> InNow,
		TFunction<int32()> InCurrentSignInNumber, const TSharedRef<IFlockLogger>& InLogger);
	~FFlockDirectAddresses();

	/**
	 * The LAN address and the public address, either empty when it cannot be found; acts for SignInNumber the first time it reads
	 * the STUN servers, and fails Cancelled once that sign-in has ended then. Cancelling the handle answers Cancelled at once.
	 */
	FFlockRequestHandle Find(int32 SignInNumber, TFunction<void(TFlockResult<FFlockDeviceAddresses>)> OnFound);

	/** Moves every search on: names looked up, STUN answers arrived, waits run out. Run once a frame. */
	void Tick();

	/** Flock is shutting down: every search ends Cancelled and its socket closes. */
	void StopForShutdown();

	void SetStunWaitForTesting(double Seconds) { StunWaitSeconds = Seconds; }
	void SetNameLookUpForTesting(FNameLookUp LookUp) { NameLookUpForTesting = MoveTemp(LookUp); }
	/** The protocol of the socket the last STUN request went out from. */
	FName GetAskedFromForTesting() const { return AskedFrom; }
	int32 GetSearchCountForTesting() const { return Searches.Num(); }

	/** The address of a binding success answering Transaction, read from its first Count bytes; empty for anything else. */
	static bool PublicEndpointInStunAnswer(const uint8* Answer, int32 Count, const TArray<uint8>& Transaction, FString& OutAddress, int32& OutPort);
	static FString PublicAddressInStunAnswer(const TArray<uint8>& Answer, const TArray<uint8>& Transaction);

	/** A binding request with this transaction (12 bytes), as sent to a STUN server. */
	static TArray<uint8> StunRequest(const TArray<uint8>& Transaction);

	/** The address this device sends from toward Ipv4: a UDP socket picks its route on connect and sends nothing. Empty when there is none. */
	static FString LanAddressToward(const FString& Ipv4);

	/** Text that is a dotted IPv4 address, as written by machines: four numbers 0 to 255 and nothing else (no spaces, no name). Empty otherwise. */
	static FString Ipv4Value(const FString& Text);

	/** A "stun:host:port" entry's host and port (3478 when none); false for TLS ("stuns:"), an IPv6 host or a port out of range. */
	static bool ParseStunUrl(const FString& Url, FString& OutHost, int32& OutPort);

	/**
	 * Reads a direct connection, or the direct addresses a relay connection carries: its addresses that are dotted IPv4 and its
	 * port, a whole number 1 to 65535 sent as a number or as text ("7777" is a port, 7777.5 is not). False for another mode (letter
	 * case counts) or a descriptor with no such port.
	 */
	static bool ReadPublished(const FFlockMultiplayerSessionConnection& Connection, FFlockPublishedDirectAddresses& OutAddresses);

	/** What a host publishes: the public address as "address" when known, the LAN one otherwise; an unknown address is left out. */
	static FFlockCommandData ValuesToPublish(const FFlockDeviceAddresses& Found, int32 Port);

private:
	struct FStunServer
	{
		FString Host;
		int32 Port = DefaultStunPort;
	};

	enum class EStep : uint8 { ReadingServers, LookingUp, Asking, Done };

	struct FSearch
	{
		int32 SignInNumber = 0;
		TFunction<void(TFlockResult<FFlockDeviceAddresses>)> OnFound;
		TSharedRef<FFlockCancelToken> Token = MakeShared<FFlockCancelToken>();
		FFlockRequestHandle ServersRead;
		EStep Step = EStep::ReadingServers;
		TArray<FStunServer> Servers;
		int32 Index = -1;
		FString FirstServerAddress;
		FString ServerAddress;
		FString Public;
		/** The name being looked up on another thread; read on the game thread once it says it is done. */
		TSharedPtr<FFlockNameLookup, ESPMode::ThreadSafe> LookUp;
		FSocket* Socket = nullptr;
		TArray<uint8> Transaction;
		double Deadline = 0.0;
	};
	using FSearchRef = TSharedRef<FSearch>;

	void StartWithServers(const FSearchRef& Search, const TArray<FStunServer>& Servers);
	void NextServer(const FSearchRef& Search);
	void Ask(const FSearchRef& Search);
	/** Sends one binding request; a dual-stack socket that cannot send to the server is replaced by a plain IPv4 one, once. */
	bool SendRequest(const FSearchRef& Search);
	void Advance(const FSearchRef& Search);
	void Finish(const FSearchRef& Search);
	/** Ends a search without an answer of its own: cancelled, or Flock shutting down. */
	void EndWith(const FSearchRef& Search, const FFlockError& Error);
	static void CloseSocket(FSearch& Search);
	static TArray<FStunServer> ServersIn(const FFlockStunServerList& List);

	TSharedRef<FFlockMultiplayerSessionRequests> Requests;
	TFunction<double()> Now;
	TFunction<int32()> CurrentSignInNumber;
	TSharedRef<IFlockLogger> Logger;
	TArray<FSearchRef> Searches;
	/** The STUN servers Flock lists, read once a launch; unset until a read works. */
	TOptional<TArray<FStunServer>> StunServers;
	double StunWaitSeconds = DefaultStunWaitSeconds;
	bool bToldPublicAddressUnknown = false;
	FName AskedFrom;
	FNameLookUp NameLookUpForTesting;
};
