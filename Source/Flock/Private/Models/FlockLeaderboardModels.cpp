// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Models/FlockLeaderboardModels.h"

#include "Misc/Timespan.h"

namespace
{
	/** Reads a wire enum value case-insensitively; returns false when nothing matched. */
	template <typename TEnum, int32 N>
	bool ParseWireEnum(const FString& Wire, const TCHAR* const (&Names)[N], TEnum& OutValue)
	{
		for (int32 Index = 0; Index < N; ++Index)
		{
			if (Wire.Equals(Names[Index], ESearchCase::IgnoreCase))
			{
				OutValue = static_cast<TEnum>(Index);
				return true;
			}
		}
		return false;
	}

	// Order matches each enum's declaration order — ParseWireEnum indexes straight into it.
	const TCHAR* const ValueTypeNames[] = { TEXT("integer"), TEXT("float"), TEXT("duration") };
	const TCHAR* const DirectionNames[] = { TEXT("higher"), TEXT("lower") };
	const TCHAR* const AggregationNames[] = { TEXT("best"), TEXT("latest"), TEXT("sum") };
	const TCHAR* const WindowTypeNames[] = { TEXT("never"), TEXT("weekly"), TEXT("seasonal") };
	const TCHAR* const ScopeNames[] = { TEXT("global"), TEXT("country") };

	template <typename TEnum, int32 N>
	FString WireEnumToString(TEnum Value, const TCHAR* const (&Names)[N])
	{
		const int32 Index = static_cast<int32>(Value);
		return Names[(Index >= 0 && Index < N) ? Index : 0];
	}

	/** True when a field is present and not JSON null. Absent and null both mean "no value" here. */
	bool HasValue(const TSharedRef<FJsonObject>& Object, const TCHAR* Field)
	{
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
		return Value.IsValid() && Value->Type != EJson::Null;
	}

	/**
	 * Reads a required wire enum, failing the whole parse when it is missing or unrecognized. A board that
	 * silently defaulted its direction would sort a best-time board backwards with nothing to show for it.
	 */
	template <typename TEnum, int32 N>
	bool ReadRequiredEnum(const TSharedRef<FJsonObject>& Object, const TCHAR* Field,
		const TCHAR* const (&Names)[N], TEnum& OutValue, FString& OutError)
	{
		FString Wire;
		if (!Object->TryGetStringField(Field, Wire))
		{
			OutError = FString::Printf(TEXT("Leaderboard is missing '%s'"), Field);
			return false;
		}
		if (!ParseWireEnum(Wire, Names, OutValue))
		{
			OutError = FString::Printf(TEXT("Leaderboard has an unrecognized '%s': '%s'"), Field, *Wire);
			return false;
		}
		return true;
	}

	/** Seconds in, clock out: h:mm:ss.fff once past an hour, m:ss.fff below it. */
	FString FormatDuration(double Seconds)
	{
		const bool bNegative = Seconds < 0.0;
		const FTimespan Span = FTimespan::FromSeconds(FMath::Abs(Seconds));

		const FString Text = Span.GetTotalHours() >= 1.0
			? FString::Printf(TEXT("%d:%02d:%02d.%03d"), static_cast<int32>(Span.GetTotalHours()),
				Span.GetMinutes(), Span.GetSeconds(), Span.GetFractionMilli())
			: FString::Printf(TEXT("%d:%02d.%03d"), Span.GetMinutes(), Span.GetSeconds(), Span.GetFractionMilli());

		return bNegative ? TEXT("-") + Text : Text;
	}

	/** Up to two decimals, trailing zeros dropped — 1.5 reads as "1.5", not "1.50". */
	FString FormatFloat(double Score)
	{
		FString Text = FString::Printf(TEXT("%.2f"), Score);
		if (Text.Contains(TEXT(".")))
		{
			while (Text.EndsWith(TEXT("0")))
			{
				Text.LeftChopInline(1);
			}
			if (Text.EndsWith(TEXT(".")))
			{
				Text.LeftChopInline(1);
			}
		}
		return Text;
	}
}

FFlockLeaderboardWindow FFlockLeaderboardWindow::Current()
{
	return FFlockLeaderboardWindow();
}

FFlockLeaderboardWindow FFlockLeaderboardWindow::Season(const FString& SeasonId)
{
	FFlockLeaderboardWindow Window;
	Window.Key = TEXT("season:") + SeasonId;
	return Window;
}

FFlockLeaderboardWindow FFlockLeaderboardWindow::Period(const FString& PeriodKey)
{
	FFlockLeaderboardWindow Window;
	Window.Key = PeriodKey;
	return Window;
}

bool FlockLeaderboardValueTypeFromWire(const FString& Wire, EFlockLeaderboardValueType& OutValue)
{
	return ParseWireEnum(Wire, ValueTypeNames, OutValue);
}

bool FlockLeaderboardDirectionFromWire(const FString& Wire, EFlockLeaderboardDirection& OutValue)
{
	return ParseWireEnum(Wire, DirectionNames, OutValue);
}

bool FlockLeaderboardAggregationFromWire(const FString& Wire, EFlockLeaderboardAggregation& OutValue)
{
	return ParseWireEnum(Wire, AggregationNames, OutValue);
}

bool FlockLeaderboardWindowTypeFromWire(const FString& Wire, EFlockLeaderboardWindowType& OutValue)
{
	return ParseWireEnum(Wire, WindowTypeNames, OutValue);
}

bool FlockLeaderboardScopeFromWire(const FString& Wire, EFlockLeaderboardScope& OutValue)
{
	return ParseWireEnum(Wire, ScopeNames, OutValue);
}

FString FlockLeaderboardValueTypeToWire(EFlockLeaderboardValueType Value)
{
	return WireEnumToString(Value, ValueTypeNames);
}

FString FlockLeaderboardDirectionToWire(EFlockLeaderboardDirection Value)
{
	return WireEnumToString(Value, DirectionNames);
}

FString FlockLeaderboardAggregationToWire(EFlockLeaderboardAggregation Value)
{
	return WireEnumToString(Value, AggregationNames);
}

FString FlockLeaderboardWindowTypeToWire(EFlockLeaderboardWindowType Value)
{
	return WireEnumToString(Value, WindowTypeNames);
}

FString FlockLeaderboardScopeToWire(EFlockLeaderboardScope Value)
{
	return WireEnumToString(Value, ScopeNames);
}

FString FFlockLeaderboard::FormatScore(double Score, bool bRanked) const
{
	// No entry, or a number that cannot be shown. Either way there is nothing to print, and "nan" is not
	// something a player should ever read off a leaderboard.
	if (!bRanked || FMath::IsNaN(Score) || !FMath::IsFinite(Score))
	{
		return FString();
	}

	switch (ValueType)
	{
	case EFlockLeaderboardValueType::DurationSeconds:
		return FormatDuration(Score);
	case EFlockLeaderboardValueType::Float:
		return FormatFloat(Score);
	case EFlockLeaderboardValueType::Integer:
	default:
		return FString::Printf(TEXT("%lld"), static_cast<int64>(FMath::RoundToDouble(Score)));
	}
}

bool FFlockLeaderboard::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockLeaderboard& OutStruct, FString& OutError)
{
	Object->TryGetStringField(TEXT("id"), OutStruct.Id);
	Object->TryGetStringField(TEXT("name"), OutStruct.Name);

	return ReadRequiredEnum(Object, TEXT("value_type"), ValueTypeNames, OutStruct.ValueType, OutError)
		&& ReadRequiredEnum(Object, TEXT("direction"), DirectionNames, OutStruct.Direction, OutError)
		&& ReadRequiredEnum(Object, TEXT("aggregation"), AggregationNames, OutStruct.Aggregation, OutError)
		&& ReadRequiredEnum(Object, TEXT("window_type"), WindowTypeNames, OutStruct.WindowType, OutError)
		&& ReadRequiredEnum(Object, TEXT("scope"), ScopeNames, OutStruct.Scope, OutError);
}

bool FFlockStandingEntry::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockStandingEntry& OutStruct, FString& OutError)
{
	Object->TryGetNumberField(TEXT("rank"), OutStruct.Rank);
	Object->TryGetStringField(TEXT("player_id"), OutStruct.PlayerId);
	Object->TryGetNumberField(TEXT("score"), OutStruct.Score);
	Object->TryGetStringField(TEXT("achieved_at"), OutStruct.AchievedAt);

	// Nullable on the wire; an absent name or country is an empty string, never the literal "null".
	if (HasValue(Object, TEXT("player_name")))
	{
		Object->TryGetStringField(TEXT("player_name"), OutStruct.PlayerName);
	}
	if (HasValue(Object, TEXT("country")))
	{
		Object->TryGetStringField(TEXT("country"), OutStruct.Country);
	}
	return true;
}

bool FFlockStandings::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockStandings& OutStruct, FString& OutError)
{
	Object->TryGetStringField(TEXT("window"), OutStruct.Window);
	Object->TryGetNumberField(TEXT("total"), OutStruct.Total);

	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (Object->TryGetArrayField(TEXT("items"), Items) && Items != nullptr)
	{
		OutStruct.Items.Reserve(Items->Num());
		for (const TSharedPtr<FJsonValue>& Value : *Items)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Entry) || Entry == nullptr)
			{
				continue;
			}
			FFlockStandingEntry Parsed;
			if (FFlockStandingEntry::FromWireObject(Entry->ToSharedRef(), Parsed, OutError))
			{
				OutStruct.Items.Add(MoveTemp(Parsed));
			}
		}
	}
	return true;
}

bool FFlockPlayerRank::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockPlayerRank& OutStruct, FString& OutError)
{
	Object->TryGetStringField(TEXT("player_id"), OutStruct.PlayerId);
	Object->TryGetStringField(TEXT("window"), OutStruct.Window);

	// Null rank/score is the documented "no entry on this board yet", not a parse failure. Ranked carries
	// that absence because rank 0, score 0 and a negative score are all legitimate values here — there is
	// no sentinel this model could spend.
	const bool bHasRank = HasValue(Object, TEXT("rank"));
	const bool bHasScore = HasValue(Object, TEXT("score"));
	OutStruct.Ranked = bHasRank && bHasScore;

	if (OutStruct.Ranked)
	{
		Object->TryGetNumberField(TEXT("rank"), OutStruct.Rank);
		Object->TryGetNumberField(TEXT("score"), OutStruct.Score);
	}
	return true;
}
