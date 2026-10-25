// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include <atomic>

class FFlockRelayStream;
class FSocket;

/**
 * The way a relay connection sends to and reads from its relay server: a UDP socket (dual-stack where the platform allows)
 * connected to it, or a TCP or TLS stream (FFlockRelayStream) for a network that blocks UDP to the relay. Either hands over
 * one whole message (a STUN message or a channel frame) at a time, and a send never holds the sender. Made empty and given its
 * socket or stream on the relay's thread once one is found; until then, and once closed, sends are refused. Sends from any
 * thread; reads and the close on the relay's thread.
 */
class FFlockRelaySocket
{
public:
	/** bInOverAStream for a relay reached over TCP or TLS, known before its stream is opened. */
	explicit FFlockRelaySocket(bool bInOverAStream = false) : bOverAStream(bInOverAStream) {}
	~FFlockRelaySocket();

	FFlockRelaySocket(const FFlockRelaySocket&) = delete;
	FFlockRelaySocket& operator=(const FFlockRelaySocket&) = delete;

	/**
	 * A socket connected to the server that the server answered: a binding request goes out from SocketsAtOnce sockets at once,
	 * again at half the round's wait, round after round, and the first one answered is kept. Some relay servers answer only some
	 * of the ports a device sends from (Cloudflare's, measured from this engine's sockets in 2026-10: 6 to 8 of 8, and 6 to 8 of
	 * 20 a week before). The server's address is IPv4, or IPv6 for a way in over IPv6. Null when none was answered, or ShouldStop
	 * said so; it blocks, so it runs on the relay's thread.
	 */
	static FSocket* FindOneTheServerAnswers(const FString& ServerAddress, int32 Port, int32 SocketsAtOnce, int32 Rounds, double RoundWaitSeconds,
		TFunctionRef<bool()> ShouldStop);

	/** Takes a UDP socket connected to the relay server; non-blocking, so a full send buffer drops a packet rather than holding a thread. */
	void Attach(FSocket* Connected);

	/** Takes a TCP or TLS stream to the relay server. */
	void AttachStream(const TSharedRef<FFlockRelayStream, ESPMode::ThreadSafe>& Opened);

	/** True over UDP, where a request or its answer may be lost and is sent again; a stream delivers it or ends. */
	bool LosesPackets() const { return !bOverAStream; }

	/** True on a stream, where each channel frame is padded to 4 bytes, as the standard asks of TCP and TLS. */
	bool PadsChannelFrames() const { return bOverAStream; }

	/** True once the relay closed a stream or the stream broke; UDP never ends. The relay's thread. */
	bool HasTheConnectionEnded() const;

	/** Why the stream ended, once HasTheConnectionEnded says so. The relay's thread. */
	FString WhyTheConnectionEnded() const;

	/** Sends one whole message, from any thread; false when there is no socket yet, it was closed, or it refused the message (a full buffer), which the answer wait judges. */
	bool TrySend(const uint8* Bytes, int32 Count);

	/** Waits up to Seconds for something to read; true when something is there. The relay's thread alone. */
	bool WaitToRead(double Seconds);

	/** Reads one whole message into Into; false when nothing is waiting or the read failed (an earlier send met a closed port). The relay's thread alone. */
	bool Receive(uint8* Into, int32 Size, int32& OutRead);

	/** The port this socket sends from, which the relay sees; 0 before a socket is attached. */
	int32 GetLocalPort() const { return LocalPort.load(); }

	/** The socket's protocol (IPv6 for a dual-stack one), NAME_None before one is attached. */
	FName GetProtocolForTesting() const;

	/** Closes the socket or stream; later sends are refused. */
	void Close();

private:
	const bool bOverAStream;
	/** Guards the socket and the stream against a close while another thread sends. */
	mutable FCriticalSection Lock;
	FSocket* Socket = nullptr;
	TSharedPtr<FFlockRelayStream, ESPMode::ThreadSafe> Stream;
	bool bClosed = false;
	FName Protocol;
	std::atomic<int32> LocalPort{ 0 };
};
