// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Persists the player's analytics consent decision across runs.
 *
 * Three states, not two: no decision recorded, granted, revoked. The distinction matters because a
 * player who has explicitly revoked must stay revoked even on a project where consent is not
 * required — see ResolveEffective().
 *
 * Plain JSON under the project's Saved dir, deliberately not encrypted: a consent flag is not a
 * secret, and being able to read (and clear) it while debugging a privacy flow is worth more than
 * obscuring it. The decision is cached in memory at construction so the per-event consent check
 * never touches disk.
 *
 * Every failure is non-fatal: an unreadable or corrupt file reads as "no decision recorded".
 */
class FLOCK_API FFlockConsentStore
{
public:
	/** An empty FilePath uses DefaultPath(). Loads the stored decision immediately. */
	explicit FFlockConsentStore(const FString& InFilePath = FString());

	/** `<ProjectSavedDir>/Flock/analytics/consent.json`. */
	static FString DefaultPath();

	/** True once the game has recorded a decision either way. */
	bool HasDecision() const { return bHasDecision; }

	/** Returns false when nothing is stored, leaving OutGranted untouched. */
	bool Load(bool& OutGranted) const;

	void Save(bool bGranted);

	/** Forgets the decision — the "erase my analytics data" path, not a way to revoke. */
	void Clear();

	/**
	 * The consent the analytics core acts on.
	 *
	 * A recorded decision always wins. With no decision recorded, consent is implied unless the
	 * project requires an explicit opt-in — which is what makes bRequireExplicitConsent=false the
	 * collect-by-default mode and =true a hard gate.
	 */
	bool ResolveEffective(bool bRequireExplicitConsent) const;

private:
	FString FilePath;
	bool bHasDecision = false;
	bool bGrantedValue = false;
};
