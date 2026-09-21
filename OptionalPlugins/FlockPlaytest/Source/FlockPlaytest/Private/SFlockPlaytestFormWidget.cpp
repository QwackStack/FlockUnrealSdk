// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "SFlockPlaytestFormWidget.h"

#include "Brushes/SlateDynamicImageBrush.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	const FLinearColor PanelColour(0.04f, 0.05f, 0.07f, 0.94f);
	const FLinearColor LabelColour(0.92f, 0.93f, 0.96f, 1.f);
	const FLinearColor HelpColour(0.62f, 0.65f, 0.72f, 1.f);
	const FLinearColor ProblemColour(1.f, 0.42f, 0.38f, 1.f);
	const FLinearColor NeededColour(1.f, 0.72f, 0.28f, 1.f);

	/** Drawn at half the file's 64 pixels, so it stays sharp on a high-density screen. */
	constexpr float FormIconSize = 32.f;

	FSlateFontInfo Font(int32 Size)
	{
		return FCoreStyle::GetDefaultFontStyle("Regular", Size);
	}

	FSlateFontInfo BoldFont(int32 Size)
	{
		return FCoreStyle::GetDefaultFontStyle("Bold", Size);
	}
}

FString SFlockPlaytestFormWidget::GetIconPath()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FlockPlaytest"));
	return Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("FeedbackFormIcon.png")) : FString();
}

void SFlockPlaytestFormWidget::Construct(const FArguments& InArgs)
{
	// A brush that loads its file when first drawn. A plain image brush is drawn only from textures a registered style
	// set loaded beforehand, so one made here draws a white square and says nothing. A packaged game carries the file
	// because the plugin's build rules stage it.
	const FString IconPath = GetIconPath();
	if (!IconPath.IsEmpty() && FPaths::FileExists(IconPath))
	{
		IconBrush = MakeShared<FSlateDynamicImageBrush>(FName(*IconPath), FVector2D(FormIconSize, FormIconSize));
	}

	Form = InArgs._Form;
	OnSubmitted = InArgs._OnSubmitted;
	OnClosed = InArgs._OnClosed;
	CanSendRecording = InArgs._CanSendRecording;
	OnSendRecording = InArgs._OnSendRecording;

	const TSharedRef<SVerticalBox> Fields = SNew(SVerticalBox);
	for (const FFlockPlaytestFormField& Field : Form.Fields)
	{
		Fields->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 18.f)
		[
			BuildField(Field)
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
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 12.f, 0.f)
						[
							SNew(SImage)
							.Image(IconBrush.Get())
							.Visibility(IconBrush.IsValid() ? EVisibility::Visible : EVisibility::Collapsed)
						]

						+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(FText::FromString(Form.Title.IsEmpty() ? TEXT("Feedback") : Form.Title))
							.Font(BoldFont(22))
							.ColorAndOpacity(LabelColour)
						]
					]

					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Form.Description))
						.Font(Font(11))
						.ColorAndOpacity(HelpColour)
						.AutoWrapText(true)
						.Visibility(Form.Description.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
					]

					+ SVerticalBox::Slot().FillHeight(1.f).Padding(0.f, 20.f, 0.f, 0.f)
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot()
						[
							Fields
						]
					]

					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 14.f, 0.f, 0.f)
					[
						SNew(SButton)
						.HAlign(HAlign_Center)
						.Visibility(this, &SFlockPlaytestFormWidget::GetSendRecordingVisibility)
						.OnClicked(this, &SFlockPlaytestFormWidget::SendTheRecording)
						.ToolTipText(FText::FromString(TEXT("Stops recording for the rest of this session and sends what was recorded.")))
						[
							SNew(STextBlock)
							.Text(FText::FromString(TEXT("Upload your recording")))
							.Font(Font(11))
						]
					]

					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 14.f, 0.f, 0.f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Your video is on its way. Recording has stopped for the rest of this session.")))
						.Visibility(this, &SFlockPlaytestFormWidget::GetRecordingOnItsWayVisibility)
						.Font(Font(10))
						.ColorAndOpacity(HelpColour)
						.AutoWrapText(true)
					]

					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 16.f, 0.f, 0.f)
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot().FillWidth(1.f)
						[
							SNew(SSpacer)
						]

						+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 10.f, 0.f)
						[
							SNew(SButton)
							.Text(FText::FromString(TEXT("Close")))
							.OnClicked_Lambda([this]()
							{
								OnClosed.ExecuteIfBound();
								return FReply::Handled();
							})
						]

						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SButton)
							.Text(FText::FromString(TEXT("Send")))
							.OnClicked_Lambda([this]()
							{
								TrySubmit();
								return FReply::Handled();
							})
						]
					]
				]
			]
		]
	];
}

TSharedRef<SWidget> SFlockPlaytestFormWidget::BuildField(const FFlockPlaytestFormField& Field)
{
	const FString FieldId = Field.Id;

	return SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(STextBlock)
				.Text(FText::FromString(Field.Label.IsEmpty() ? Field.Id : Field.Label))
				.Font(BoldFont(13))
				.ColorAndOpacity(LabelColour)
			]

			// The marker, not the word "required", so it reads the same in a form of any length.
			+ SHorizontalBox::Slot().AutoWidth().Padding(4.f, 0.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("*")))
				.Font(BoldFont(13))
				.ColorAndOpacity(NeededColour)
				.Visibility(Field.Required ? EVisibility::Visible : EVisibility::Collapsed)
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 3.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Field.HelpText))
			.Font(Font(10))
			.ColorAndOpacity(HelpColour)
			.AutoWrapText(true)
			.Visibility(Field.HelpText.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
		[
			BuildAnswerControl(Field)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 5.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Text(this, &SFlockPlaytestFormWidget::GetProblemText, FieldId)
			.Visibility(this, &SFlockPlaytestFormWidget::GetProblemVisibility, FieldId)
			.Font(Font(10))
			.ColorAndOpacity(ProblemColour)
			.AutoWrapText(true)
		];
}

TSharedRef<SWidget> SFlockPlaytestFormWidget::BuildAnswerControl(const FFlockPlaytestFormField& Field)
{
	const FString FieldId = Field.Id;

	if (Field.IsOfKind(FlockPlaytestFormFieldTypes::TextArea))
	{
		return SNew(SBox).HeightOverride(96.f)
		[
			SNew(SMultiLineEditableTextBox)
			.AutoWrapText(true)
			.OnTextChanged_Lambda([this, FieldId](const FText& Text) { Answers.SetText(FieldId, Text.ToString()); })
		];
	}

	if (Field.IsOfKind(FlockPlaytestFormFieldTypes::Rating))
	{
		return BuildRating(Field);
	}

	if (Field.IsOfKind(FlockPlaytestFormFieldTypes::Select))
	{
		return BuildSelect(Field);
	}

	if (Field.IsOfKind(FlockPlaytestFormFieldTypes::Checkbox))
	{
		// Recorded straight away, unticked, because the server counts a checkbox as answered whichever way it is set.
		// Leaving it unrecorded would make a required checkbox impossible to satisfy without ticking it, which is
		// stricter than the server and would refuse a submit it would have taken.
		Answers.SetChecked(FieldId, false);
		return SNew(SCheckBox)
			.OnCheckStateChanged_Lambda([this, FieldId](ECheckBoxState State)
			{
				Answers.SetChecked(FieldId, State == ECheckBoxState::Checked);
			});
	}

	// Text, and every kind this plugin does not know -- which the server reads as text as well, so a form using a
	// newer kind stays answerable instead of leaving a question nobody can fill in.
	return SNew(SEditableTextBox)
		.OnTextChanged_Lambda([this, FieldId](const FText& Text) { Answers.SetText(FieldId, Text.ToString()); });
}

TSharedRef<SWidget> SFlockPlaytestFormWidget::BuildRating(const FFlockPlaytestFormField& Field)
{
	const FString FieldId = Field.Id;
	const TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);

	for (int32 Score = FlockPlaytestRatings::Lowest; Score <= FlockPlaytestRatings::Highest; ++Score)
	{
		Row->AddSlot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)
		[
			SNew(SBox).WidthOverride(44.f)
			[
				SNew(SButton)
				.HAlign(HAlign_Center)
				.Text(FText::AsNumber(Score))
				.OnClicked_Lambda([this, FieldId, Score]()
				{
					// Picking the score already chosen clears it, so an optional rating can be taken back.
					Answers.SetRating(FieldId, Answers.GetRating(FieldId) == Score ? 0 : Score);
					return FReply::Handled();
				})
				.ButtonColorAndOpacity_Lambda([this, FieldId, Score]()
				{
					return Answers.GetRating(FieldId) == Score
						? FSlateColor(FLinearColor(0.20f, 0.52f, 0.96f, 1.f))
						: FSlateColor(FLinearColor(0.16f, 0.17f, 0.20f, 1.f));
				})
			]
		];
	}
	return Row;
}

TSharedRef<SWidget> SFlockPlaytestFormWidget::BuildSelect(const FFlockPlaytestFormField& Field)
{
	const FString FieldId = Field.Id;

	// The combo box holds a pointer to its list of options, so the list has to outlive this call and never move.
	const TSharedRef<TArray<TSharedPtr<FString>>> Options = SelectOptionLists.Add_GetRef(MakeShared<TArray<TSharedPtr<FString>>>());
	for (const FString& Option : Field.Options)
	{
		Options->Add(MakeShared<FString>(Option));
	}

	return SNew(SComboBox<TSharedPtr<FString>>)
		.OptionsSource(&Options.Get())
		.OnGenerateWidget_Lambda([](TSharedPtr<FString> Option)
		{
			return SNew(STextBlock).Text(FText::FromString(Option.IsValid() ? *Option : FString())).Font(Font(11));
		})
		.OnSelectionChanged_Lambda([this, FieldId](TSharedPtr<FString> Option, ESelectInfo::Type)
		{
			if (Option.IsValid())
			{
				Answers.SetChosenOption(FieldId, *Option);
			}
		})
		[
			SNew(STextBlock)
			.Font(Font(11))
			.Text_Lambda([this, FieldId]()
			{
				const FString Chosen = Answers.GetText(FieldId);
				return FText::FromString(Chosen.IsEmpty() ? TEXT("Choose one") : Chosen);
			})
		];
}

FText SFlockPlaytestFormWidget::GetProblemText(FString FieldId) const
{
	for (const FFlockPlaytestFormProblem& Problem : Problems)
	{
		if (Problem.FieldId.Equals(FieldId, ESearchCase::CaseSensitive))
		{
			return FText::FromString(Problem.Message);
		}
	}
	return FText::GetEmpty();
}

EVisibility SFlockPlaytestFormWidget::GetProblemVisibility(FString FieldId) const
{
	return GetProblemText(FieldId).IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

TArray<FFlockPlaytestFormProblem> SFlockPlaytestFormWidget::TrySubmit()
{
	// Checked here so a player is told before a request is spent, with the same rules the server keeps. Shown only
	// from now on: a form complaining while someone is still typing reads as broken.
	Problems = Answers.FindProblems(Form);
	if (Problems.Num() == 0)
	{
		OnSubmitted.ExecuteIfBound(Answers);
	}
	return Problems;
}

EVisibility SFlockPlaytestFormWidget::GetSendRecordingVisibility() const
{
	// Offered only while there is something to send. A playtest that records no video, or one whose recording has
	// already stopped, shows nothing rather than a button that would do nothing.
	if (bAskedForTheRecording || !CanSendRecording.IsBound())
	{
		return EVisibility::Collapsed;
	}
	return CanSendRecording.Execute() ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SFlockPlaytestFormWidget::GetRecordingOnItsWayVisibility() const
{
	// Only once sending really began, never merely because the button was pressed.
	return bTheRecordingIsOnItsWay ? EVisibility::Visible : EVisibility::Collapsed;
}

FReply SFlockPlaytestFormWidget::SendTheRecording()
{
	// Remembered here rather than read back from the recording: there is one recording a launch, so asking twice could
	// only ever send the same video again, and the player should see that their asking was taken.
	bAskedForTheRecording = true;
	bTheRecordingIsOnItsWay = OnSendRecording.IsBound() && OnSendRecording.Execute();
	return FReply::Handled();
}
