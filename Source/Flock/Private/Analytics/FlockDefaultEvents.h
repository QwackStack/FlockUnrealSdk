// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Analytics/FlockAnalyticsJson.h"
#include "FlockEventModels.h"
#include "Models/FlockAnalyticsModels.h"

/**
 * The SDK's own analytics events (Analytics Send Default Events) and the flock_ parameters every analytics event carries.
 * Only what the device knows: purchases that were paid, sign-ins, matches and the country are recorded by the server.
 */
namespace FlockDefaultEvents
{
	// ── The launch ──
	inline const TCHAR* const AppOpen = TEXT("app_open");
	inline const TCHAR* const FirstLaunch = TEXT("first_launch");
	inline const TCHAR* const AppUpdate = TEXT("app_update");
	inline const TCHAR* const AppCategory = TEXT("app");
	inline const TCHAR* const PreviousAppVersion = TEXT("previous_app_version");

	// ── Screens ──
	inline const TCHAR* const ScreenView = TEXT("screen_view");
	inline const TCHAR* const ScreenCategory = TEXT("screen");
	inline const TCHAR* const ScreenName = TEXT("screen_name");

	// ── The store's own dialog ──
	inline const TCHAR* const StorePurchase = TEXT("store_purchase");
	inline const TCHAR* const StoreCategory = TEXT("store");
	inline const TCHAR* const Cancelled = TEXT("cancelled");
	inline const TCHAR* const Failed = TEXT("failed");
	inline const TCHAR* const Deferred = TEXT("deferred");

	// ── The session's end ──
	inline const TCHAR* const SessionEnd = TEXT("session_end");
	inline const TCHAR* const SessionCategory = TEXT("session");
	inline const TCHAR* const Quit = TEXT("quit");
	inline const TCHAR* const Timeout = TEXT("timeout");
	inline const TCHAR* const SignOut = TEXT("sign_out");
	inline const TCHAR* const PlayerChanged = TEXT("player_changed");
	inline const TCHAR* const Restarted = TEXT("restarted");
	inline const TCHAR* const EndedByTheGame = TEXT("ended_by_the_game");
	/** The launch ended without ending its session (a crash, or the system closing the game); found at the next launch. */
	inline const TCHAR* const UnexpectedExit = TEXT("unexpected_exit");

	// ── Parameters on every analytics event ──
	inline const TCHAR* const Platform = TEXT("flock_platform");
	inline const TCHAR* const OperatingSystem = TEXT("flock_os");
	inline const TCHAR* const DeviceModel = TEXT("flock_device_model");
	inline const TCHAR* const AppVersion = TEXT("flock_app_version");
	inline const TCHAR* const SdkVersion = TEXT("flock_sdk_version");
	inline const TCHAR* const SessionNumber = TEXT("flock_session_number");
	inline const TCHAR* const Language = TEXT("flock_language");

	/** True for one of the SDK's own event names, letter for letter; a game's TrackEvent of one would count it twice. */
	bool IsOneOfTheSdksOwnEvents(const FString& EventName);

	/** The six names, for the refusal's tests and its message. */
	TArray<FString> Names();

	/** The game's version, from Project Settings > Project > Description > Project Version. */
	FString AppVersionOfThisGame();

	/**
	 * Adds the flock_ parameters. A property the event already has keeps its value: a key in any letter case, since the
	 * engine's JSON object cannot hold two spellings. No session number while no session runs.
	 */
	void AddParameters(FFlockCommandData& Properties, const FFlockDeviceInfo& Device, const TOptional<int32>& SessionNumber);

	/** Why a session ended, as session_end names it. */
	FString ReasonName(EFlockSessionEndReason Reason);

	/** The server keeps a session's length in whole seconds; rounded, so 0.9 s is 1, not 0. */
	int32 WholeSeconds(float Seconds);

	/** session_end's properties: reason, length, screens, bounce, number, first session, pauses, and the frame rate when sampled. */
	FFlockCommandData SessionEndProperties(const FFlockSessionSnapshot& Snapshot, const FString& Reason);

	/**
	 * What this game remembers on this device between launches: whether first_launch was recorded, and the version it last
	 * started with. One file per game (its backend and API key), beside the consent decision.
	 */
	struct FLaunchRecord
	{
		bool bFirstLaunchRecorded = false;
		FString LastAppVersion;

		/** Nothing remembered when the file is missing or unreadable. */
		static FLaunchRecord Load(const FString& Path);
		void Save(const FString& Path) const;
	};
}
