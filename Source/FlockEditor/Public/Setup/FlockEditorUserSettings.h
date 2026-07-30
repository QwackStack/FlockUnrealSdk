// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "FlockEditorUserSettings.generated.h"

/**
 * Per-developer, per-project editor state for the Flock setup panel.
 *
 * Deliberately **not** on UFlockConfig. That class is `Config = Game, DefaultConfig`, so it writes
 * DefaultGame.ini — a committed file. Last-seen-version state there would be actively wrong: the first
 * developer to upgrade would silence the notice for the whole team, and committing the file would flip
 * the flag for people who never upgraded at all.
 *
 * `EditorPerProjectUserSettings` lands under Saved/, which is already gitignored, so this stays
 * uncommitted by construction rather than by anyone remembering.
 */
UCLASS(Config = EditorPerProjectUserSettings)
class FLOCKEDITOR_API UFlockEditorUserSettings : public UObject
{
	GENERATED_BODY()

public:
	static UFlockEditorUserSettings* Get();

	/**
	 * SDK version this developer last saw the panel for. Empty means they have never seen it — which is
	 * what "first add" is, and why it fires exactly once per developer per project.
	 */
	UPROPERTY(Config)
	FString LastSeenSdkVersion;

	/**
	 * Findings whose auto-open the developer muted. Suppression hides the *interruption* only — the panel
	 * and the settings banner always report the finding to anyone who looks.
	 */
	UPROPERTY(Config)
	TArray<FName> SuppressedFindingIds;

	bool IsFirstAdd() const { return LastSeenSdkVersion.IsEmpty(); }

	/** Records the current SDK version as seen, and persists. */
	void MarkSeen(const FString& SdkVersion);

	bool IsSuppressed(FName FindingId) const { return SuppressedFindingIds.Contains(FindingId); }

	/** Mutes auto-open for this finding, and persists. */
	void Suppress(FName FindingId);

	/** Un-mutes everything — the panel's "show all notices again". */
	void ClearSuppressions();
};
