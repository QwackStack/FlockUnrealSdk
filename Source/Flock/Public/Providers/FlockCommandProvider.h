// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "Analytics/FlockLifecyclePump.h"
#include "Auth/FlockAuthSession.h"
#include "CoreMinimal.h"
#include "Http/FlockProviderBase.h"
#include "Models/FlockCommandModels.h"
#include "Models/FlockPlayerModels.h"

class FFlockPlayerProvider;

/**
 * Game commands: the authoritative, server-validated mutations of a player's data.
 *
 * Every command posts to a `game_command/*` route and answers with the whole updated player-data row, which
 * is written straight back through the player provider's cache — so a read right after a mutation sees the
 * new values rather than the pre-write ones.
 *
 * **Money is never queued and never retried blind.** AddGameFunds fails outright when the server can't be
 * reached instead of joining the offline queue, and posts non-idempotently so an ambiguous failure (the
 * request may have committed before the connection dropped) surfaces to the caller rather than being
 * re-sent into a double credit. The queueable commands — generic data updates, single-field updates,
 * achievement unlocks — are the ones a replay can repeat harmlessly.
 *
 * Offline queue: a queueable command issued with no connectivity is persisted under a **player-scoped**
 * snapshot key, overlaid optimistically onto the cached row, and replayed in order on the next flush. The
 * player scoping is the point — one player's queued writes must never replay under another's auth, so the
 * queue reloads whenever the signed-in player changes. Flushes are single-flight (a manual flush and an
 * automatic one can't both drain the queue and double-post) and stop at the first transient failure,
 * keeping the rest queued. A permanent 4xx will never succeed, so that entry is dropped — and the
 * optimistic row it wrote is evicted, since the server never accepted it.
 *
 * Auto-flush triggers mirror the queue's own lifecycle: returning to the foreground, connectivity coming
 * back mid-session (only observable when a reachability probe is set — UE has no dependable cross-platform
 * one, so the default probe always reports reachable), and signing in. The subsystem drives the last of
 * those; the provider owns its own lifecycle pump for the first two, because it must keep working with
 * analytics switched off.
 *
 * Completion-lambda rule: capture shared refs / weak self / values only — never `this`. Continuations that
 * re-enter the provider pin a TWeakPtr to itself, so teardown with requests in flight stays safe.
 */
class FLOCK_API FFlockCommandProvider
	: public FFlockProviderBase
	, public TSharedFromThis<FFlockCommandProvider>
{
public:
	FFlockCommandProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
		const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession,
		const FString& InVersionedApiUrl, const TSharedPtr<FFlockSnapshotStore>& InSnapshotStore,
		const FString& InGameVersionId);

	virtual ~FFlockCommandProvider() override;

	/**
	 * Wires the player provider used for the cache write-through, the offline overlay, and eviction. Weak,
	 * and may go null on teardown — every use pins it and skips when it can't. Set once at construction.
	 */
	void SetPlayerProvider(const TWeakPtr<FFlockPlayerProvider>& InPlayerProvider) { PlayerProvider = InPlayerProvider; }

	/** Starts the lifecycle pump that auto-flushes on foreground/reconnect. Idempotent. */
	void Initialize();

	/** Stops the pump. Anything still queued stays on disk and replays next run. Idempotent. */
	void Shutdown();

	// ── Commands ──

	/**
	 * Writes a set of fields onto a player-data row. Queued when offline (the cached row is overlaid so a
	 * read-after-write is consistent), posted immediately otherwise.
	 */
	void UpdatePlayerData(const FString& PlayerDataId, const FFlockCommandData& Data,
		TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete);

	/** Writes a single field onto a player-data row. Same offline behavior as UpdatePlayerData. */
	void UpdatePlayerDataField(const FString& PlayerDataId, const FString& Key, const FFlockCommandValue& Value,
		TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete);

	/**
	 * Unlocks an achievement on the signed-in player's achievements row — the row for the player template
	 * tagged "achievement", which the SDK resolves, so there is no id to pass. Queued when offline.
	 */
	void UnlockAchievement(const FString& AchievementName,
		TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete);

	/**
	 * Adds funds to the signed-in player's wallet, resolving the "currency"-tagged player template at
	 * runtime. Prefer the template-id overload where the id is known — it skips the lookup.
	 *
	 * Money-safe: fails with a Connection error when the server is unreachable (never queued) and does not
	 * retry an ambiguous failure (never double-credits).
	 */
	void AddGameFunds(const FString& Currency, int32 Amount,
		TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete);

	/**
	 * AddGameFunds against a known "currency"-tagged template id; the player's wallet row is resolved from
	 * it, so there is still no player-data id to pass. Same money-safety rules.
	 */
	void AddGameFunds(const FString& Currency, int32 Amount, const FString& CurrencyTemplateId,
		TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete);

	// ── Offline queue ──

	/**
	 * Replay attempts a single queued write gets before it is dropped as undeliverable.
	 *
	 * **Only a completed exchange counts.** A failure that never reached the server — Connection, Timeout,
	 * Cancelled, all of which carry `StatusCode` 0 — does not spend budget, and that gate is what makes this
	 * number safe. Without it a real outage burns the budget on its own: a flush fires on the rising edge of
	 * reachability, the HTTP client's offline latch self-expires every 30 seconds, so a game left open with
	 * no network manufactures an attempt every half-minute and would discard the player's change in about
	 * half an hour. A real outage must keep the player's writes; this exists only so that a verdict the
	 * classifier may have got wrong cannot block the queue forever — and a verdict needs a server to have
	 * answered.
	 */
	static constexpr int32 MaxReplayAttempts = 50;

	/**
	 * Replays queued commands oldest-first, reporting how many were delivered. Stops at the first transient
	 * failure (the rest stay queued); drops an entry the server permanently rejected. Single-flight: a
	 * second call while one is running completes immediately with the count so far, which is zero.
	 */
	void FlushPendingWrites(TFunction<void(TFlockResult<int32>)> OnComplete = nullptr);

	/** How many commands are waiting for the signed-in player. Loads the queue from disk on first ask. */
	int32 GetPendingWriteCount();

	/** Drops the signed-in player's queued commands without sending them, on disk and in memory. */
	void ClearPendingWrites();

	/** The pump driving foreground/reconnect auto-flush; exposed so tests can raise those without the engine. */
	FFlockLifecyclePump& GetPumpForTesting() { return Pump; }

private:
	FString MakeUrl(const FString& Path) const { return FString::Printf(TEXT("%s/%s"), *VersionedApiUrl, *Path); }
	TMap<FString, FString> HeadersNow() const { return Session->GetAuthHeaders(); }

	/** Posts a command body and writes the returned row through the player cache. */
	void PostCommand(const FString& Path, const FString& PayloadJson, const FString& Context, bool bIdempotent,
		TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete);

	/**
	 * The shared offline/online fork for a queueable command: offline it persists, overlays the cached row
	 * and answers Ok with that row; online it posts. Never used by AddGameFunds.
	 */
	void SendQueueable(const FString& Path, const FString& PayloadJson, const FString& Context,
		const FString& PlayerDataId, const FFlockCommandData& OptimisticFields,
		TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete);

	/** Persists the write and returns the optimistically-updated cached row (an empty row when uncached). */
	FFlockPlayerData EnqueueOffline(const FString& Path, const FString& PayloadJson, const FString& Context,
		const FString& PlayerDataId, const FFlockCommandData& OptimisticFields);

	/** Drains the head of the queue, then recurses; ends the flush on an empty queue or a transient failure. */
	void FlushNext(const TSharedRef<int32>& Delivered, TFunction<void(TFlockResult<int32>)> OnComplete);

	/** Ends the in-flight flush: clears the guard and reports the count exactly once. */
	void FinishFlush(int32 Delivered, TFunction<void(TFlockResult<int32>)> OnComplete);

	/** Fire-and-forget flush used by the automatic triggers; skipped while signed out. */
	void TriggerFlush();

	void HandleTick(float DeltaSeconds);
	void HandleBackgroundChanged(bool bBackgrounded);

	/** Loads the queue for the signed-in player, reloading whenever that player changes (logout included). */
	void EnsureQueueLoaded();
	void PersistQueue();

	/** "<version>/command/<player id>" — one queue per player, so a replay can't cross accounts. */
	FString GetQueueScope() const;

	/** Folds a mutated row into the player cache. No-op without a player provider. */
	void ApplyToPlayerCache(const FFlockPlayerData& Row);

	/**
	 * Drops the optimistic row a rejected write had left, unless another queued write still targets it —
	 * that one's overlay has to survive. Called after the rejected entry is already off the queue.
	 */
	void EvictOptimisticRow(const FString& PlayerDataId);

	/**
	 * True only when the **backend** authoritatively rejected this write, so replaying it can never succeed.
	 *
	 * The rule is "who said no", not "what number came back". A queued write is a change the player already
	 * made; discarding it needs an answer from the server, and everything else keeps it queued for the next
	 * flush. Three cases carry the whole thing, and each of them exists because the naive reading loses data
	 * or wedges the queue:
	 *
	 *  - **Serialization is not permanent.** The status on one of these is whatever the exchange carried —
	 *    a captive portal answering a write with an HTML 200 produces `Serialization` with `StatusCode` 200,
	 *    which is outside the 4xx range. Reading the status alone therefore calls it transient and stalls
	 *    the queue forever; reading the *type* alone and calling it permanent throws the write away when the
	 *    server never even saw it. It is genuinely ambiguous, so it stays queued and the attempt cap bounds
	 *    the stall.
	 *  - **Only a backend-coded 403 is authoritative.** A 401 clears on the next sign-in. A bare 403 with no
	 *    coded body is a proxy, WAF or corporate gateway — not this backend refusing the write.
	 *  - **Everything else is decided by status**, where a permanent 4xx really is the server's answer.
	 *
	 * FFlockPendingCommand::Attempts backstops all three: no verdict this function can get wrong is able to
	 * hold the queue indefinitely.
	 */
	static bool IsPermanentFailure(const FFlockError& Error);

	/**
	 * True when the head of the queue is still the entry a completion was sent for — i.e. ClearPendingWrites()
	 * did not empty it and nothing reloaded a different queue underneath.
	 *
	 * **Not sufficient on its own**, and the flush completion checks the player id too. A sign-out/sign-in
	 * during an in-flight replay does not reload the queue at all — nothing on that path calls
	 * EnsureQueueLoaded, and the sign-in flush trigger returns early while a flush is in flight — so the head
	 * still matches while the bearer has changed underneath it.
	 */
	bool HeadStillIs(const FFlockPendingCommand& Entry) const;


	TSharedRef<FFlockAuthSession> Session;
	TWeakPtr<FFlockPlayerProvider> PlayerProvider;
	FString VersionedApiUrl;

	FFlockLifecyclePump Pump;

	/** Oldest-first; index 0 is the head. A TArray, not a queue type, because it is persisted as a list. */
	TArray<FFlockPendingCommand> PendingWrites;
	bool bQueueLoaded = false;
	FString QueuePlayerId;

	bool bFlushInFlight = false;
	bool bWasReachable = true;

	static const TCHAR* const SnapshotCategory;
	static const TCHAR* const PendingWritesKey;
};
