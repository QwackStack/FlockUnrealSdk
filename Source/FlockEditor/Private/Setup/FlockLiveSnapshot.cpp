// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Setup/FlockLiveSnapshot.h"
#include "FlockSubsystem.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

namespace FlockLive
{
	/**
	 * The Flock subsystem belonging to the running PIE session, or null when there isn't one.
	 *
	 * Looked up per capture rather than cached: the panel outlives PIE, and a cached GameInstance would
	 * be a dangling read on the second play session.
	 */
	UFlockSubsystem* FindPieSubsystem()
	{
		if (!GEditor)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEditor->GetWorldContexts())
		{
			if (Context.WorldType != EWorldType::PIE || !Context.World())
			{
				continue;
			}
			if (UGameInstance* GameInstance = Context.World()->GetGameInstance())
			{
				if (UFlockSubsystem* Subsystem = GameInstance->GetSubsystem<UFlockSubsystem>())
				{
					return Subsystem;
				}
			}
		}
		return nullptr;
	}
}

FFlockLiveSnapshot FFlockPieSnapshotSource::Capture() const
{
	FFlockLiveSnapshot Snapshot;

	const UFlockSubsystem* Sdk = FlockLive::FindPieSubsystem();
	if (!Sdk)
	{
		return Snapshot;
	}

	Snapshot.bSdkPresent = true;
	Snapshot.bInitialized = Sdk->IsInitialized();
	Snapshot.InitializationError = Sdk->GetInitializationError();

	Snapshot.bAuthenticated = Sdk->IsAuthenticated();
	Snapshot.bRestoringSession = Sdk->IsRestoringSession();
	Snapshot.PlayerId = Sdk->GetPlayerId();

	Snapshot.bHasAnalyticsSession = Sdk->HasActiveAnalyticsSession();
	Snapshot.AnalyticsSessionId = Sdk->GetAnalyticsSessionId();
	Snapshot.bAnalyticsConsent = Sdk->HasAnalyticsConsent();

	Snapshot.PendingCommandWrites = Sdk->GetPendingCommandCount();
	Snapshot.bLikelyOffline = Sdk->IsLikelyOffline();

	return Snapshot;
}
