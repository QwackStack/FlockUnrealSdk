// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Setup/FlockConfigDetails.h"
#include "Setup/FlockSetupContext.h"
#include "Setup/FlockSetupUI.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "FlockEditor"

TSharedRef<IDetailCustomization> FFlockConfigDetails::MakeInstance()
{
	return MakeShared<FFlockConfigDetails>();
}

void FFlockConfigDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	const TArray<FFlockSetupFinding> Findings = FFlockSetupContext::Evaluate();
	if (Findings.Num() == 0)
	{
		// Nothing wrong, so no banner. A permanent "all good" strip would just be furniture.
		return;
	}

	int32 ErrorCount = 0;
	int32 WarningCount = 0;
	for (const FFlockSetupFinding& Finding : Findings)
	{
		if (Finding.Severity == EFlockSetupSeverity::Error)
		{
			++ErrorCount;
		}
		else if (Finding.Severity == EFlockSetupSeverity::Warning)
		{
			++WarningCount;
		}
	}

	const bool bBlocking = ErrorCount > 0;

	FText Headline;
	if (bBlocking)
	{
		Headline = FText::Format(
			LOCTEXT("BannerBlocking", "Flock is not ready — {0} {0}|plural(one=error,other=errors)"),
			FText::AsNumber(ErrorCount));
	}
	else
	{
		Headline = FText::Format(
			LOCTEXT("BannerWarning", "Flock works, but {0} {0}|plural(one=item,other=items) need attention"),
			FText::AsNumber(WarningCount));
	}

	// Findings are already ordered most-severe-first, so the first one is the one worth quoting.
	const FText Lead = FText::Format(LOCTEXT("BannerLead", "{0} {1}"), Findings[0].Title, Findings[0].Detail);

	IDetailCategoryBuilder& Category = DetailBuilder.EditCategory(
		"Setup Status", LOCTEXT("SetupStatusCategory", "Setup Status"), ECategoryPriority::Important);

	Category.AddCustomRow(LOCTEXT("SetupStatusRow", "Setup Status"))
	.WholeRowContent()
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(10.f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
				[
					SNew(SImage).Image(FAppStyle::GetBrush(
						bBlocking ? "Icons.ErrorWithColor" : "Icons.WarningWithColor"))
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Headline)
					.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
					.AutoWrapText(true)
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.Text(Lead)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("OpenFlockPanel", "Open Flock Panel"))
					.ToolTipText(LOCTEXT("OpenFlockPanelTip", "Every finding, with the buttons that fix them."))
					.OnClicked_Lambda([]()
					{
						FFlockSetupUI::OpenPanel();
						return FReply::Handled();
					})
				]
			]
		]
	];
}

#undef LOCTEXT_NAMESPACE
