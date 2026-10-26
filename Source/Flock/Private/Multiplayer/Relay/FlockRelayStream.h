// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Multiplayer/Relay/FlockRelayThreads.h"
#include <atomic>

class FEvent;
class FFlockRelayTls;
class FSocket;

/**
 * A TCP or TLS stream to one relay server, for a network that blocks UDP to it. Connected, and over TLS checked, on the relay's
 * thread, which also reads it: what arrives is kept and whole messages are cut from it (a STUN message by its length, a channel
 * frame and its padding to 4 bytes). A sending thread of its own ("Flock relay sending") writes what was handed over, so a send
 * never waits on the network, and a full room drops a whole packet as a full UDP buffer does; a send copies into that room and
 * makes nothing. A stream the relay closes, or that breaks, says why once.
 */
class FFlockRelayStream : public IFlockRelayThreadWork, public TSharedFromThis<FFlockRelayStream, ESPMode::ThreadSafe>
{
public:
	/** Room for what arrived and is not read yet, and for what waits to be written: each holds well over a hundred full packets. */
	static constexpr int32 ArrivedRoom = 256 * 1024;
	static constexpr int32 SendingRoom = 256 * 1024;
	static constexpr const TCHAR* SendingThreadName = TEXT("Flock relay sending");

	/**
	 * Connects to Address:Port (the server's IPv4 or IPv6 address) within WaitSeconds and, over TLS, has the relay show within
	 * WaitSeconds more a certificate the platform trusts for Host, then starts the sending thread. Null with why in OutCause, the
	 * words after "The relay at <way>" (empty when ShouldStop ended it), and bOutNoThread when no sending thread could start. Blocks,
	 * so it runs on the relay's thread.
	 */
	static TSharedPtr<FFlockRelayStream, ESPMode::ThreadSafe> Open(const FString& Address, int32 Port, const FString& Host, bool bOverTls,
		double WaitSeconds, TFunctionRef<bool()> ShouldStop, FString& OutCause, bool& bOutNoThread);

	virtual ~FFlockRelayStream() override;

	FFlockRelayStream(const FFlockRelayStream&) = delete;
	FFlockRelayStream& operator=(const FFlockRelayStream&) = delete;

	/** Waits up to Seconds for a whole message; true when one is there. The relay's thread alone. */
	bool WaitToRead(double Seconds);

	/** Copies the next whole message into Into (room for Size bytes; a larger one is passed over); false when none is there. The relay's thread alone. */
	bool Receive(uint8* Into, int32 Size, int32& OutRead);

	/** Hands one whole message to the sending thread, from any thread; false when the room is full or the stream ended, so it was dropped. */
	bool TrySend(const uint8* Bytes, int32 Count);

	/** True once the relay closed the stream or it broke (never for a close of this side's own). Any thread. */
	bool HasTheConnectionEnded() const { return bConnectionEnded.load(); }

	/** Why the connection ended, once HasTheConnectionEnded says so. Any thread. */
	FString WhyTheConnectionEnded() const;

	int32 GetLocalPort() const { return LocalPort; }
	FName GetProtocol() const { return Protocol; }

	/** Ends the connection (the relay ends the reservation with it); later sends are refused. The relay's thread. */
	void Close();

	// ---- the sending thread ----

	virtual uint32 Run() override;
	virtual bool HasEnded() const override { return bSendingDone.load(); }
	virtual void EndNow() override;

	/** Every stream's sending thread sends nothing while this is set, as a slow network would hold it; what is handed over waits. */
	static std::atomic<bool> bHoldsSendsForTesting;

private:
	FFlockRelayStream(FSocket* InSocket, TUniquePtr<FFlockRelayTls> InTls);

	/** Moves what the network has into the room for what arrived, decrypting over TLS; never waits. The relay's thread. */
	void TakeFromTheNetwork();

	/** The length of the whole message at the front of what arrived: 0 while it has not all arrived, -1 when the stream holds neither kind. */
	int32 WholeMessageLength() const;

	/** Sends Bytes whole from the sending thread, waiting while the network's buffer is full; false once the stream ended or was closed. */
	bool SendWhole(const TArray<uint8>& Bytes);

	/** The first reason a connection ended is kept; a stream this side closed has not ended for the relay's reasons. */
	void End(const FString& Reason);

	bool IsStoppingToSend() const { return bClosed.load() || bQuitNow.load() || bConnectionEnded.load(); }

	FSocket* Socket = nullptr;
	int32 LocalPort = 0;
	FName Protocol;

	/** Held around every TLS call: the relay's thread decrypts what arrives, the sending thread encrypts what goes. */
	FCriticalSection TlsLock;
	TUniquePtr<FFlockRelayTls> Tls;

	/** The relay's thread's own: what arrived, unread, at [ArrivedFirst, ArrivedFirst + ArrivedCount). */
	TArray<uint8> Arrived;
	int32 ArrivedFirst = 0;
	int32 ArrivedCount = 0;
	TArray<uint8> ReadChunk;

	/** What waits to be written, a ring at [SendingFirst, SendingFirst + SendingCount), and the wake-up for the sending thread. */
	FCriticalSection SendingLock;
	TArray<uint8> Sending;
	int32 SendingFirst = 0;
	int32 SendingCount = 0;
	FEvent* WakeUp = nullptr;

	mutable FCriticalSection EndedLock;
	FString EndedBecause;
	std::atomic<bool> bConnectionEnded{ false };
	std::atomic<bool> bClosed{ false };
	std::atomic<bool> bQuitNow{ false };
	std::atomic<bool> bSendingDone{ false };
};
