// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

/**
 * This install's device id: a GUID the SDK makes once and keeps in device_id.txt among its saved files (Saved/Flock, beside the
 * saved sign-in), read the first time it is asked for and the same until the SDK shuts down. Each start of the SDK (a launch, a
 * Play In Editor session) reads the file again.
 *
 * Every copy of the game started from one project folder shares the file (two game clients, Play In Editor beside a standalone
 * game), so the first to save it wins: the file is only ever made new, never written over, and a launch that finds it made reads
 * that id. A file holding no GUID is moved aside before a new one is made, and a launch that moved another launch's new id aside
 * puts it back. An id written another way (upper case, braces, a line break or a byte-order mark around it) is used as written,
 * since the server may already know it so. Game thread only.
 */
class FLOCK_API FFlockDeviceIdFile
{
public:
	static const TCHAR* const FileName;

	/** Keeps the file in Folder: the SDK's saved files folder, or a test's own. */
	explicit FFlockDeviceIdFile(const FString& Folder);

	const FString& GetPath() const { return Path; }

	/**
	 * The id, read or made the first time it is asked for and the same after. When it cannot be kept, Warn hears why, once, and
	 * an id made for this start of the SDK is used, so the device calls still sign in.
	 */
	FString Read(const TFunction<void(const FString&)>& Warn);

	/** The GUID Text holds, trimmed of whitespace and byte-order marks but otherwise as written; empty when it holds none. */
	static FString IdIn(const FString& Text);

	/** Runs Hook once, just before the next save (a new file made or one moved aside), where a test has another launch save first. */
	static void SetBeforeNextSaveForTesting(TFunction<void()> Hook);

private:
	FString Path;
	FString Id;
};
