// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "FlockLocalPlayer.h"

#include "FlockEvents.h"

FString UFlockLocalPlayer::GetPlayerId() const
{
	return AuthSession->GetPlayerId();
}

bool UFlockLocalPlayer::IsAuthenticated() const
{
	return AuthSession->IsAuthenticated();
}

void UFlockLocalPlayer::Release(const FString& Reason, const FString& Fix)
{
	if (bReleased)
	{
		return;
	}
	bReleased = true;
	const bool bWasSignedIn = AuthSession->IsAuthenticated();
	CommandProvider->Shutdown();
	// Every later call needing a sign-in says why this player ended and what to do instead.
	AuthSession->SetNotSignedInFix(Reason + TEXT(" ") + Fix);
	// Ends the sign-in in memory too, and refuses any sign-in answered later, so it cannot sign this player in again.
	AuthSession->EndSignIn(Reason, Fix);
	HttpClient->Close(Reason, Fix);
	// Ended while signed in (the main player signed in as them, or Flock shut down): their own Logged Out says so, as a
	// removal's sign-out does.
	if (bWasSignedIn)
	{
		Events->InvokeLoggedOut();
	}
}

void UFlockLocalPlayer::HandleAuthenticated(const FFlockAuthInfo& Info)
{
	CommandProvider->FlushPendingWrites();
}
