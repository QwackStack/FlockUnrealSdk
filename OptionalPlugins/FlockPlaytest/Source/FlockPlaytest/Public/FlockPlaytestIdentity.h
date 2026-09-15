// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockPlaytestIdentity.generated.h"

/** The longest values Protokite's session start accepts. A longer one would make it refuse the whole start. */
namespace FlockPlaytestIdentityLimits
{
	inline constexpr int32 SteamIdLength = 64;
	inline constexpr int32 DeviceIdLength = 200;
	inline constexpr int32 PlayerNameLength = 200;
}

/**
 * Who is playing, as Protokite is told: the Steam id when a Steam subsystem is running, otherwise this install's
 * device id. Exactly one of the two is set once the identity has been resolved.
 */
USTRUCT(BlueprintType)
struct FLOCKPLAYTEST_API FFlockPlaytestIdentity
{
	GENERATED_BODY()

	/** The player's Steam id; empty when the device id is used instead. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	FString SteamId;

	/** This install's device id; empty when the Steam id is used instead. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	FString DeviceId;

	/** The player's Steam name, shown on Protokite's Sessions page; empty when there is none. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock|Playtest")
	FString PlayerName;

	/** True when neither id is set: Protokite cannot take a session without one. */
	bool IsEmpty() const { return SteamId.IsEmpty() && DeviceId.IsEmpty(); }
};

/** The signed-in account of a Steam subsystem that is already running. Both members are empty when none is. */
struct FFlockRunningSteamAccount
{
	FString Id;
	FString Nickname;
};

/**
 * Reads the signed-in account of the engine's Steam subsystem, but only when something else has already created
 * one. It never creates, loads or starts a Steam subsystem, so a build without Steam never asks for it.
 */
FLOCKPLAYTEST_API FFlockRunningSteamAccount ReadRunningSteamAccount();

/**
 * True when Id is not empty, is at most MaxLength characters long and contains no whitespace. Never trims: a space
 * in an id another system issued means something went wrong there, and sending it would fail quietly later.
 */
FLOCKPLAYTEST_API bool IsUsablePlaytestId(const FString& Id, int32 MaxLength);

/** What reading the device id file found. */
enum class EFlockDeviceIdFileResult : uint8
{
	/** The file held a device id, which is used as it is. */
	Read,

	/** There was no file, so a new device id was made and saved. */
	Created,

	/** The file held something other than a device id this plugin writes, so a new device id replaced it. */
	Replaced,

	/** The file exists but could not be read. It is left untouched and no device id is used. */
	Unreadable,

	/**
	 * A new device id could not be saved, so none is used: an id that changes on every launch would show one player
	 * as many.
	 */
	CouldNotSave,
};

/** This install's device id, kept in a small file so it stays the same from one launch to the next. */
class FLOCKPLAYTEST_API FFlockPlaytestDeviceIdFile
{
public:
	explicit FFlockPlaytestDeviceIdFile(const FString& InPath);

	/** FlockPlaytest/device_id.txt in the project's Saved folder. */
	static FString GetDefaultPath();

	const FString& GetPath() const { return Path; }

	/**
	 * Reads the device id, making and saving one when the file does not exist yet. OutDeviceId is set for Read, Created
	 * and Replaced, and empty otherwise. A device id is a lower-case GUID with hyphens, and nothing else counts as one.
	 * Temporary files a crashed launch left next to the file are removed first.
	 */
	EFlockDeviceIdFileResult ReadOrCreate(FString& OutDeviceId) const;

private:
	/** Saves a new device id and reads the file back, so the id used is the one the file holds. */
	bool SaveNewDeviceId(FString& OutDeviceId) const;

	/** Removes temporary files older than a minute that a launch left behind while saving a new id. */
	void SweepStrayTemporaryFiles() const;

	FString Path;
};
