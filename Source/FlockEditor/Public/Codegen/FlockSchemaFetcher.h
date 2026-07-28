// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Codegen/FlockSchemaSnapshot.h"
#include "Http/FlockHttpClient.h"

/**
 * Fetches the schema snapshot codegen emits from, at edit time.
 *
 * Five request families, chained rather than fired together: resolve the configured version *name* to its
 * id, then player templates, game configs, the paged shop list, and each shop's items. Sequential because
 * abort semantics matter more here than latency — the first failure has to stop the run before any
 * emitter sees a partial snapshot, and that is far easier to guarantee (and to read) when there is one
 * request in flight at a time. This is an editor action measured in a second or two, not a frame budget.
 *
 * The version is resolved by name even though project settings already carry a baked id, matching the
 * canonical SDK: resolving is what catches a *stale* bake, where the backend cut a new version under the
 * same name. Both ids ride on the snapshot so the caller can surface the mismatch.
 *
 * The client is injected rather than built internally, so tests drive the whole chain over the fake
 * transport. Use CreateDefaultClient() for the real one.
 */
class FLOCKEDITOR_API FFlockSchemaFetcher
{
public:
	/** Ok with a complete snapshot, or a failure carrying the first error — never a partial result. */
	DECLARE_DELEGATE_OneParam(FOnSchemaFetched, TFlockResult<FFlockSchemaSnapshot>);

	/**
	 * Runs the chain. ApiUrl is the unversioned base (the versioned segment is appended here), and
	 * BakedGameVersionId is copied onto the snapshot for the staleness check — it is not used to fetch.
	 */
	static void Fetch(const TSharedRef<FFlockHttpClient>& Client, const FString& ApiUrl, const FString& ApiKey,
		const FString& GameVersionName, const FString& BakedGameVersionId, FOnSchemaFetched OnComplete);

	/**
	 * Fetch driven by project settings; fails fast with a clear message when the settings are incomplete.
	 * The entry point the menu action and the CI commandlet both use.
	 */
	static void FetchFromSettings(FOnSchemaFetched OnComplete);

	/** An HTTP client over the engine's HTTP module, configured from project settings. */
	static TSharedRef<FFlockHttpClient> CreateDefaultClient();
};
