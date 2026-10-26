// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Tests/Support/FlockFakeRelayTls.h"

#if WITH_AUTOMATION_TESTS

#include "Tests/Support/FlockFakeRelayCertificate.h"

#if FLOCK_RELAY_OVER_TLS
#if WITH_SSL
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#endif
THIRD_PARTY_INCLUDES_START
// OpenSSL names a type UI, as the engine names a namespace: its is renamed while its headers load.
#define UI UI_ST
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#undef UI
THIRD_PARTY_INCLUDES_END
#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#endif
#define FLOCK_FAKE_RELAY_TLS 1
#endif
#endif

#ifndef FLOCK_FAKE_RELAY_TLS
#define FLOCK_FAKE_RELAY_TLS 0
#endif

struct FFlockFakeRelayTlsState
{
#if FLOCK_FAKE_RELAY_TLS
	SSL_CTX* Context = nullptr;
	SSL* Ssl = nullptr;
	BIO* FromTheClient = nullptr;
	BIO* ForTheClient = nullptr;
	bool bHandshakeDone = false;

	~FFlockFakeRelayTlsState()
	{
		if (Ssl != nullptr)
		{
			SSL_free(Ssl);
		}
		if (Context != nullptr)
		{
			SSL_CTX_free(Context);
		}
	}
#endif
};

TUniquePtr<FFlockFakeRelayTls> FFlockFakeRelayTls::Start()
{
#if FLOCK_FAKE_RELAY_TLS
	TUniquePtr<FFlockFakeRelayTlsState> State = MakeUnique<FFlockFakeRelayTlsState>();
	State->Context = SSL_CTX_new(TLS_server_method());
	if (State->Context == nullptr)
	{
		return nullptr;
	}
	const FTCHARToUTF8 CertificateText(FlockFakeRelayCertificate::Certificate);
	const FTCHARToUTF8 KeyText(FlockFakeRelayCertificate::Key);
	BIO* CertificateMemory = BIO_new_mem_buf(CertificateText.Get(), CertificateText.Length());
	BIO* KeyMemory = BIO_new_mem_buf(KeyText.Get(), KeyText.Length());
	X509* Certificate = CertificateMemory != nullptr ? PEM_read_bio_X509(CertificateMemory, nullptr, nullptr, nullptr) : nullptr;
	EVP_PKEY* Key = KeyMemory != nullptr ? PEM_read_bio_PrivateKey(KeyMemory, nullptr, nullptr, nullptr) : nullptr;
	const bool bLoaded = Certificate != nullptr && Key != nullptr && SSL_CTX_use_certificate(State->Context, Certificate) == 1
		&& SSL_CTX_use_PrivateKey(State->Context, Key) == 1;
	X509_free(Certificate);
	EVP_PKEY_free(Key);
	BIO_free(CertificateMemory);
	BIO_free(KeyMemory);
	if (!bLoaded)
	{
		return nullptr;
	}
	State->Ssl = SSL_new(State->Context);
	State->FromTheClient = BIO_new(BIO_s_mem());
	State->ForTheClient = BIO_new(BIO_s_mem());
	BIO_set_mem_eof_return(State->FromTheClient, -1);
	BIO_set_mem_eof_return(State->ForTheClient, -1);
	SSL_set_bio(State->Ssl, State->FromTheClient, State->ForTheClient);
	SSL_set_accept_state(State->Ssl);
	return TUniquePtr<FFlockFakeRelayTls>(new FFlockFakeRelayTls(MoveTemp(State)));
#else
	return nullptr;
#endif
}

FFlockFakeRelayTls::FFlockFakeRelayTls(TUniquePtr<FFlockFakeRelayTlsState> InState)
	: State(MoveTemp(InState))
{
}

FFlockFakeRelayTls::~FFlockFakeRelayTls() = default;

bool FFlockFakeRelayTls::Arrived(const uint8* Bytes, int32 Count, TArray<uint8>& OutPlain)
{
#if FLOCK_FAKE_RELAY_TLS
	ERR_clear_error();
	if (Count > 0)
	{
		BIO_write(State->FromTheClient, Bytes, Count);
	}
	if (!State->bHandshakeDone)
	{
		const int Result = SSL_do_handshake(State->Ssl);
		if (Result != 1)
		{
			const int Error = SSL_get_error(State->Ssl, Result);
			return Error == SSL_ERROR_WANT_READ || Error == SSL_ERROR_WANT_WRITE;
		}
		State->bHandshakeDone = true;
	}
	uint8 Plain[16384];
	while (true)
	{
		const int Read = SSL_read(State->Ssl, Plain, sizeof(Plain));
		if (Read > 0)
		{
			OutPlain.Append(Plain, Read);
			continue;
		}
		const int Error = SSL_get_error(State->Ssl, Read);
		return Error == SSL_ERROR_WANT_READ || Error == SSL_ERROR_WANT_WRITE;
	}
#else
	return false;
#endif
}

bool FFlockFakeRelayTls::Write(const uint8* Bytes, int32 Count)
{
#if FLOCK_FAKE_RELAY_TLS
	return State->bHandshakeDone && SSL_write(State->Ssl, Bytes, Count) == Count;
#else
	return false;
#endif
}

void FFlockFakeRelayTls::TakeForTheNetwork(TArray<uint8>& Out)
{
#if FLOCK_FAKE_RELAY_TLS
	const int32 Pending = static_cast<int32>(BIO_ctrl_pending(State->ForTheClient));
	if (Pending > 0)
	{
		const int32 At = Out.Num();
		Out.AddUninitialized(Pending);
		const int Read = BIO_read(State->ForTheClient, Out.GetData() + At, Pending);
		Out.SetNum(At + FMath::Max(0, Read));
	}
#endif
}

#endif
