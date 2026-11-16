// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "SProtokitePlaytestConsentWidget.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	// Given as the colours seen on screen (sRGB): Slate draws in linear colour, so a colour written as linear shows paler.
	const FLinearColor PanelColour(FColor(10, 13, 18, 245));
	const FLinearColor ScreenDimColour(FColor(0, 0, 0, 153));
	const FLinearColor LabelColour(FColor(235, 237, 245));
	const FLinearColor HelpColour(FColor(158, 166, 184));
	const FLinearColor AccentColour(FColor(250, 194, 46));
	const FLinearColor AccentHighlightColour(FColor(255, 217, 107));
	const FLinearColor AccentTextColour(FColor(18, 20, 26));

	FSlateFontInfo Font(int32 Size)
	{
		return FCoreStyle::GetDefaultFontStyle("Regular", Size);
	}

	FSlateFontInfo BoldFont(int32 Size)
	{
		return FCoreStyle::GetDefaultFontStyle("Bold", Size);
	}

	/** The two answers, in the order offered: everything, then nothing. One table, read by the buttons and the tests. */
	constexpr FProtokitePlaytestConsentOption Options[] = {
		{ EProtokitePlaytestConsentChoice::VideoAndPlayData, TEXT("Yes, help the developers") },
		{ EProtokitePlaytestConsentChoice::Nothing, TEXT("Not now") },
	};

	constexpr FProtokitePlaytestCollectedPart WhatIsCollected[] = {
		{ TEXT("Your screen"), TEXT("A video of your screen while you play, if this game records one.") },
		{ TEXT("How the game runs"), TEXT("Frame rate, memory and the levels you load.") },
		{ TEXT("Who sees it"), TEXT("Only the game's developers, to find what to fix.") },
		{ TEXT("Not now"), TEXT("Nothing is recorded or sent. Feedback you choose to send still reaches the developers.") },
		{ TEXT("Your other choices"), TEXT("This does not change any privacy choice the game asks you about.") },
		{ TEXT("Your answer"), TEXT("Saved on this device for next time.") },
	};

	/** The first answer's look: filled in the accent colour, since tinting the style's own grey button would darken it. */
	const FButtonStyle& AccentButtonStyle()
	{
		static const FButtonStyle Style = FButtonStyle(FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("Button"))
			.SetNormal(FSlateRoundedBoxBrush(AccentColour, 4.f))
			.SetHovered(FSlateRoundedBoxBrush(AccentHighlightColour, 4.f))
			.SetPressed(FSlateRoundedBoxBrush(AccentColour, 4.f));
		return Style;
	}

	/** The link's words: the accent colour, like the first answer. */
	const FTextBlockStyle& LinkTextStyle()
	{
		static const FTextBlockStyle Style = FTextBlockStyle(FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText"))
			.SetFont(Font(11))
			.SetColorAndOpacity(AccentColour);
		return Style;
	}

	TSharedRef<SWidget> Card(float Width, const TSharedRef<SWidget>& Content, const FLinearColor& Colour = PanelColour)
	{
		return SNew(SBox)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(Width)
				.MaxDesiredHeight(720.f)
				[
					SNew(SBorder)
					.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
					.BorderBackgroundColor(Colour)
					.Padding(28.f)
					[
						Content
					]
				]
			];
	}
}

TArrayView<const FProtokitePlaytestConsentOption> SProtokitePlaytestConsentWidget::GetOptions()
{
	return MakeArrayView(Options);
}

FString SProtokitePlaytestConsentWidget::GetQwacksLogoPath()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ProtokitePlaytest"));
	return Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("QwacksLogo.png")) : FString();
}

TArrayView<const FProtokitePlaytestCollectedPart> SProtokitePlaytestConsentWidget::GetWhatIsCollected()
{
	return MakeArrayView(WhatIsCollected);
}

void SProtokitePlaytestConsentWidget::Construct(const FArguments& InArgs)
{
	OnChosen = InArgs._OnChosen;
	AnswersCountFrom = FPlatformTime::Seconds() + SecondsBeforeAnAnswerCounts;
	const FString LogoPath = GetQwacksLogoPath();
	if (!LogoPath.IsEmpty() && FPaths::FileExists(LogoPath))
	{
		QwacksLogoBrush = MakeShared<FSlateDynamicImageBrush>(FName(*LogoPath), FVector2D(48.f, 48.f));
	}

	const TSharedRef<SVerticalBox> Answers = SNew(SVerticalBox);
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Options); ++Index)
	{
		Answers->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
		[
			BuildOption(Options[Index], Index == 0)
		];
	}

	const TSharedRef<SWidget> Question = SNew(SVerticalBox)
		// The pop-up covers the question; its answers take no press or focus until it shuts.
		.IsEnabled_Lambda([this]() { return !bWhatIsCollectedOpen; })

		// Centred above the heading; left out when the plugin's file is missing.
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 0.f, 0.f, 12.f)
		[
			SNew(SImage)
			.Image(QwacksLogoBrush.Get())
			.Visibility(QwacksLogoBrush.IsValid() ? EVisibility::Visible : EVisibility::Collapsed)
		]

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("Help the developers improve this game?")))
			.Font(BoldFont(22))
			.ColorAndOpacity(LabelColour)
			.AutoWrapText(true)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("We may record your screen and how the game runs while you play. Only the game's developers see it.")))
			.Font(Font(11))
			.ColorAndOpacity(HelpColour)
			.AutoWrapText(true)
		]

		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0.f, 4.f, 0.f, 0.f)
		[
			SNew(SHyperlink)
			.Text(FText::FromString(TEXT("What's collected?")))
			.TextStyle(&LinkTextStyle())
			.OnNavigate_Lambda([this]() { OpenWhatIsCollected(); })
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 20.f, 0.f, 0.f)
		[
			Answers
		];

	ChildSlot
	[
		SNew(SOverlay)

		+ SOverlay::Slot()
		[
			Card(620.f, Question)
		]

		+ SOverlay::Slot()
		[
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor(ScreenDimColour)
			.Visibility_Lambda([this]() { return bWhatIsCollectedOpen ? EVisibility::Visible : EVisibility::Collapsed; })
			[
				// Opaque, so the question's words do not show through it.
				Card(560.f, BuildWhatIsCollected(), PanelColour.CopyWithNewOpacity(1.f))
			]
		]
	];
}

TSharedRef<SWidget> SProtokitePlaytestConsentWidget::BuildOption(const FProtokitePlaytestConsentOption& Option, bool bOfferedFirst)
{
	const EProtokitePlaytestConsentChoice Choice = Option.Choice;
	return SNew(SButton)
		.HAlign(HAlign_Fill)
		.ContentPadding(FMargin(14.f, 12.f))
		.ButtonStyle(bOfferedFirst ? &AccentButtonStyle() : &FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("Button"))
		.OnClicked(FOnClicked::CreateSP(this, &SProtokitePlaytestConsentWidget::Press, Choice))
		[
			SNew(STextBlock)
			.Text(FText::FromString(Option.Title))
			.Font(BoldFont(13))
			.ColorAndOpacity(bOfferedFirst ? FSlateColor(AccentTextColour) : FSlateColor::UseForeground())
			.AutoWrapText(true)
		];
}

TSharedRef<SWidget> SProtokitePlaytestConsentWidget::BuildWhatIsCollected()
{
	const TSharedRef<SVerticalBox> Parts = SNew(SVerticalBox);
	for (const FProtokitePlaytestCollectedPart& Part : WhatIsCollected)
	{
		Parts->AddSlot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::FromString(Part.Title))
			.Font(BoldFont(12))
			.ColorAndOpacity(LabelColour)
			.AutoWrapText(true)
		];
		Parts->AddSlot().AutoHeight().Padding(0.f, 2.f, 0.f, 10.f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Part.Explanation))
			.Font(Font(10))
			.ColorAndOpacity(HelpColour)
			.AutoWrapText(true)
		];
	}

	return SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 14.f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("What's collected")))
			.Font(BoldFont(20))
			.ColorAndOpacity(LabelColour)
		]

		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				Parts
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			SNew(SButton)
			.HAlign(HAlign_Fill)
			.ContentPadding(FMargin(14.f, 12.f))
			.OnClicked(this, &SProtokitePlaytestConsentWidget::Back)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Back")))
				.Font(BoldFont(13))
			]
		];
}

FReply SProtokitePlaytestConsentWidget::Press(EProtokitePlaytestConsentChoice Choice)
{
	if (!bWhatIsCollectedOpen && FPlatformTime::Seconds() >= AnswersCountFrom)
	{
		OnChosen.ExecuteIfBound(Choice);
		return FReply::Handled();
	}
	return FReply::Handled().SetUserFocus(SharedThis(this), EFocusCause::SetDirectly);
}

FReply SProtokitePlaytestConsentWidget::OpenWhatIsCollected()
{
	bWhatIsCollectedOpen = true;
	return FReply::Handled();
}

FReply SProtokitePlaytestConsentWidget::Back()
{
	bWhatIsCollectedOpen = false;
	// A second click on Back lands on the answer under it.
	AnswersCountFrom = FPlatformTime::Seconds() + SecondsBeforeAnAnswerCounts;
	// Back on the question, not on the hidden Back button or an answer.
	return FReply::Handled().SetUserFocus(SharedThis(this), EFocusCause::SetDirectly);
}
