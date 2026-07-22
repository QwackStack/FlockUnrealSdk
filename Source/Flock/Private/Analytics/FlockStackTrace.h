// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Callstack capture, shared by the automatic log-sink path and the manual LogException call.
 *
 * Frames are formatted `Module+0xOffset`, plus function and `[file:line]` when symbols resolve. The
 * offset is measured from the module's base address, NOT the raw program counter: a program counter
 * is ASLR-shifted every run, so a trace built from raw addresses names the failing module but can
 * never be symbolicated afterwards. Module + offset is stable across runs and is what a symbol
 * server needs.
 *
 * Named frames are a bonus, not the contract — a shipped build has no PDBs on the symbol path.
 *
 * Symbol lookup is the expensive part, so callers that can cheaply decide to discard an entry
 * (the sink's queue cap) must decide before calling this.
 */
class FFlockStackTrace
{
public:
	/**
	 * FramesToSkip drops the capture plumbing so a trace starts at the caller. It cannot account for
	 * frames above the caller: when capture comes through UE_LOG the engine's
	 * UE_LOG -> GLog -> FOutputDevice dispatch contributes roughly five Core frames of its own, and
	 * that depth varies with the redirector, threading and buffering. A larger constant would eat
	 * real frames on other paths, which is worse than a few lines of plumbing.
	 */
	static FString Capture(uint32 FramesToSkip);
};
