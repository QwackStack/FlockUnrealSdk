// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/Relay/FlockRelayRequests.h"
#include "HAL/PlatformTime.h"
#include "Multiplayer/Relay/FlockRelayConnection.h"
#include "Multiplayer/Relay/FlockRelaySocket.h"

namespace
{
	/** A refusal that names no code is still a refusal, never the success 0 stands for. */
	constexpr int32 RefusalWithNoCode = -2;
	/** A request is sent again 0.5 s, 1.5 s and 3.5 s after it first went, then given up on once the answer wait is over. */
	constexpr int32 MostSends = 4;
	constexpr double FirstResendSeconds = 0.5;
	/** The most stale nonces (and login challenges) one request answers. */
	constexpr int32 MostNoncesARequestTakes = 2;
}

TFunction<void()> FFlockRelayRequests::BeforeASignedReservationGoesForTesting;
TFunction<void()> FFlockRelayRequests::AfterASignedReservationWentForTesting;
TFunction<void()> FFlockRelayRequests::WhileASignedReservationGoesForTesting;
TFunction<void()> FFlockRelayRequests::AfterTheReleaseGoesForTesting;

FFlockRelayRequests::FFlockRelayRequests(const FFlockRelayLogin& InLogin, FFlockRelaySocket& InSocket, const FFlockRelayTiming& InTiming)
	: Login(InLogin)
	, Socket(InSocket)
	, Timing(InTiming)
{
}

bool FFlockRelayRequests::IsReleaseWaitingForAnswer() const
{
	FScopeLock Locked(&ReleaseLock);
	return ReleaseTransaction.Num() > 0;
}

void FFlockRelayRequests::Send(const FFlockRelayRequestRef& Request)
{
	TArray<FFlockRelayAttribute> Attributes = Request->Attributes;
	TArray<uint8> SignWith;
	{
		FScopeLock Locked(&SigningLock);
		if (!Nonce.IsEmpty())
		{
			Attributes.Emplace(FFlockRelayWire::UsernameAttribute, FFlockRelayWire::Utf8(Login.Username));
			Attributes.Emplace(FFlockRelayWire::RealmAttribute, FFlockRelayWire::Utf8(Realm));
			Attributes.Emplace(FFlockRelayWire::NonceAttribute, FFlockRelayWire::Utf8(Nonce));
			SignWith = Key;
		}
	}
	Request->SignedWith = SignWith;
	Request->Transaction = FFlockRelayWire::NewTransaction();
	Request->Bytes = FFlockRelayWire::Write(Request->Method | FFlockRelayWire::RequestClass, Request->Transaction, Attributes, SignWith);
	if (SignWith.Num() > 0 && Request->Method == FFlockRelayWire::AllocateMethod)
	{
		if (const TFunction<void()> Hook = BeforeASignedReservationGoesForTesting)
		{
			Hook();
		}
		// Marked before it goes: a stop decided meanwhile either releases it or keeps it from going.
		bReservationAsked = true;
	}
	const double Now = FPlatformTime::Seconds();
	Request->FirstSentAt = Now;
	Request->Sends = 1;
	Request->NextSendAt = Now + FirstResendSeconds;
	SendBytes(Request->Bytes, SignWith.Num() > 0 && Request->Method == FFlockRelayWire::AllocateMethod);
	if (SignWith.Num() > 0 && Request->Method == FFlockRelayWire::AllocateMethod)
	{
		if (const TFunction<void()> Hook = AfterASignedReservationWentForTesting)
		{
			Hook();
		}
	}
	Waiting.AddUnique(Request);
}

void FFlockRelayRequests::SendAgainOrGiveUp(double Now)
{
	for (int32 Index = Waiting.Num() - 1; Index >= 0 && Index < Waiting.Num(); --Index)
	{
		const FFlockRelayRequestRef Request = Waiting[Index];
		if (Now - Request->FirstSentAt >= Timing.AnswerWaitSeconds)
		{
			Waiting.RemoveAt(Index);
			Request->Done(NoAnswer, TEXT("no answer"), nullptr);
		}
		else if (Request->Sends < MostSends && Now >= Request->NextSendAt)
		{
			SendBytes(Request->Bytes);
			Request->NextSendAt = Now + FirstResendSeconds * static_cast<double>(1 << Request->Sends);
			++Request->Sends;
		}
	}
}

void FFlockRelayRequests::KeepAlive(double Now)
{
	if (Now - Socket.GetLastSentAt() < Timing.KeepAliveAfterSeconds)
	{
		return;
	}
	SendBytes(FFlockRelayWire::Write(FFlockRelayWire::BindingMethod | FFlockRelayWire::RequestClass, FFlockRelayWire::NewTransaction(),
		TArray<FFlockRelayAttribute>(), TArray<uint8>()));
}

void FFlockRelayRequests::Take(const uint8* Packet, int32 Length)
{
	FFlockRelayMessage Message;
	if (!FFlockRelayWire::Read(Packet, Length, Message))
	{
		return;
	}
	const uint16 AnswerClass = FFlockRelayWire::ClassOf(Message.Type);
	if (AnswerClass != FFlockRelayWire::SuccessClass && AnswerClass != FFlockRelayWire::ErrorClass)
	{
		return;
	}
	bool bAnswersTheRelease = false;
	{
		FScopeLock Locked(&ReleaseLock);
		bAnswersTheRelease = ReleaseTransaction.Num() > 0 && ReleaseTransaction == Message.Transaction;
	}
	if (bAnswersTheRelease)
	{
		ReleaseAnswered(Message, AnswerClass);
		return;
	}
	const FFlockRelayRequestRef* Found = Waiting.FindByPredicate([&Message](const FFlockRelayRequestRef& Request)
	{
		return Request->Transaction == Message.Transaction;
	});
	if (Found == nullptr)
	{
		return;
	}
	const FFlockRelayRequestRef Request = *Found;
	// Only an answer the relay made is believed: a success to a signed request must be signed with the login, and so must any
	// answer that carries a signature.
	if (Request->SignedWith.Num() > 0 && (Message.IntegrityAt != INDEX_NONE || AnswerClass == FFlockRelayWire::SuccessClass)
		&& !FFlockRelayWire::SignatureHolds(Message, Request->SignedWith))
	{
		return;
	}
	Answer(Request, Message, AnswerClass);
}

void FFlockRelayRequests::Release()
{
	SendRelease();
}

void FFlockRelayRequests::Answer(const FFlockRelayRequestRef& Request, const FFlockRelayMessage& Message, uint16 AnswerClass)
{
	Waiting.Remove(Request);
	if (AnswerClass == FFlockRelayWire::SuccessClass)
	{
		Request->Done(0, FString(), &Message);
		return;
	}
	FString Words;
	const int32 Code = RefusalCodeOf(Message, Words);
	// The first answer to an unsigned request names the realm and nonce to sign with, and a stale nonce comes with a new one;
	// twice at most.
	const bool bChallenge = (Code == 401 && Request->SignedWith.Num() == 0) || Code == 438;
	if (bChallenge && Request->NoncesTaken < MostNoncesARequestTakes && TakeNonce(Message))
	{
		++Request->NoncesTaken;
		Send(Request);
		return;
	}
	Request->Done(Code, Words, &Message);
}

int32 FFlockRelayRequests::RefusalCodeOf(const FFlockRelayMessage& Message, FString& OutWords)
{
	const int32 Code = FFlockRelayWire::ErrorCodeOf(Message.Attributes.Find(FFlockRelayWire::ErrorCodeAttribute), OutWords);
	if (Code != 0)
	{
		return Code;
	}
	OutWords = TEXT("a refusal naming no code");
	return RefusalWithNoCode;
}

bool FFlockRelayRequests::TakeNonce(const FFlockRelayMessage& Message)
{
	FString TakenRealm;
	FString TakenNonce;
	if (!Message.TryGetText(FFlockRelayWire::RealmAttribute, TakenRealm) || !Message.TryGetText(FFlockRelayWire::NonceAttribute, TakenNonce))
	{
		return false;
	}
	{
		FScopeLock Locked(&SigningLock);
		if (!TakenRealm.Equals(Realm, ESearchCase::CaseSensitive) || Key.Num() == 0)
		{
			Key = FFlockRelayWire::LoginKey(Login.Username, TakenRealm, Login.Password);
		}
		Realm = TakenRealm;
		Nonce = TakenNonce;
	}
	++NoncesTaken;
	return true;
}

void FFlockRelayRequests::ReleaseAnswered(const FFlockRelayMessage& Message, uint16 AnswerClass)
{
	FString Words;
	if (AnswerClass == FFlockRelayWire::ErrorClass && !bReleaseSentAgain && RefusalCodeOf(Message, Words) == 438 && TakeNonce(Message))
	{
		bReleaseSentAgain = true;
		SendRelease();
		return;
	}
	FScopeLock Locked(&ReleaseLock);
	ReleaseTransaction.Reset();
}

void FFlockRelayRequests::SendRelease()
{
	TArray<FFlockRelayAttribute> Attributes;
	Attributes.Emplace(FFlockRelayWire::LifetimeAttribute, FFlockRelayWire::UInt32Value(0));
	TArray<uint8> SignWith;
	{
		// Asked only once a signed reservation went out, so the realm and nonce are known.
		FScopeLock Locked(&SigningLock);
		Attributes.Emplace(FFlockRelayWire::UsernameAttribute, FFlockRelayWire::Utf8(Login.Username));
		Attributes.Emplace(FFlockRelayWire::RealmAttribute, FFlockRelayWire::Utf8(Realm));
		Attributes.Emplace(FFlockRelayWire::NonceAttribute, FFlockRelayWire::Utf8(Nonce));
		SignWith = Key;
	}
	const TArray<uint8> Transaction = FFlockRelayWire::NewTransaction();
	const TArray<uint8> Release = FFlockRelayWire::Write(FFlockRelayWire::RefreshMethod | FFlockRelayWire::RequestClass, Transaction, Attributes, SignWith);
	{
		// Known before it goes, so an answer that comes back at once (a stale nonce among them) is already the release's.
		FScopeLock Locked(&ReleaseLock);
		ReleaseTransaction = Transaction;
	}
	if (!Socket.TrySend(Release.GetData(), Release.Num()))
	{
		// Best effort: an address not given back lapses on the relay within its lifetime.
		FScopeLock Locked(&ReleaseLock);
		ReleaseTransaction.Reset();
		return;
	}
	if (const TFunction<void()> Hook = AfterTheReleaseGoesForTesting)
	{
		Hook();
	}
}

void FFlockRelayRequests::StopSending()
{
	FScopeLock Locked(&SendLock);
	bSendingStopped = true;
}

void FFlockRelayRequests::SendBytes(const TArray<uint8>& Bytes, bool bSignedReservation)
{
	// The one owner of "no request goes after the stop": a renewal, a resend or a keep-alive the same turn reaches is dropped.
	FScopeLock Locked(&SendLock);
	if (bSendingStopped)
	{
		return;
	}
	if (bSignedReservation)
	{
		if (const TFunction<void()> Hook = WhileASignedReservationGoesForTesting)
		{
			Hook();
		}
	}
	Socket.TrySend(Bytes.GetData(), Bytes.Num());
}
