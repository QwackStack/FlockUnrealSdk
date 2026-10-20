// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockJwt.h"
#include "Auth/FlockTokenStore.h"
#include "FlockEventModels.h"
#include "FlockLogger.h"
#include "UObject/WeakObjectPtrTemplates.h"

class FFlockHttpClient;
class UFlockEvents;

/**
 * Owner of the player's auth state: access/refresh tokens, parsed claims, the login method, and
 * their persistence via IFlockTokenStore. Also owns access-token refresh, single-flight: concurrent
 * callers while a refresh is in flight are queued onto the one POST rather than issuing their own.
 *
 * Game-thread only (all SDK HTTP completions arrive there), which is what makes the lock-free
 * single-flight sound. Built per-initialization by the subsystem; the URL/headers are fixed for the
 * session's lifetime.
 */
class FLOCK_API FFlockAuthSession : public TSharedFromThis<FFlockAuthSession>
{
public:
	FFlockAuthSession(const TSharedRef<FFlockHttpClient>& InClient, const TSharedPtr<IFlockTokenStore>& InStore,
		const TSharedRef<IFlockLogger>& InLogger, const FString& InVersionedApiUrl,
		const TMap<FString, FString>& InBaseHeaders);

	/** Event hub for OnTokenRefreshed/OnAuthExpired raises; safe to leave unset (tests). */
	void SetEvents(const TWeakObjectPtr<UFlockEvents>& InEvents) { Events = InEvents; }

	/**
	 * Run when the server refuses to refresh the sign-in, just before its tokens are cleared, while the player is still
	 * known: whatever a sign-out ends (the analytics session, the player's cached data) ends there too.
	 */
	void SetBeforeExpiredSignInEnds(TFunction<void()> InWork) { BeforeExpiredSignInEnds = MoveTemp(InWork); }

	// ── State ──

	/**
	 * Adopts tokens from a sign-in (a login or a restore) and persists them, which starts a new sign-in. Returns false
	 * with OutError (state untouched) when the access token is non-empty but not a parseable JWT — the SDK cannot
	 * operate without claims, and a silent fallback would leave the player id empty with no obvious cause.
	 */
	bool SetTokens(const FString& InAccessToken, const FString& InRefreshToken, FString& OutError);

	/** Records how the session was established and re-persists it, so a restore reports how the player signed in. */
	void SetAuthMethod(EFlockAuthMethod Method);

	/** Wipes tokens, claims, and method from memory and the store, which ends the sign-in. */
	void ClearTokens();

	/**
	 * Which sign-in is current: a login, a restore and a sign-out each move it, a token refresh does not. A request or a
	 * repeating call remembers the number it went out under, so nothing it does lands on a later one.
	 */
	int32 GetSignInNumber() const { return SignInNumber; }

	bool IsAuthenticated() const { return !AccessToken.IsEmpty(); }

	/** True when claims carry an expiry that has passed. No expiry claim = never expired. */
	bool IsTokenExpired() const;

	FString GetPlayerId() const { return Claims.IsSet() ? Claims.GetValue().PlayerId : FString(); }
	FString GetAccessToken() const { return AccessToken; }
	FString GetRefreshTokenValue() const { return RefreshToken; }
	const TOptional<FFlockJwtClaims>& GetClaims() const { return Claims; }
	TOptional<EFlockAuthMethod> GetAuthMethod() const { return AuthMethod; }

	/** Base headers plus Authorization: Bearer when signed in. Fetch per attempt, not per call site. */
	TMap<FString, FString> GetAuthHeaders() const;

	/** The API key and game version headers alone, for a route that never acts as the player. */
	const TMap<FString, FString>& GetBaseHeaders() const { return BaseHeaders; }

	/** Store passthrough; false when no usable stored session. Never throws. */
	bool LoadPersisted(FFlockStoredTokens& OutTokens) const;

	// ── Refresh ──

	/**
	 * Refreshes the access token with the stored refresh token (single POST, single-flight per sign-in).
	 * Completes false without network when there is no refresh token. On a server rejection the
	 * session is cleared and OnAuthExpired raised; on a transport failure tokens are kept so a
	 * flaky network doesn't sign the player out.
	 */
	void RefreshAccessToken(TFunction<void(bool)> OnComplete) { RefreshAccessToken(SignInNumber, MoveTemp(OnComplete)); }

	/**
	 * Refreshes the token of sign-in ForSignInNumber only: completes false without a request once that sign-in has ended,
	 * and a reply landing after it ended changes nothing (no tokens kept or cleared, no event), so a refresh can never
	 * sign a player back in after a sign-out or overwrite the next player.
	 */
	void RefreshAccessToken(int32 ForSignInNumber, TFunction<void(bool)> OnComplete);

private:
	/** One refresh POST and everyone waiting on it, for the sign-in it was sent for. */
	struct FRefreshInFlight
	{
		int32 SignInNumber = 0;
		TArray<TFunction<void(bool)>> Callbacks;
	};

	bool AdoptTokens(const FString& InAccessToken, const FString& InRefreshToken, FString& OutError);
	void Persist();
	void FinishRefresh(const TSharedRef<FRefreshInFlight>& Refresh, bool bSuccess);
	void FailRefreshAsExpired(const TSharedRef<FRefreshInFlight>& Refresh);

	TSharedRef<FFlockHttpClient> Client;
	TSharedPtr<IFlockTokenStore> Store;
	TSharedRef<IFlockLogger> Logger;
	FString VersionedApiUrl;
	TMap<FString, FString> BaseHeaders;
	TWeakObjectPtr<UFlockEvents> Events;
	TFunction<void()> BeforeExpiredSignInEnds;

	FString AccessToken;
	FString RefreshToken;
	TOptional<FFlockJwtClaims> Claims;
	TOptional<EFlockAuthMethod> AuthMethod;

	int32 SignInNumber = 0;
	/** The refresh on its way for the current sign-in; one sent for an earlier sign-in is let go when the number moves. */
	TSharedPtr<FRefreshInFlight> RefreshInFlight;
};
