// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/Relay/FlockRelayTls.h"
#include "Misc/ScopeLock.h"
#include "Multiplayer/FlockDirectAddresses.h"
#include <atomic>

#if FLOCK_RELAY_OVER_TLS
#if WITH_SSL
#include "Ssl.h"
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#endif
THIRD_PARTY_INCLUDES_START
// OpenSSL names a type UI, as the engine names a namespace (ObjectMacros.h, in the shared precompiled header): its is renamed
// while its headers load, and nothing here uses it.
#define UI UI_ST
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#undef UI
THIRD_PARTY_INCLUDES_END
#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#endif
#define FLOCK_RELAY_TLS_LINKED 1
#endif
#endif

#ifndef FLOCK_RELAY_TLS_LINKED
#define FLOCK_RELAY_TLS_LINKED 0
#endif

namespace
{
	/** Set once the engine's SSL module was reached on the game thread, so the relay's thread may use it. */
	std::atomic<bool> bSslModuleReady{ false };

	FCriticalSection TestCertificateLock;
	FString TestCertificate;
}

struct FFlockRelayTlsState
{
#if FLOCK_RELAY_TLS_LINKED
	SSL_CTX* Context = nullptr;
	/** Owns both memory buffers once they are set on it. */
	SSL* Ssl = nullptr;
	BIO* FromTheNetwork = nullptr;
	BIO* ForTheNetwork = nullptr;
	/** The relay's name, which its certificate must carry. */
	FString Host;

	~FFlockRelayTlsState()
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

#if FLOCK_RELAY_TLS_LINKED
namespace
{
	/** OpenSSL's reason for the last failure on this thread, as one line. */
	FString LastTlsError()
	{
		const unsigned long Code = ERR_get_error();
		if (Code == 0)
		{
			return TEXT("no reason given");
		}
		char Text[256];
		ERR_error_string_n(Code, Text, sizeof(Text));
		return FString(UTF8_TO_TCHAR(Text));
	}

	/** The platform's own check passed for this certificate: the engine's own follows, as for its HTTP (the game's pinned keys). */
	int VerifyAsTheEngineDoes(int bPlatformTrustsIt, X509_STORE_CTX* Context)
	{
		if (bPlatformTrustsIt != 1)
		{
			return bPlatformTrustsIt;
		}
		const SSL* Ssl = static_cast<const SSL*>(X509_STORE_CTX_get_ex_data(Context, SSL_get_ex_data_X509_STORE_CTX_idx()));
		const FFlockRelayTlsState* State = Ssl != nullptr ? static_cast<const FFlockRelayTlsState*>(SSL_get_app_data(Ssl)) : nullptr;
		if (State == nullptr)
		{
			return 0;
		}
		return FSslModule::Get().GetCertificateManager().VerifySslCertificates(Context, State->Host) ? 1 : 0;
	}

	void TrustTheTestCertificate(SSL_CTX* Context)
	{
		FString Pem;
		{
			FScopeLock Locked(&TestCertificateLock);
			Pem = TestCertificate;
		}
		if (Pem.IsEmpty())
		{
			return;
		}
		const FTCHARToUTF8 Text(*Pem);
		BIO* Memory = BIO_new_mem_buf(Text.Get(), Text.Length());
		if (Memory == nullptr)
		{
			return;
		}
		if (X509* Certificate = PEM_read_bio_X509(Memory, nullptr, nullptr, nullptr))
		{
			X509_STORE_add_cert(SSL_CTX_get_cert_store(Context), Certificate);
			X509_free(Certificate);
		}
		BIO_free(Memory);
	}

	/** Why a handshake failed: a certificate this device does not trust says so, with OpenSSL's words for it. */
	FString WhyTheHandshakeFailed(const SSL* Ssl)
	{
		const long Verified = SSL_get_verify_result(Ssl);
		if (Verified != X509_V_OK)
		{
			return FString::Printf(TEXT("showed a certificate this device does not trust (%s); a network that inspects encrypted traffic does this"),
				UTF8_TO_TCHAR(X509_verify_cert_error_string(Verified)));
		}
		return FString::Printf(TEXT("did not finish the TLS handshake (%s)"), *LastTlsError());
	}
}
#endif

bool FFlockRelayTls::IsAvailable()
{
	return FLOCK_RELAY_TLS_LINKED != 0;
}

bool FFlockRelayTls::ReadyOnTheGameThread()
{
#if FLOCK_RELAY_TLS_LINKED
	if (!bSslModuleReady)
	{
		// Loads the module when nothing has yet (the engine's HTTP module normally has), which only the game thread may.
		FSslModule::Get();
		bSslModuleReady = true;
	}
	return true;
#else
	return false;
#endif
}

void FFlockRelayTls::SetTrustedCertificateForTesting(const FString& Pem)
{
	FScopeLock Locked(&TestCertificateLock);
	TestCertificate = Pem;
}

TUniquePtr<FFlockRelayTls> FFlockRelayTls::Start(const FString& Host, FString& OutCause)
{
	OutCause.Reset();
#if FLOCK_RELAY_TLS_LINKED
	if (!bSslModuleReady)
	{
		OutCause = TEXT("could not use TLS: the engine's SSL module was not readied on the game thread");
		return nullptr;
	}
	ERR_clear_error();
	TUniquePtr<FFlockRelayTlsState> State = MakeUnique<FFlockRelayTlsState>();
	State->Host = Host;
	State->Context = SSL_CTX_new(TLS_client_method());
	if (State->Context == nullptr)
	{
		OutCause = FString::Printf(TEXT("could not set TLS up (%s)"), *LastTlsError());
		return nullptr;
	}
	SSL_CTX_set_min_proto_version(State->Context, TLS1_2_VERSION);
	SSL_CTX_set_verify(State->Context, SSL_VERIFY_PEER, VerifyAsTheEngineDoes);
	FSslModule::Get().GetCertificateManager().AddCertificatesToSslContext(State->Context);
	TrustTheTestCertificate(State->Context);
	State->Ssl = SSL_new(State->Context);
	BIO* FromTheNetwork = BIO_new(BIO_s_mem());
	BIO* ForTheNetwork = BIO_new(BIO_s_mem());
	if (State->Ssl == nullptr || FromTheNetwork == nullptr || ForTheNetwork == nullptr)
	{
		BIO_free(FromTheNetwork);
		BIO_free(ForTheNetwork);
		OutCause = FString::Printf(TEXT("could not set TLS up (%s)"), *LastTlsError());
		return nullptr;
	}
	// Empty buffers ask for more rather than read as the end of the connection.
	BIO_set_mem_eof_return(FromTheNetwork, -1);
	BIO_set_mem_eof_return(ForTheNetwork, -1);
	SSL_set_bio(State->Ssl, FromTheNetwork, ForTheNetwork);
	State->FromTheNetwork = FromTheNetwork;
	State->ForTheNetwork = ForTheNetwork;
	SSL_set_app_data(State->Ssl, State.Get());
	SSL_set_connect_state(State->Ssl);
	const FTCHARToUTF8 Name(*Host);
	if (Host.Contains(TEXT(":")) || !FFlockDirectAddresses::Ipv4Value(Host).IsEmpty())
	{
		// A relay given as an address: its certificate must carry that address.
		X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(State->Ssl), Name.Get());
	}
	else
	{
		// The name the relay serves its certificate for, and the name that certificate must carry.
		SSL_set_tlsext_host_name(State->Ssl, const_cast<char*>(Name.Get()));
		SSL_set1_host(State->Ssl, Name.Get());
	}
	return TUniquePtr<FFlockRelayTls>(new FFlockRelayTls(MoveTemp(State)));
#else
	OutCause = TEXT("could not use TLS: this build of the game has none");
	return nullptr;
#endif
}

FFlockRelayTls::FFlockRelayTls(TUniquePtr<FFlockRelayTlsState> InState)
	: State(MoveTemp(InState))
{
}

FFlockRelayTls::~FFlockRelayTls() = default;

FFlockRelayTls::EStep FFlockRelayTls::Handshake(FString& OutCause)
{
#if FLOCK_RELAY_TLS_LINKED
	ERR_clear_error();
	const int Result = SSL_do_handshake(State->Ssl);
	if (Result == 1)
	{
		return EStep::Done;
	}
	const int Error = SSL_get_error(State->Ssl, Result);
	if (Error == SSL_ERROR_WANT_READ || Error == SSL_ERROR_WANT_WRITE)
	{
		return EStep::Waiting;
	}
	OutCause = WhyTheHandshakeFailed(State->Ssl);
#else
	OutCause = TEXT("could not use TLS: this build of the game has none");
#endif
	return EStep::Failed;
}

void FFlockRelayTls::PutArrived(const uint8* Bytes, int32 Count)
{
#if FLOCK_RELAY_TLS_LINKED
	if (Count > 0)
	{
		BIO_write(State->FromTheNetwork, Bytes, Count);
	}
#endif
}

int32 FFlockRelayTls::ReadPlain(uint8* Into, int32 Size, FString& OutCause)
{
#if FLOCK_RELAY_TLS_LINKED
	ERR_clear_error();
	const int Read = SSL_read(State->Ssl, Into, Size);
	if (Read > 0)
	{
		return Read;
	}
	const int Error = SSL_get_error(State->Ssl, Read);
	if (Error == SSL_ERROR_WANT_READ || Error == SSL_ERROR_WANT_WRITE)
	{
		return 0;
	}
	OutCause = Error == SSL_ERROR_ZERO_RETURN ? FString(TEXT("the relay ended TLS")) : FString::Printf(TEXT("TLS broke (%s)"), *LastTlsError());
#else
	OutCause = TEXT("could not use TLS: this build of the game has none");
#endif
	return -1;
}

bool FFlockRelayTls::WritePlain(const uint8* Bytes, int32 Count, FString& OutCause)
{
#if FLOCK_RELAY_TLS_LINKED
	ERR_clear_error();
	// Written whole: the buffer for the network takes all a record holds.
	if (SSL_write(State->Ssl, Bytes, Count) == Count)
	{
		return true;
	}
	OutCause = FString::Printf(TEXT("TLS broke (%s)"), *LastTlsError());
#else
	OutCause = TEXT("could not use TLS: this build of the game has none");
#endif
	return false;
}

bool FFlockRelayTls::HasBytesForTheNetwork() const
{
#if FLOCK_RELAY_TLS_LINKED
	return BIO_ctrl_pending(State->ForTheNetwork) > 0;
#else
	return false;
#endif
}

void FFlockRelayTls::TakeForTheNetwork(TArray<uint8>& Out)
{
#if FLOCK_RELAY_TLS_LINKED
	const int32 Pending = static_cast<int32>(BIO_ctrl_pending(State->ForTheNetwork));
	if (Pending <= 0)
	{
		return;
	}
	const int32 At = Out.Num();
	Out.AddUninitialized(Pending);
	const int Read = BIO_read(State->ForTheNetwork, Out.GetData() + At, Pending);
	Out.SetNum(At + FMath::Max(0, Read));
#endif
}
