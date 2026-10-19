// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include <atomic>

class FSocket;

/**
 * The UDP socket to one relay server (dual-stack where the platform allows): connected to it, and never holding a sender.
 * Made empty and given its socket on the relay's thread once one is found; until then, and once closed, sends are refused.
 * Sends from any thread; reads and the close on the relay's thread.
 */
class FFlockRelaySocket
{
public:
	FFlockRelaySocket() = default;
	~FFlockRelaySocket();

	FFlockRelaySocket(const FFlockRelaySocket&) = delete;
	FFlockRelaySocket& operator=(const FFlockRelaySocket&) = delete;

	/**
	 * A socket connected to the server that the server answered: a binding request goes out from SocketsAtOnce sockets at once,
	 * again at half the round's wait, round after round, and the first one answered is kept. Some relay servers answer only some
	 * of the ports a device sends from (Cloudflare's, measured from this engine's sockets in 2026-10: 6 to 8 of 8, and 6 to 8 of
	 * 20 a week before). Null when none was answered, or ShouldStop said so; it blocks, so it runs on the relay's thread.
	 */
	static FSocket* FindOneTheServerAnswers(const FString& ServerIpv4, int32 Port, int32 SocketsAtOnce, int32 Rounds, double RoundWaitSeconds,
		TFunctionRef<bool()> ShouldStop);

	/** Takes a socket connected to the relay server; non-blocking, so a full send buffer drops a packet rather than holding a thread. */
	void Attach(FSocket* Connected);

	/** Sends one packet, from any thread; false when there is no socket yet, it was closed, or it refused the packet, which the answer wait judges. */
	bool TrySend(const uint8* Bytes, int32 Count);

	/** Waits up to Seconds for something to read; true when something is there. The relay's thread alone. */
	bool WaitToRead(double Seconds);

	/** Reads one packet into Into; false when nothing is waiting or the read failed (an earlier send met a closed port). The relay's thread alone. */
	bool Receive(uint8* Into, int32 Size, int32& OutRead);

	/** The port this socket sends from, which the relay sees; 0 before a socket is attached. */
	int32 GetLocalPort() const { return LocalPort.load(); }

	/** The socket's protocol (IPv6 for a dual-stack one), NAME_None before one is attached. */
	FName GetProtocolForTesting() const;

	/** Closes the socket; later sends are refused. */
	void Close();

private:
	/** Guards the socket against a close while another thread sends. */
	mutable FCriticalSection Lock;
	FSocket* Socket = nullptr;
	bool bClosed = false;
	FName Protocol;
	std::atomic<int32> LocalPort{ 0 };
};
