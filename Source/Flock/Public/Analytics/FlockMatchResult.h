// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

/**
 * The match results the tracking plan names, for UFlockSubsystem::TrackMatchComplete. The server takes any word, so a game
 * may send its own instead. Blueprint gets them as the Flock Match Result Win, Loss and Draw nodes.
 */
namespace FlockMatchResult
{
	inline const TCHAR* const Win = TEXT("win");
	inline const TCHAR* const Loss = TEXT("loss");
	inline const TCHAR* const Draw = TEXT("draw");
}
