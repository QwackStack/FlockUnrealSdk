// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Multiplayer/Relay/FlockRelayLogin.h"
#include "Multiplayer/Relay/FlockRelayWire.h"
#include <atomic>

class FFlockRelaySocket;
struct FFlockRelayTiming;

/** What a request is done with: 0 for a success, the relay's code for a refusal, or FFlockRelayRequests::NoAnswer; the answer when one came. Runs on the relay's thread. */
using FFlockRelayRequestDone = TFunction<void(int32 Code, const FString& Words, const FFlockRelayMessage* Answer)>;

/** A request waiting for its answer: what it asks, the bytes last sent, and what to do with the answer on the relay's thread. */
struct FFlockRelayRequest
{
	FFlockRelayRequest(uint16 InMethod, FFlockRelayRequestDone InDone, TArray<FFlockRelayAttribute> InAttributes)
		: Method(InMethod), Done(MoveTemp(InDone)), Attributes(MoveTemp(InAttributes))
	{
	}

	uint16 Method = 0;
	FFlockRelayRequestDone Done;
	TArray<FFlockRelayAttribute> Attributes;
	TArray<uint8> SignedWith;
	TArray<uint8> Transaction;
	TArray<uint8> Bytes;
	double FirstSentAt = 0.0;
	double NextSendAt = 0.0;
	int32 Sends = 0;
	int32 NoncesTaken = 0;
};
using FFlockRelayRequestRef = TSharedRef<FFlockRelayRequest, ESPMode::ThreadSafe>;

/**
 * The relay's requests and their answers: signed once the relay names a realm and nonce, sent again while unanswered, given
 * up on after the answer wait, and the release when the connection stops. The relay's thread, but for the release, which
 * the thread that stops the connection sends.
 */
class FFlockRelayRequests
{
public:
	/** The code a request is done with when the relay never answered it. */
	static constexpr int32 NoAnswer = -1;

	FFlockRelayRequests(const FFlockRelayLogin& InLogin, FFlockRelaySocket& InSocket, const FFlockRelayTiming& InTiming);

	/** Realms and nonces the relay handed over to sign with: its login challenge, then each stale nonce answered. */
	int32 GetNoncesTaken() const { return NoncesTaken.load(); }

	/** True once a signed reservation went out, so there may be an address to give back. */
	bool WasReservationAsked() const { return bReservationAsked.load(); }

	/** True while the release went out and its answer has not come. */
	bool IsReleaseWaitingForAnswer() const;

	/** Sends a request with a new transaction: first, and again once a realm or nonce is taken. */
	void Send(const FFlockRelayRequestRef& Request);

	/**
	 * Over UDP an answer may be lost on the way: the same bytes again, under the same transaction, so the relay knows it for a repeat
	 * (a stream delivers it or ends, so it goes once); given up on after the answer wait.
	 */
	void SendAgainOrGiveUp(double Now);

	/** Asks the relay (a binding request) every AskTheRelayEverySeconds, whatever else goes; it also keeps the way open through a router or Docker's port forwarding. */
	void AskTheRelay(double Now);

	/** True when the relay, which answered the asks, has answered nothing for RelayLostWhenSilentSeconds: this device no longer reaches it. */
	bool HasStoppedAnswering(double Now) const;

	/** Takes one of the relay's own messages: the release's answer, or an answer to a request waiting. */
	void Take(const uint8* Packet, int32 Length);

	/** Gives the address back, sent from the calling thread so it goes out even while the game quits; asked only once a signed reservation went out. */
	void Release();

	/**
	 * The connection stopped: from now on no request is sent but the release (and its one resend on a stale nonce). A request
	 * already past the sending check goes out first, so the release never reaches the relay ahead of it. Any thread.
	 */
	void StopSending();

	/** Run on the relay's thread just before a signed reservation goes out, so a test can stop the connection in between. */
	static TFunction<void()> BeforeASignedReservationGoesForTesting;

	/** Run on the relay's thread just after a signed reservation went out, so a test can stop the connection in between. */
	static TFunction<void()> AfterASignedReservationWentForTesting;

	/** Run on the relay's thread once the sending check let a signed reservation through, just before its bytes go. */
	static TFunction<void()> WhileASignedReservationGoesForTesting;

	/** Run just after a release goes out: on the thread stopping the connection, so a test can hold it there, or on the relay's thread when it sends the release again. */
	static TFunction<void()> AfterTheReleaseGoesForTesting;

private:
	void Answer(const FFlockRelayRequestRef& Request, const FFlockRelayMessage& Message, uint16 AnswerClass);
	/** The relay's code for a refusal; one with no code is still a refusal. */
	static int32 RefusalCodeOf(const FFlockRelayMessage& Message, FString& OutWords);
	/** Signs every later request with the realm and nonce an answer carries; false when it carries none. */
	bool TakeNonce(const FFlockRelayMessage& Message);
	/** The release's answer: a stale nonce is answered once with the new one, and anything else lets the socket close. */
	void ReleaseAnswered(const FFlockRelayMessage& Message, uint16 AnswerClass);
	/** A renewal asking for 0 s gives the address back at once; its transaction is waited on, or none when it could not go. */
	void SendRelease();
	void SendBytes(const TArray<uint8>& Bytes, bool bSignedReservation = false);

	FFlockRelayLogin Login;
	FFlockRelaySocket& Socket;
	const FFlockRelayTiming& Timing;

	/** What requests are signed with; the thread that stops the connection reads it too. */
	mutable FCriticalSection SigningLock;
	FString Realm;
	FString Nonce;
	TArray<uint8> Key;
	std::atomic<int32> NoncesTaken{ 0 };
	std::atomic<bool> bReservationAsked{ false };
	/**
	 * Held around "is the sending stopped, then send", and by the stop around stopping it: a request that passed the check goes out
	 * before the stop goes on, so the release never reaches the relay ahead of a reservation already on its way, and a reservation
	 * the stop came before never goes.
	 */
	FCriticalSection SendLock;
	bool bSendingStopped = false;

	/** The relay's thread's own. */
	TArray<FFlockRelayRequestRef> Waiting;
	/** The latest ask, when it went, and when the relay last answered anything; the relay's thread's own. */
	TArray<uint8> AskTransaction;
	double LastAskAt = 0.0;
	double LastAnsweredAt = 0.0;
	/** A relay may answer the reservation and never a binding request (from a socket it did not answer a binding on): its silence says nothing. */
	bool bAnswersAsks = false;

	/** The release on its way: set by the thread that stops the connection, answered on the relay's thread. */
	mutable FCriticalSection ReleaseLock;
	TArray<uint8> ReleaseTransaction;
	bool bReleaseSentAgain = false;
};
