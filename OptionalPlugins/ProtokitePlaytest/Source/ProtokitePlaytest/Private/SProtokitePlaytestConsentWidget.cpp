// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "SProtokitePlaytestConsentWidget.h"

#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	const FLinearColor PanelColour(0.04f, 0.05f, 0.07f, 0.96f);
	const FLinearColor LabelColour(0.92f, 0.93f, 0.96f, 1.f);
	const FLinearColor HelpColour(0.62f, 0.65f, 0.72f, 1.f);

	FSlateFontInfo Font(int32 Size)
	{
		return FCoreStyle::GetDefaultFontStyle("Regular", Size);
	}

	FSlateFontInfo BoldFont(int32 Size)
	{
		return FCoreStyle::GetDefaultFontStyle("Bold", Size);
	}

	/**
	 * The four answers, in the order they are offered: everything first, then each half on its own, then nothing.
	 *
	 * One table, read by the buttons and by the tests, so an answer's words and the answer a button gives cannot come
	 * apart.
	 */
	constexpr FProtokitePlaytestConsentOption Options[] = {
		{
			EProtokitePlaytestConsentChoice::VideoAndPlayData,
			TEXT("Record my screen and collect play data"),
			TEXT("The playtest records what is on screen while I play, and collects how the game runs for me: frame rate, memory, the levels I load, and faults."),
		},
		{
			EProtokitePlaytestConsentChoice::VideoOnly,
			TEXT("Record my screen only"),
			TEXT("The playtest records what is on screen. It collects nothing about how the game runs."),
		},
		{
			EProtokitePlaytestConsentChoice::PlayDataOnly,
			TEXT("Collect play data only"),
			TEXT("The playtest collects how the game runs for me. Nothing on my screen is recorded."),
		},
		{
			EProtokitePlaytestConsentChoice::Nothing,
			TEXT("Collect nothing"),
			TEXT("The playtest collects nothing at all: nothing is recorded, nothing is sent, and no playtest session is made. Exactly what this game does with playtesting turned off."),
		},
	};
}

TArrayView<const FProtokitePlaytestConsentOption> SProtokitePlaytestConsentWidget::GetOptions()
{
	return MakeArrayView(Options);
}

void SProtokitePlaytestConsentWidget::Construct(const FArguments& InArgs)
{
	OnChosen = InArgs._OnChosen;

	const TSharedRef<SVerticalBox> Answers = SNew(SVerticalBox);
	for (const FProtokitePlaytestConsentOption& Option : Options)
	{
		Answers->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
		[
			BuildOption(Option)
		];
	}

	ChildSlot
	[
		SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(620.f)
			.MaxDesiredHeight(720.f)
			[
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
				.BorderBackgroundColor(PanelColour)
				.Padding(28.f)
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("What this playtest may collect")))
						.Font(BoldFont(22))
						.ColorAndOpacity(LabelColour)
					]

					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("You are playing a playtest build of this game. Choose what the playtest may collect while you play. This is the playtest's own question: it is separate from any privacy or analytics choice the game itself asks you about, and your answer here changes nothing else.")))
						.Font(Font(11))
						.ColorAndOpacity(HelpColour)
						.AutoWrapText(true)
					]

					+ SVerticalBox::Slot().FillHeight(1.f).Padding(0.f, 20.f, 0.f, 0.f)
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot()
						[
							Answers
						]
					]

					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 14.f, 0.f, 0.f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Your answer is kept on this computer and used every time you play this build. It does not cover the feedback form: that is sent only when you fill it in and press Send.")))
						.Font(Font(10))
						.ColorAndOpacity(HelpColour)
						.AutoWrapText(true)
					]
				]
			]
		]
	];
}

TSharedRef<SWidget> SProtokitePlaytestConsentWidget::BuildOption(const FProtokitePlaytestConsentOption& Option)
{
	const EProtokitePlaytestConsentChoice Choice = Option.Choice;
	return SNew(SButton)
		.HAlign(HAlign_Fill)
		.ContentPadding(FMargin(14.f, 12.f))
		.OnClicked(FOnClicked::CreateSP(this, &SProtokitePlaytestConsentWidget::Choose, Choice))
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(Option.Title))
				.Font(BoldFont(13))
				.AutoWrapText(true)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Option.Explanation))
				.Font(Font(10))
				.AutoWrapText(true)
			]
		];
}

FReply SProtokitePlaytestConsentWidget::Choose(EProtokitePlaytestConsentChoice Choice)
{
	OnChosen.ExecuteIfBound(Choice);
	return FReply::Handled();
}
