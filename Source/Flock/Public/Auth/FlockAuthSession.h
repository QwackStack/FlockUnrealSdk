// Copyright 2022, Qwacks. All Rights Reserved.

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

	// ── State ──

	/**
	 * Adopts tokens and persists them. Returns false with OutError (state untouched) when the
	 * access token is non-empty but not a parseable JWT — the SDK cannot operate without claims,
	 * and a silent fallback would leave the player id empty with no obvious cause.
	 */
	bool SetTokens(const FString& InAccessToken, const FString& InRefreshToken, FString& OutError);

	/** Records how the session was established and re-persists (drives method-gated flows). */
	void SetAuthMethod(EFlockAuthMethod Method);

	/** Wipes tokens, claims, and method from memory and the store. */
	void ClearTokens();

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

	/** Store passthrough; false when no usable stored session. Never throws. */
	bool LoadPersisted(FFlockStoredTokens& OutTokens) const;

	// ── Refresh ──

	/**
	 * Refreshes the access token with the stored refresh token (single POST, single-flight).
	 * Completes false without network when there is no refresh token. On a server rejection the
	 * session is cleared and OnAuthExpired raised; on a transport failure tokens are kept so a
	 * flaky network doesn't sign the player out.
	 */
	void RefreshAccessToken(TFunction<void(bool)> OnComplete);

private:
	void Persist();
	void DrainRefreshCallbacks(bool bSuccess);
	void FailRefreshAsExpired();

	TSharedRef<FFlockHttpClient> Client;
	TSharedPtr<IFlockTokenStore> Store;
	TSharedRef<IFlockLogger> Logger;
	FString VersionedApiUrl;
	TMap<FString, FString> BaseHeaders;
	TWeakObjectPtr<UFlockEvents> Events;

	FString AccessToken;
	FString RefreshToken;
	TOptional<FFlockJwtClaims> Claims;
	TOptional<EFlockAuthMethod> AuthMethod;

	bool bRefreshInFlight = false;
	TArray<TFunction<void(bool)>> PendingRefreshCallbacks;
};
