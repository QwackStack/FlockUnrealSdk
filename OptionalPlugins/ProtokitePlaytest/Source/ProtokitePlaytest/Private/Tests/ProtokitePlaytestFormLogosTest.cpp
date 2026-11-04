// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Engine/Texture2D.h"
#include "Layout/ArrangedChildren.h"
#include "ProtokitePlaytestFormLogos.h"
#include "ProtokitePlaytestLibrary.h"
#include "ProtokitePlaytestSettings.h"
#include "ProtokitePlaytestSubsystem.h"
#include "SProtokitePlaytestFormWidget.h"
#include "Styling/SlateBrush.h"
#include "Tests/ProtokitePlaytestSubsystemTestSupport.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/SBoxPanel.h"

#include <limits>

using namespace ProtokitePlaytestSubsystemTesting;

namespace ProtokitePlaytestFormLogosTesting
{
	/** An image of the given size, loaded and drawable, as a game's imported texture would be. */
	TStrongObjectPtr<UTexture2D> Image(int32 Width, int32 Height)
	{
		return TStrongObjectPtr<UTexture2D>(UTexture2D::CreateTransient(Width, Height));
	}

	/** A settings object of its own, so a test never reads or changes the project's. */
	TStrongObjectPtr<UProtokitePlaytestSettings> NewSettings()
	{
		TStrongObjectPtr<UProtokitePlaytestSettings> Settings(NewObject<UProtokitePlaytestSettings>(GetTransientPackage()));
		Settings->FeedbackFormLogos.Reset();
		Settings->FeedbackFormFooterLogo.Reset();
		Settings->FeedbackFormLogoHeight = UProtokitePlaytestSettings::DefaultFeedbackFormLogoHeight;
		Settings->FeedbackFormFooterLogoHeight = UProtokitePlaytestSettings::DefaultFeedbackFormFooterLogoHeight;
		return Settings;
	}

	/** The form a game would open with these settings. */
	TSharedRef<SProtokitePlaytestFormWidget> FormWith(const UProtokitePlaytestSettings& Settings)
	{
		FProtokitePlaytestForm Form;
		Form.Id = TEXT("form-1");
		Form.Title = TEXT("How was this playtest?");
		return SNew(SProtokitePlaytestFormWidget).Form(Form).Logos(FProtokitePlaytestFormLogos::FromSettings(Settings));
	}

	/** The texture a brush draws. */
	const UObject* Drawn(const FSlateBrush* Brush)
	{
		return Brush != nullptr ? Brush->GetResourceObject() : nullptr;
	}

	/** Sets the project's logo settings for one test and puts them back when it ends. */
	struct FScopedFormLogoSettings
	{
		UProtokitePlaytestSettings* Settings = GetMutableDefault<UProtokitePlaytestSettings>();
		TArray<TSoftObjectPtr<UTexture2D>> SavedLogos = Settings->FeedbackFormLogos;
		float SavedLogoHeight = Settings->FeedbackFormLogoHeight;
		TSoftObjectPtr<UTexture2D> SavedFooterLogo = Settings->FeedbackFormFooterLogo;
		float SavedFooterLogoHeight = Settings->FeedbackFormFooterLogoHeight;

		FScopedFormLogoSettings()
		{
			Settings->FeedbackFormLogos.Reset();
			Settings->FeedbackFormFooterLogo.Reset();
			Settings->FeedbackFormLogoHeight = UProtokitePlaytestSettings::DefaultFeedbackFormLogoHeight;
			Settings->FeedbackFormFooterLogoHeight = UProtokitePlaytestSettings::DefaultFeedbackFormFooterLogoHeight;
		}

		~FScopedFormLogoSettings()
		{
			Settings->FeedbackFormLogos = SavedLogos;
			Settings->FeedbackFormLogoHeight = SavedLogoHeight;
			Settings->FeedbackFormFooterLogo = SavedFooterLogo;
			Settings->FeedbackFormFooterLogoHeight = SavedFooterLogoHeight;
		}
	};

	/** Where a widget was placed among a panel's arranged children, or null when it was not. */
	const FArrangedWidget* Find(const FArrangedChildren& Arranged, const TSharedPtr<SWidget>& Widget)
	{
		for (int32 Index = 0; Index < Arranged.Num(); ++Index)
		{
			if (Arranged[Index].Widget == Widget)
			{
				return &Arranged[Index];
			}
		}
		return nullptr;
	}

	/** The width a widget is drawn at, its layout scale included. */
	float DrawnWidth(const FArrangedWidget& Arranged)
	{
		return Arranged.Geometry.GetLocalSize().X * Arranged.Geometry.GetAccumulatedLayoutTransform().GetScale();
	}

	float DrawnHeight(const FArrangedWidget& Arranged)
	{
		return Arranged.Geometry.GetLocalSize().Y * Arranged.Geometry.GetAccumulatedLayoutTransform().GetScale();
	}
}

using namespace ProtokitePlaytestFormLogosTesting;

/**
 * The four settings are on the project settings page under the names and ranges a game reads in the docs, typed as
 * textures, and reachable from Blueprint.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormLogoSettingsTest,
	"Protokite.Playtest.Form.Logos.TheSettingsAreNamedTypedAndKeptInRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormLogoSettingsTest::RunTest(const FString& Parameters)
{
	struct FExpected
	{
		const TCHAR* Property;
		const TCHAR* DisplayName;
		const TCHAR* ClampMin;
		const TCHAR* ClampMax;
	};
	const FExpected Expected[] = {
		{ TEXT("FeedbackFormLogos"), TEXT("Feedback Form Logos"), nullptr, nullptr },
		{ TEXT("FeedbackFormLogoHeight"), TEXT("Feedback Form Logo Height"), TEXT("16"), TEXT("96") },
		{ TEXT("FeedbackFormFooterLogo"), TEXT("Feedback Form Footer Logo"), nullptr, nullptr },
		{ TEXT("FeedbackFormFooterLogoHeight"), TEXT("Feedback Form Footer Logo Height"), TEXT("12"), TEXT("48") },
	};
	for (const FExpected& Each : Expected)
	{
		const FProperty* Property = UProtokitePlaytestSettings::StaticClass()->FindPropertyByName(Each.Property);
		if (!TestNotNull(Each.Property, Property))
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("%s is saved with the project"), Each.Property), Property->HasAnyPropertyFlags(CPF_Config));
		TestTrue(FString::Printf(TEXT("%s can be set from Blueprint"), Each.Property), Property->HasAllPropertyFlags(CPF_BlueprintVisible) && !Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly));
#if WITH_EDITORONLY_DATA
		TestEqual(FString::Printf(TEXT("%s's name on the settings page"), Each.Property), Property->GetMetaData(TEXT("DisplayName")), FString(Each.DisplayName));
		if (Each.ClampMin != nullptr)
		{
			TestEqual(FString::Printf(TEXT("%s's lowest"), Each.Property), Property->GetMetaData(TEXT("ClampMin")), FString(Each.ClampMin));
			TestEqual(FString::Printf(TEXT("%s's highest"), Each.Property), Property->GetMetaData(TEXT("ClampMax")), FString(Each.ClampMax));
		}
#endif
	}

	// Typed as textures, so the settings page offers only images and Blueprint sees what each one is.
	const FArrayProperty* Logos = CastField<FArrayProperty>(UProtokitePlaytestSettings::StaticClass()->FindPropertyByName(TEXT("FeedbackFormLogos")));
	const FSoftObjectProperty* LogoSlot = Logos != nullptr ? CastField<FSoftObjectProperty>(Logos->Inner) : nullptr;
	if (TestNotNull(TEXT("A list of soft references"), LogoSlot))
	{
		TestTrue(TEXT("To textures"), LogoSlot->PropertyClass == UTexture2D::StaticClass());
	}
	const FSoftObjectProperty* Footer = CastField<FSoftObjectProperty>(UProtokitePlaytestSettings::StaticClass()->FindPropertyByName(TEXT("FeedbackFormFooterLogo")));
	if (TestNotNull(TEXT("A soft reference"), Footer))
	{
		TestTrue(TEXT("To a texture"), Footer->PropertyClass == UTexture2D::StaticClass());
	}
#if WITH_EDITORONLY_DATA
	TestTrue(TEXT("The import advice is on the page"), Logos != nullptr && Logos->GetToolTipText().ToString().Contains(TEXT("Padding and Resizing set to None")));
#endif
	TestTrue(TEXT("Blueprint reaches the settings themselves"), UProtokitePlaytestLibrary::GetPlaytestSettings() == GetMutableDefault<UProtokitePlaytestSettings>());

	// The page keeps a height in range, but a game can set any number from C++, Blueprint or the config file.
	TestEqual(TEXT("Too short a logo"), FProtokitePlaytestFormLogos::KeepLogoHeightInRange(0.f), 16.f);
	TestEqual(TEXT("Too tall a logo"), FProtokitePlaytestFormLogos::KeepLogoHeightInRange(500.f), 96.f);
	TestEqual(TEXT("A logo height that is not a number"), FProtokitePlaytestFormLogos::KeepLogoHeightInRange(std::numeric_limits<float>::quiet_NaN()), 40.f);
	TestEqual(TEXT("Too short a footer"), FProtokitePlaytestFormLogos::KeepFooterLogoHeightInRange(-3.f), 12.f);
	TestEqual(TEXT("Too tall a footer"), FProtokitePlaytestFormLogos::KeepFooterLogoHeightInRange(100.f), 48.f);
	TestEqual(TEXT("A footer height that is not a number"), FProtokitePlaytestFormLogos::KeepFooterLogoHeightInRange(std::numeric_limits<float>::infinity()), 22.f);
	TestEqual(TEXT("A height in range is kept"), FProtokitePlaytestFormLogos::KeepLogoHeightInRange(30.f), 30.f);
	return true;
}

/** With no logos, or only slots that cannot be drawn, there is no row and the form is as it always was. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormNoLogoRowTest,
	"Protokite.Playtest.Form.Logos.NoRowWithoutLogosOrWithOnlyEmptySlots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormNoLogoRowTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UProtokitePlaytestSettings> Settings = NewSettings();
	TestFalse(TEXT("None set"), FormWith(*Settings)->GetLogoRowForTesting().IsValid());

	Settings->FeedbackFormLogos.Add(TSoftObjectPtr<UTexture2D>());
	TestFalse(TEXT("An empty slot"), FormWith(*Settings)->GetLogoRowForTesting().IsValid());

	// An image with no size, as a texture with no data reads.
	TStrongObjectPtr<UTexture2D> NoSize(NewObject<UTexture2D>(GetTransientPackage()));
	Settings->FeedbackFormLogos.Add(TSoftObjectPtr<UTexture2D>(NoSize.Get()));
	AddExpectedError(TEXT("the image has no size to draw"), EAutomationExpectedErrorFlags::Contains, 1);
	const TSharedRef<SProtokitePlaytestFormWidget> Form = FormWith(*Settings);
	TestFalse(TEXT("An image with no size"), Form->GetLogoRowForTesting().IsValid());
	TestEqual(TEXT("Nothing drawn"), Form->GetLogoBrushesForTesting().Num(), 0);
	return true;
}

/**
 * The logos are drawn above the title, left to right in their order, each at one height in its own shape, and an empty
 * slot or an image that cannot be loaded is left out.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormLogoOrderTest,
	"Protokite.Playtest.Form.Logos.TheLogosAreFirstInTheirOrderAtOneHeightAndTheirOwnShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormLogoOrderTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UTexture2D> Wide = Image(200, 100);
	TStrongObjectPtr<UTexture2D> Square = Image(64, 64);
	TStrongObjectPtr<UProtokitePlaytestSettings> Settings = NewSettings();
	Settings->FeedbackFormLogos = {
		TSoftObjectPtr<UTexture2D>(Wide.Get()),
		TSoftObjectPtr<UTexture2D>(),
		TSoftObjectPtr<UTexture2D>(FSoftObjectPath(TEXT("/Game/NoSuchFolder/NoSuchLogo.NoSuchLogo"))),
		TSoftObjectPtr<UTexture2D>(Square.Get()),
	};
	Settings->FeedbackFormLogoHeight = 30.f;
	AddExpectedError(TEXT("could not be loaded"), EAutomationExpectedErrorFlags::Contains, 1);

	const TSharedRef<SProtokitePlaytestFormWidget> Form = FormWith(*Settings);
	const TSharedPtr<SWidget> Row = Form->GetLogoRowForTesting();
	if (!TestTrue(TEXT("A row"), Row.IsValid()))
	{
		return false;
	}
	const TSharedPtr<SVerticalBox> Column = Form->GetFormColumnForTesting();
	TestTrue(TEXT("Above the title"), Column.IsValid() && Column->GetChildren()->Num() > 0 && Column->GetChildren()->GetChildAt(0) == Row);

	const TArray<const FSlateBrush*> Brushes = Form->GetLogoBrushesForTesting();
	if (!TestEqual(TEXT("The empty slot and the image that cannot be loaded are left out"), Brushes.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("First the wide one"), Drawn(Brushes[0]) == Wide.Get());
	TestTrue(TEXT("In their order"), Drawn(Brushes[1]) == Square.Get());
	TestEqual(TEXT("At the height set"), static_cast<float>(Brushes[0]->ImageSize.Y), 30.f, 0.001f);
	TestEqual(TEXT("Twice as wide as tall, like its image"), static_cast<float>(Brushes[0]->ImageSize.X), 60.f, 0.001f);
	TestEqual(TEXT("The square one square"), static_cast<float>(Brushes[1]->ImageSize.X), 30.f, 0.001f);
	TestEqual(TEXT("At the same height"), static_cast<float>(Brushes[1]->ImageSize.Y), 30.f, 0.001f);
	TestFalse(TEXT("The footer is its own setting: the mark, not a logo from the row"), Drawn(Form->GetFooterLogoBrushForTesting()) == Wide.Get());
	return true;
}

/** A logo wider than the form is drawn smaller in its own shape, never past the form's edge. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormLogoTooWideTest,
	"Protokite.Playtest.Form.Logos.ALogoWiderThanTheFormIsDrawnSmallerInItsShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormLogoTooWideTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UTexture2D> Banner = Image(2000, 100);
	TStrongObjectPtr<UProtokitePlaytestSettings> Settings = NewSettings();
	Settings->FeedbackFormLogos = { TSoftObjectPtr<UTexture2D>(Banner.Get()) };
	Settings->FeedbackFormLogoHeight = 96.f;

	const TArray<const FSlateBrush*> Brushes = FormWith(*Settings)->GetLogoBrushesForTesting();
	if (!TestEqual(TEXT("Drawn"), Brushes.Num(), 1))
	{
		return false;
	}
	const float Widest = SProtokitePlaytestFormWidget::WidestLogo();
	TestTrue(TEXT("Room for it on the form"), Widest > 0.f && Widest < 96.f * 20.f);
	TestEqual(TEXT("As wide as the form allows"), static_cast<float>(Brushes[0]->ImageSize.X), Widest, 0.001f);
	TestEqual(TEXT("And twenty times as wide as tall, like its image"), static_cast<float>(Brushes[0]->ImageSize.Y), Widest / 20.f, 0.001f);
	return true;
}

/**
 * Logos that do not fit side by side stay on one row, all drawn smaller by the same factor, each in its own shape, so the
 * row never passes the form's edges and never pushes the questions down (owner, 2026-11-04).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormLogosOneRowTest,
	"Protokite.Playtest.Form.Logos.LogosThatDoNotFitStayOnOneRowDrawnSmallerTogether",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormLogosOneRowTest::RunTest(const FString& Parameters)
{
	// Five at 96 tall are 192 wide each: 960, and 160 of margins, on a row of 564.
	TArray<TStrongObjectPtr<UTexture2D>> Images;
	TStrongObjectPtr<UProtokitePlaytestSettings> Settings = NewSettings();
	for (int32 Index = 0; Index < 5; ++Index)
	{
		Images.Add(Image(200, 100));
		Settings->FeedbackFormLogos.Add(TSoftObjectPtr<UTexture2D>(Images.Last().Get()));
	}
	Settings->FeedbackFormLogoHeight = 96.f;

	const TSharedRef<SProtokitePlaytestFormWidget> Form = FormWith(*Settings);
	const TArray<const FSlateBrush*> Brushes = Form->GetLogoBrushesForTesting();
	const TSharedPtr<SWidget> Row = Form->GetLogoRowForTesting();
	if (!TestEqual(TEXT("All five drawn"), Brushes.Num(), 5) || !TestTrue(TEXT("A row"), Row.IsValid()))
	{
		return false;
	}
	FChildren* Parts = Row->GetChildren();
	const TSharedRef<SWidget> Logos = Parts->GetChildAt(0);
	TestEqual(TEXT("One row of logos, side by side"), Logos->GetType(), FName(TEXT("SHorizontalBox")));
	TestEqual(TEXT("Holding all five"), Logos->GetChildren()->Num(), 5);

	const float Room = SProtokitePlaytestFormWidget::LogoRowWidth() - 5 * 2 * 16.f;
	const float Width = Room / 5.f;
	for (const FSlateBrush* Brush : Brushes)
	{
		TestEqual(TEXT("Each drawn smaller by the same factor, so the five fill the row"), static_cast<float>(Brush->ImageSize.X), Width, 0.01f);
		TestEqual(TEXT("And still twice as wide as tall"), static_cast<float>(Brush->ImageSize.Y), Width / 2.f, 0.01f);
	}

	// Logos that fit are left at their size.
	float Margin = 0.f;
	const TArray<FVector2D> Small = SProtokitePlaytestFormWidget::FitLogoRow({ FVector2D(60.f, 30.f), FVector2D(30.f, 30.f) }, Margin);
	TestEqual(TEXT("Two that fit keep their margins"), Margin, 16.f, 0.001f);
	TestTrue(TEXT("And their sizes"), Small.Num() == 2 && Small[0].Equals(FVector2D(60.f, 30.f)) && Small[1].Equals(FVector2D(30.f, 30.f)));

	// So many that their margins alone would fill the row: the margins take half of it, the logos the other half.
	TArray<FVector2D> Many;
	Many.Init(FVector2D(40.f, 40.f), 40);
	const TArray<FVector2D> Fitted = SProtokitePlaytestFormWidget::FitLogoRow(Many, Margin);
	const float RowWidth = SProtokitePlaytestFormWidget::LogoRowWidth();
	TestEqual(TEXT("The margins shrink to a quarter of the row's width per logo"), Margin, RowWidth / 160.f, 0.001f);
	float Total = 0.f;
	for (const FVector2D& Size : Fitted)
	{
		Total += static_cast<float>(Size.X) + 2.f * Margin;
	}
	TestEqual(TEXT("And the forty still fill the row exactly"), Total, RowWidth, 0.01f);
	TestTrue(TEXT("Each still square"), Fitted.Num() == 40 && FMath::IsNearlyEqual(Fitted[0].X, Fitted[0].Y));
	return true;
}

/**
 * The footer image is drawn first on the row of Close and Send, at the bottom left, at its height and in its own shape,
 * and with no image of the game's own it is Powered by Qwacks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormFooterLogoTest,
	"Protokite.Playtest.Form.Logos.TheFooterLogoIsFirstInTheButtonRowAtItsHeightAndOwnShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormFooterLogoTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UTexture2D> Wide = Image(160, 40);
	TStrongObjectPtr<UProtokitePlaytestSettings> Settings = NewSettings();
	Settings->FeedbackFormFooterLogo = Wide.Get();
	Settings->FeedbackFormFooterLogoHeight = 20.f;

	const TSharedRef<SProtokitePlaytestFormWidget> Form = FormWith(*Settings);
	const TSharedPtr<SHorizontalBox> Buttons = Form->GetButtonRowForTesting();
	const TSharedPtr<SWidget> Footer = Form->GetFooterLogoForTesting();
	if (!TestTrue(TEXT("A footer image on the button row"), Buttons.IsValid() && Footer.IsValid()))
	{
		return false;
	}
	FChildren* Children = Buttons->GetChildren();
	if (TestEqual(TEXT("The footer, Close and Send"), Children->Num(), 3))
	{
		TestTrue(TEXT("The footer on its left"), Children->GetChildAt(0) == Footer);
		TestTrue(TEXT("Then Close"), Children->GetChildAt(1) == Form->GetCloseButtonForTesting());
		TestTrue(TEXT("Then Send"), Children->GetChildAt(2) == Form->GetSendButtonForTesting());
	}

	const FSlateBrush* Brush = Form->GetFooterLogoBrushForTesting();
	TestTrue(TEXT("The game's own image"), Drawn(Brush) == Wide.Get());
	if (TestNotNull(TEXT("A brush"), Brush))
	{
		TestEqual(TEXT("At the height set"), static_cast<float>(Brush->ImageSize.Y), 20.f, 0.001f);
		TestEqual(TEXT("Four times as wide as tall, like its image"), static_cast<float>(Brush->ImageSize.X), 80.f, 0.001f);
	}
	TestFalse(TEXT("The top row is its own setting"), Form->GetLogoRowForTesting().IsValid());
	return true;
}

/** With nothing set the footer is Powered by Qwacks, the mark this plugin carries; the game's own image replaces it. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormPoweredByQwacksTest,
	"Protokite.Playtest.Form.Logos.TheFooterIsPoweredByQwacksUntilTheGameSetsItsOwn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormPoweredByQwacksTest::RunTest(const FString& Parameters)
{
	UTexture2D* Mark = FProtokitePlaytestFormLogos::LoadPoweredByQwacks();
	if (!TestNotNull(TEXT("The plugin carries the mark"), Mark))
	{
		return false;
	}
	TestEqual(TEXT("At its path"), FSoftObjectPath(Mark).ToString(), FString(FProtokitePlaytestFormLogos::PoweredByQwacksPath));
	TestEqual(TEXT("512 wide"), FProtokitePlaytestFormLogos::ShapeOf(*Mark).X, 512);
	TestEqual(TEXT("80 tall"), FProtokitePlaytestFormLogos::ShapeOf(*Mark).Y, 80);

	TStrongObjectPtr<UProtokitePlaytestSettings> Settings = NewSettings();
	const TSharedRef<SProtokitePlaytestFormWidget> NothingSet = FormWith(*Settings);
	TestTrue(TEXT("None set: the mark"), Drawn(NothingSet->GetFooterLogoBrushForTesting()) == Mark);
	if (const FSlateBrush* Brush = NothingSet->GetFooterLogoBrushForTesting())
	{
		TestEqual(TEXT("At the default height"), static_cast<float>(Brush->ImageSize.Y), 22.f, 0.001f);
		TestEqual(TEXT("In its own shape"), static_cast<float>(Brush->ImageSize.X), 22.f * 512.f / 80.f, 0.001f);
	}
	TestTrue(TEXT("At the bottom left"), NothingSet->GetFooterLogoForTesting().IsValid()
		&& NothingSet->GetButtonRowForTesting()->GetChildren()->GetChildAt(0) == NothingSet->GetFooterLogoForTesting());

	Settings->FeedbackFormFooterLogo = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(TEXT("/Game/NoSuchFolder/NoSuchFooter.NoSuchFooter")));
	AddExpectedError(TEXT("could not be loaded"), EAutomationExpectedErrorFlags::Contains, 1);
	TestTrue(TEXT("An image that cannot be loaded counts as none"), Drawn(FormWith(*Settings)->GetFooterLogoBrushForTesting()) == Mark);

	TStrongObjectPtr<UTexture2D> Own = Image(40, 20);
	Settings->FeedbackFormFooterLogo = Own.Get();
	const TSharedRef<SProtokitePlaytestFormWidget> OwnSet = FormWith(*Settings);
	TestTrue(TEXT("The game's own replaces it"), Drawn(OwnSet->GetFooterLogoBrushForTesting()) == Own.Get());
	TestEqual(TEXT("One image at the bottom left, not two"), OwnSet->GetButtonRowForTesting()->GetChildren()->Num(), 3);
	return true;
}

/**
 * On a narrow row the footer image is drawn smaller in its own shape and Close and Send keep their size: the image gives
 * up its room first.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormButtonsNeverShrinkTest,
	"Protokite.Playtest.Form.Logos.OnANarrowRowTheFooterIsDrawnSmallerNeverTheButtons",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormButtonsNeverShrinkTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UTexture2D> Wide = Image(160, 40);
	TStrongObjectPtr<UProtokitePlaytestSettings> Settings = NewSettings();
	Settings->FeedbackFormFooterLogo = Wide.Get();
	Settings->FeedbackFormFooterLogoHeight = 40.f;

	const TSharedRef<SProtokitePlaytestFormWidget> Form = FormWith(*Settings);
	const TSharedPtr<SHorizontalBox> Buttons = Form->GetButtonRowForTesting();
	const TSharedPtr<SWidget> Close = Form->GetCloseButtonForTesting();
	const TSharedPtr<SWidget> Send = Form->GetSendButtonForTesting();
	const TSharedPtr<SWidget> Footer = Form->GetFooterLogoForTesting();
	if (!TestTrue(TEXT("The row's parts"), Buttons.IsValid() && Close.IsValid() && Send.IsValid() && Footer.IsValid()))
	{
		return false;
	}
	Buttons->SlatePrepass(1.f);
	const float CloseWidth = Close->GetDesiredSize().X;
	const float SendWidth = Send->GetDesiredSize().X;
	TestTrue(TEXT("The buttons are measured"), CloseWidth > 0.f && SendWidth > 0.f);

	// The image wants 160 by 40; the row has 60 more than the buttons and their gaps.
	const float RowWidth = CloseWidth + SendWidth + 20.f + 60.f;
	FArrangedChildren Row(EVisibility::Visible);
	Buttons->ArrangeChildren(FGeometry::MakeRoot(FVector2D(RowWidth, 40.f), FSlateLayoutTransform()), Row);
	const FArrangedWidget* ClosePlaced = Find(Row, Close);
	const FArrangedWidget* SendPlaced = Find(Row, Send);
	const FArrangedWidget* FooterPlaced = Find(Row, Footer);
	if (!TestTrue(TEXT("All three placed"), ClosePlaced != nullptr && SendPlaced != nullptr && FooterPlaced != nullptr))
	{
		return false;
	}
	TestEqual(TEXT("Close keeps its width"), DrawnWidth(*ClosePlaced), CloseWidth, 0.01f);
	TestEqual(TEXT("Send keeps its width"), DrawnWidth(*SendPlaced), SendWidth, 0.01f);
	TestTrue(TEXT("Send at the row's right"), FMath::IsNearlyEqual(SendPlaced->Geometry.Position.X + SendWidth, RowWidth, 0.01f));
	TestTrue(TEXT("The footer at the row's left"), FMath::IsNearlyZero(FooterPlaced->Geometry.Position.X, 0.01f));

	// Inside its room, the image is drawn smaller in its own shape.
	FArrangedChildren Inside(EVisibility::Visible);
	Footer->ArrangeChildren(FooterPlaced->Geometry, Inside);
	if (!TestEqual(TEXT("The image is placed"), Inside.Num(), 1))
	{
		return false;
	}
	const float ImageWidth = DrawnWidth(Inside[0]);
	const float ImageHeight = DrawnHeight(Inside[0]);
	TestTrue(TEXT("Narrower than it wanted"), ImageWidth < 160.f - 1.f);
	TestTrue(TEXT("Within its room"), ImageWidth <= DrawnWidth(*FooterPlaced) + 0.01f);
	TestEqual(TEXT("Still four times as wide as tall"), ImageWidth / ImageHeight, 4.f, 0.01f);
	return true;
}

/**
 * The form holds its images while it is open: a brush alone does not, so an image the game let go of would be collected
 * under an open form. The control case shows it is collected once the form is gone.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormKeepsItsImagesTest,
	"Protokite.Playtest.Form.Logos.AnOpenFormKeepsItsImagesLoaded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormKeepsItsImagesTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UProtokitePlaytestSettings> Settings = NewSettings();
	TWeakObjectPtr<UTexture2D> Logo;
	TWeakObjectPtr<UTexture2D> Footer;
	TSharedPtr<SProtokitePlaytestFormWidget> Form;
	{
		TStrongObjectPtr<UTexture2D> LogoHeld = Image(200, 100);
		TStrongObjectPtr<UTexture2D> FooterHeld = Image(160, 40);
		Logo = LogoHeld.Get();
		Footer = FooterHeld.Get();
		Settings->FeedbackFormLogos = { TSoftObjectPtr<UTexture2D>(LogoHeld.Get()) };
		Settings->FeedbackFormFooterLogo = FooterHeld.Get();
		Form = FormWith(*Settings);
	}

	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestTrue(TEXT("The logo survives while the form is open"), Logo.IsValid());
	TestTrue(TEXT("So does the footer image"), Footer.IsValid());
	TestTrue(TEXT("And the form still draws them"), Form->GetLogoBrushesForTesting().Num() == 1 && Drawn(Form->GetLogoBrushesForTesting()[0]) == Logo.Get());

	Form.Reset();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestFalse(TEXT("Control: with the form gone, nothing else held the logo"), Logo.IsValid());
	TestFalse(TEXT("Nor the footer image"), Footer.IsValid());
	return true;
}

/**
 * The form Open Feedback Form shows reads the settings as it opens: a game that changes them between two forms sees the
 * second follow, and the first keeps what it opened with.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormReadsTheSettingsEachTimeTest,
	"Protokite.Playtest.Form.Logos.TheLogosFollowTheSettingsEachTimeTheFormOpens",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormReadsTheSettingsEachTimeTest::RunTest(const FString& Parameters)
{
	FScopedPlaytestSettings Playtest(true, UsableUrl);
	FScopedFormLogoSettings Logos;
	FPlaytestFixture Fixture;
	TStrongObjectPtr<UTexture2D> Wide = Image(200, 100);
	TStrongObjectPtr<UTexture2D> Square = Image(64, 64);
	TStrongObjectPtr<UTexture2D> Own = Image(40, 20);
	UTexture2D* Mark = FProtokitePlaytestFormLogos::LoadPoweredByQwacks();

	const TSharedRef<SProtokitePlaytestFormWidget> First = Fixture.Playtest->MakeFeedbackFormWidgetForTesting();
	TestFalse(TEXT("Nothing set: no row"), First->GetLogoRowForTesting().IsValid());
	TestTrue(TEXT("Nothing set: the mark"), Mark != nullptr && Drawn(First->GetFooterLogoBrushForTesting()) == Mark);

	Logos.Settings->FeedbackFormLogos = { TSoftObjectPtr<UTexture2D>(Wide.Get()), TSoftObjectPtr<UTexture2D>(Square.Get()) };
	Logos.Settings->FeedbackFormLogoHeight = 30.f;
	Logos.Settings->FeedbackFormFooterLogo = Own.Get();
	const TSharedRef<SProtokitePlaytestFormWidget> Second = Fixture.Playtest->MakeFeedbackFormWidgetForTesting();
	const TArray<const FSlateBrush*> Brushes = Second->GetLogoBrushesForTesting();
	TestEqual(TEXT("The second form draws both logos"), Brushes.Num(), 2);
	if (Brushes.Num() == 2)
	{
		TestEqual(TEXT("At the height set since"), static_cast<float>(Brushes[0]->ImageSize.Y), 30.f, 0.001f);
	}
	TestTrue(TEXT("And the game's own footer"), Drawn(Second->GetFooterLogoBrushForTesting()) == Own.Get());
	TestFalse(TEXT("The first form keeps what it opened with"), First->GetLogoRowForTesting().IsValid());

	Logos.Settings->FeedbackFormLogos.Reset();
	Logos.Settings->FeedbackFormFooterLogo.Reset();
	const TSharedRef<SProtokitePlaytestFormWidget> Third = Fixture.Playtest->MakeFeedbackFormWidgetForTesting();
	TestFalse(TEXT("Taken out again: no row"), Third->GetLogoRowForTesting().IsValid());
	TestTrue(TEXT("And the mark is back"), Drawn(Third->GetFooterLogoBrushForTesting()) == Mark);
	return true;
}

/**
 * A game's image that streams is held at full detail while the form shows it, rather than drawn blurred until its detail
 * loads; an image that never streams is left as the game set it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormImagesAtFullDetailTest,
	"Protokite.Playtest.Form.Logos.AStreamedImageIsDrawnAtFullDetail",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormImagesAtFullDetailTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UTexture2D> Streamed = Image(200, 100);
	Streamed->NeverStream = false;
	TStrongObjectPtr<UTexture2D> NeverStreamed = Image(64, 64);
	NeverStreamed->NeverStream = true;
	TestFalse(TEXT("Precondition: nothing holds the streamed one at full detail"), Streamed->ShouldMipLevelsBeForcedResident());

	TStrongObjectPtr<UProtokitePlaytestSettings> Settings = NewSettings();
	Settings->FeedbackFormLogos = { TSoftObjectPtr<UTexture2D>(Streamed.Get()), TSoftObjectPtr<UTexture2D>(NeverStreamed.Get()) };
	const TSharedRef<SProtokitePlaytestFormWidget> Form = FormWith(*Settings);

	TestTrue(TEXT("The streamed image is held at full detail"), Streamed->ShouldMipLevelsBeForcedResident());
	TestFalse(TEXT("The one that never streams is left alone"), NeverStreamed->ShouldMipLevelsBeForcedResident());
	return true;
}

/** Everything a form can draw is listed for packaging: the mark always, and each image the settings name. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestFormImagesArePackagedTest,
	"Protokite.Playtest.Form.Logos.EveryImageTheFormCanDrawIsPackaged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestFormImagesArePackagedTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UProtokitePlaytestSettings> Settings = NewSettings();
	const FSoftObjectPath Mark(FProtokitePlaytestFormLogos::PoweredByQwacksPath);
	const TArray<FSoftObjectPath> NothingSet = FProtokitePlaytestFormLogos::ImagesToPackage(*Settings);
	TestTrue(TEXT("Nothing set: the mark alone"), NothingSet.Num() == 1 && NothingSet[0] == Mark);

	Settings->FeedbackFormLogos = {
		TSoftObjectPtr<UTexture2D>(FSoftObjectPath(TEXT("/Game/Jam/Logos/Exel.Exel"))),
		TSoftObjectPtr<UTexture2D>(),
		TSoftObjectPtr<UTexture2D>(FSoftObjectPath(TEXT("/Game/Jam/Logos/Sponsor.Sponsor"))),
	};
	Settings->FeedbackFormFooterLogo = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(TEXT("/Game/Jam/Logos/Studio.Studio")));
	const TArray<FSoftObjectPath> Images = FProtokitePlaytestFormLogos::ImagesToPackage(*Settings);
	TestEqual(TEXT("The mark, both logos and the footer; the empty slot adds nothing"), Images.Num(), 4);
	TestTrue(TEXT("The mark is still packaged when the game sets its own footer"), Images.Contains(Mark));
	TestTrue(TEXT("A logo"), Images.Contains(FSoftObjectPath(TEXT("/Game/Jam/Logos/Exel.Exel"))));
	TestTrue(TEXT("The other logo"), Images.Contains(FSoftObjectPath(TEXT("/Game/Jam/Logos/Sponsor.Sponsor"))));
	TestTrue(TEXT("The footer"), Images.Contains(FSoftObjectPath(TEXT("/Game/Jam/Logos/Studio.Studio"))));
	return true;
}

#if WITH_EDITORONLY_DATA
/** The mark is imported in its own shape and smooth drawn smaller: no power-of-two stretching, with mipmaps. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProtokitePlaytestPoweredByQwacksImportTest,
	"Protokite.Playtest.Form.Logos.ThePoweredByQwacksMarkIsImportedInItsOwnShapeAndSmooth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProtokitePlaytestPoweredByQwacksImportTest::RunTest(const FString& Parameters)
{
	UTexture2D* Mark = FProtokitePlaytestFormLogos::LoadPoweredByQwacks();
	if (!TestNotNull(TEXT("The mark"), Mark))
	{
		return false;
	}
	TestTrue(TEXT("Rounded to a power of two, it is drawn stretched"), Mark->PowerOfTwoMode == ETexturePowerOfTwoSetting::None);
	TestTrue(TEXT("Smooth drawn smaller"), Mark->MipGenSettings != TMGS_NoMipmaps && Mark->MipGenSettings != TMGS_FromTextureGroup);
	TestTrue(TEXT("A UI image"), Mark->LODGroup == TEXTUREGROUP_UI);
	TestTrue(TEXT("Never streamed, so never drawn blurred while its detail loads"), Mark->NeverStream);
	TestTrue(TEXT("Kept uncompressed, its edges clean"), Mark->CompressionSettings == TC_EditorIcon);
	return true;
}
#endif

#endif // WITH_AUTOMATION_TESTS
