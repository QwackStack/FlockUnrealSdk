// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Multiplayer/FlockTravelToSession.h"

/**
 * The engine's side of a travel and of a host's check of joining players, scripted by the test: what the game's networking says,
 * what the travel asked of it, and the connections waiting on the host. Include only from guarded test files.
 */
class FFlockScriptedGameTravel : public IFlockGameTravel
{
public:
	FFlockGameNetState State;
	FString Level = TEXT("/Game/Maps/Lobby");
	TArray<FString> Opened;
	/** Where each travel went ("address:port"), in order. */
	TArray<FString> TraveledTo;
	/** The join token each travel's URL carried (?FlockJoinToken=), in order; empty for none. */
	TArray<FString> TokensTraveledWith;
	/** Each travel's whole URL, in order. */
	TArray<FString> Urls;
	int32 StoppedListening = 0;
	int32 StoppedConnecting = 0;
	TFunction<void(const FString&, const FString&)> OnFailure;

	/** A connection the host has: what its request carries and where from, and whether a check has asked about it yet. */
	struct FJoiningConnection
	{
		uint64 Number = 0;
		FString Options;
		FString Address;
		bool bAskedAbout = false;
	};
	TArray<FJoiningConnection> Connections;
	/** The connections still on the host. */
	TSet<uint64> Here;
	/** The connection each stand-in player controller plays through. */
	TMap<const APlayerController*, uint64> Controllers;
	/** The stand-in controllers of this device's own players. */
	TSet<const APlayerController*> OwnPlayers;
	/** What IsThisGame answers for any game mode. */
	bool bThisGame = true;

	FFlockScriptedGameTravel()
	{
		State.bHasWorld = true;
	}

	virtual FFlockGameNetState Read() const override { return State; }
	virtual FString CurrentLevel() const override { return Level; }
	virtual void MarkWorld() override { State.bInAnotherWorld = false; }
	virtual void OpenListening(const FString& InLevel) override
	{
		Opened.Add(InLevel);
		State.bChangingLevel = true;
	}
	virtual void TravelTo(const FString& Url) override
	{
		Urls.Add(Url);
		FString Where = Url;
		FString Options;
		Url.Split(TEXT("?"), &Where, &Options);
		TraveledTo.Add(Where);
		FString Token;
		TArray<FString> Parts;
		Options.ParseIntoArray(Parts, TEXT("?"), true);
		for (const FString& Part : Parts)
		{
			if (Part.StartsWith(TEXT("FlockJoinToken="), ESearchCase::CaseSensitive))
			{
				Token = Part.Mid(15);
			}
		}
		TokensTraveledWith.Add(Token);
		// The engine reads the world it leaves as a client while it connects (measured).
		State.bChangingLevel = true;
		State.NetMode = NM_Client;
	}
	virtual void StopListening() override
	{
		++StoppedListening;
		State.NetMode = NM_Standalone;
		State.ListeningPort = 0;
	}
	virtual void StopConnecting() override
	{
		++StoppedConnecting;
		State.bChangingLevel = false;
		State.NetMode = NM_Standalone;
	}
	virtual void WatchFailures(TFunction<void(const FString& Kind, const FString& Words)> InOnFailure) override { OnFailure = MoveTemp(InOnFailure); }

	virtual uint64 FindJoiningConnection(const FString& Options, const FString& Address) override
	{
		for (int32 Index = Connections.Num() - 1; Index >= 0; --Index)
		{
			FJoiningConnection& Connection = Connections[Index];
			if (!Connection.bAskedAbout && Here.Contains(Connection.Number) && Connection.Options.Equals(Options, ESearchCase::CaseSensitive)
				&& Connection.Address.Equals(Address, ESearchCase::CaseSensitive))
			{
				Connection.bAskedAbout = true;
				return Connection.Number;
			}
		}
		return 0;
	}
	virtual bool IsStillHere(uint64 Connection) const override { return Here.Contains(Connection); }
	virtual uint64 ConnectionOf(const APlayerController* Controller) const override
	{
		const uint64* Found = Controllers.Find(Controller);
		return Found != nullptr ? *Found : 0;
	}
	virtual bool IsOwnPlayer(const APlayerController* Controller) const override { return OwnPlayers.Contains(Controller); }
	virtual bool IsThisGame(const AGameModeBase* GameMode) const override { return bThisGame; }

	/** The level opened again, listening on Port (0: it opened without listening). */
	void Opens(int32 Port)
	{
		State.bChangingLevel = false;
		State.bInAnotherWorld = true;
		State.NetMode = Port > 0 ? NM_ListenServer : NM_Standalone;
		State.ListeningPort = Port;
	}
	/** The host's level loaded and the connection is open; bLetIn once the host has let the player in. */
	void Arrives(bool bLetIn)
	{
		State.bChangingLevel = false;
		State.bInAnotherWorld = true;
		State.NetMode = NM_Client;
		State.bLetInByHost = bLetIn;
	}
	/** The engine gave the connection up and loaded the game's default level. */
	void GoesBackToItsDefaultLevel()
	{
		State.bChangingLevel = false;
		State.bInAnotherWorld = true;
		State.NetMode = NM_Standalone;
	}
	/** The engine tells a failure, written "Kind: words" as the engine logs it. */
	void Fails(const FString& KindAndWords)
	{
		FString Kind = KindAndWords;
		FString Words;
		KindAndWords.Split(TEXT(": "), &Kind, &Words);
		if (OnFailure)
		{
			OnFailure(Kind, Words);
		}
	}

	/** A player's connection reaches the host, its request carrying Options from Address; its number. */
	uint64 Connects(const FString& Options, const FString& Address)
	{
		const uint64 Number = static_cast<uint64>(Connections.Num()) + 1;
		Connections.Add(FJoiningConnection{ Number, Options, Address, false });
		Here.Add(Number);
		return Number;
	}
	/** The player's connection is gone from the host. */
	void Leaves(uint64 Connection) { Here.Remove(Connection); }
};
