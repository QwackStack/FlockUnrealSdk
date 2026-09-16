// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

/**
 * Gives requests already on their way a bounded chance to finish, for the moment a game is shutting down.
 *
 * The last thing an SDK does is the thing most likely to be lost. A session end is sent from shutdown and the game
 * then tears down around it, so the request is abandoned part-way: the engine's own HTTP module reports "Unbinding
 * delegates for N outstanding Http Requests" and drops them, leaving the session open on the server with no duration.
 * Nothing fails and the SDK logs nothing, because the completion that would have reported it is the thing being
 * unbound.
 *
 * Called while the SDK is still alive, so those completions actually run and say what became of the request. The wait
 * is bounded by the engine: it gives requests two seconds, cancels whatever is still going, and stops waiting at four,
 * so it cannot hang a game's exit, and a request that does not make it is cancelled exactly as it would have been a
 * moment later.
 *
 * This matters more than a lost request usually would, because a session end has no second chance: a saved end is not
 * re-sent on a later launch, so an end lost here leaves a session open for good.
 */
FLOCK_API void FlockFinishRequestsBeforeShutdown();
