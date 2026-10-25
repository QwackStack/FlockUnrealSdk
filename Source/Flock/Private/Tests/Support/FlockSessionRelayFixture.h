// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Multiplayer/Relay/FlockRelayTls.h"
#include "Multiplayer/Relay/FlockRelayWays.h"
#include "Tests/Support/FlockConnectingFixture.h"
#include "Tests/Support/FlockFakeRelayServer.h"
#include "Tests/Support/FlockRelayFixture.h"

// The session stack over the fake session server, whose seats draw logins for a fake relay on loopback, for the relay tests
// that open a relay as a session does. Include only from guarded test files.

namespace FlockRelayTestHelpers
{
	using namespace FlockConnectingTestHelpers;

	/** The session stack over the fake session server, whose seats draw logins for a fake relay on loopback (taking TCP and TLS too when made with streams). */
	struct FSessionRelayFixture : FConnectingFixture
	{
		TUniquePtr<FFlockFakeRelayServer> Relay;

		explicit FSessionRelayFixture(int32 MaxRetries = 0, bool bWithStreams = false)
			: FConnectingFixture(MaxRetries)
			, Relay(MakeUnique<FFlockFakeRelayServer>(bWithStreams))
		{
			Server->RelayUrls.Add(Relay->Url());
			Sessions().SetRelayTimingForTesting(Quick());
		}

		~FSessionRelayFixture()
		{
			FFlockRelayWays::AfterAWayOpenedForTesting = nullptr;
			FFlockRelayTls::SetTrustedCertificateForTesting(FString());
		}

		TSharedRef<FOpening> OpenRelay(UFlockMultiplayerSession* Of, const FString& OnServer = FString(), FFlockRequestHandle* OutHandle = nullptr)
		{
			const TSharedRef<FOpening> Answer = MakeShared<FOpening>();
			const FFlockRequestHandle Handle = Sessions().OpenRelay(Of, OnServer, Into(Answer));
			if (OutHandle != nullptr)
			{
				*OutHandle = Handle;
			}
			return Answer;
		}

		/** Runs frames, a millisecond apart, until Done or Seconds pass. */
		bool RelayFramesUntil(TFunctionRef<bool()> Done, double Seconds = 10.0)
		{
			const double Ends = FPlatformTime::Seconds() + Seconds;
			while (!Done() && FPlatformTime::Seconds() < Ends)
			{
				Frame();
				FPlatformProcess::Sleep(0.001f);
			}
			Frame();
			return Done();
		}

		void RealSeconds(double Seconds) { RelayFramesUntil([]() { return false; }, Seconds); }

		TArray<FFlockHttpRequest> Logins() const { return Server->RequestsTo(ERoute::RelayCredentials); }

		/** Flock lists these servers (in this order) by names of their own, each found on loopback, as two servers are two names. */
		void NameEachServer(const TArray<FString>& RelayUrls, FFlockRelayTiming Timing = Quick())
		{
			Server->RelayUrls = RelayUrls;
			Timing.NameLookUpForTesting = EveryNameOnLoopback();
			Sessions().SetRelayTimingForTesting(Timing);
		}
	};
}

#endif
