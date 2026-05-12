#include "QwackSettingsDetails.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Dom/JsonObject.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IPluginManager.h"
#include "Qwack_ue_Sdk/Config/QwackSettings.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateBrush.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "FQwackSettingsDetails"

TSharedRef<IDetailCustomization> FQwackSettingsDetails::MakeInstance()
{
	return MakeShared<FQwackSettingsDetails>();
}

void FQwackSettingsDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	// Header card at the very top.
	IDetailCategoryBuilder& HeaderCat = DetailBuilder.EditCategory(
		TEXT("0Header"), FText::GetEmpty(), ECategoryPriority::Important);
	HeaderCat.SetSortOrder(0);
	HeaderCat.AddCustomRow(LOCTEXT("HeaderRow", "Flock Header"))
		.WholeRowContent()
		[
			BuildHeader()
		];

	// API Credentials — add the "API Key is required" warning row.
	IDetailCategoryBuilder& ApiCat = DetailBuilder.EditCategory(TEXT("API Credentials"));
	ApiCat.SetSortOrder(1);
	ApiCat.AddCustomRow(LOCTEXT("ApiKeyWarningRow", "ApiKey Warning"))
		.Visibility(MakeAttributeLambda(&FQwackSettingsDetails::GetApiKeyWarningVisibility))
		.WholeRowContent()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(8))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("⚠")))   // ⚠
					.ColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.75f, 0.f, 1.f)))
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(FMargin(8, 0, 0, 0))
				[
					SNew(STextBlock).Text(LOCTEXT("ApiKeyRequired", "API Key is required"))
				]
			]
		];

	// Section order matches Unity.
	DetailBuilder.EditCategory(TEXT("Optional")).SetSortOrder(2);
	DetailBuilder.EditCategory(TEXT("Analytics")).SetSortOrder(3);

	// Tools section gets the Test Connection row.
	IDetailCategoryBuilder& ToolsCat = DetailBuilder.EditCategory(TEXT("Tools"));
	ToolsCat.SetSortOrder(4);
	ToolsCat.AddCustomRow(LOCTEXT("TestConnRow", "Test Connection"))
		.WholeRowContent()
		[
			BuildTestConnectionRow()
		];
}

TSharedRef<SWidget> FQwackSettingsDetails::BuildHeader()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Qwack_ue_Sdk"));
	if (Plugin.IsValid())
	{
		const FString IconPath = Plugin->GetBaseDir() / TEXT("Resources/Icon128.png");
		if (FPaths::FileExists(IconPath))
		{
			IconBrush = MakeShared<FSlateDynamicImageBrush>(FName(*IconPath), FVector2D(64.f, 64.f));
		}
	}

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(12, 10))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0, 0, 12, 0))
			[
				IconBrush.IsValid()
					? StaticCastSharedRef<SWidget>(SNew(SImage).Image(IconBrush.Get()))
					: StaticCastSharedRef<SWidget>(SNullWidget::NullWidget)
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("FlockTitle", "Flock"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 16))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0, 2, 0, 0))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("FlockSubtitle", "Configure the Flock SDK"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SHyperlink)
					.Text(LOCTEXT("Docs", "Documentation"))
					.OnNavigate(FSimpleDelegate::CreateStatic(&FQwackSettingsDetails::OpenDocs))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(8, 0)).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("|")))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SHyperlink)
					.Text(LOCTEXT("Support", "Support"))
					.OnNavigate(FSimpleDelegate::CreateStatic(&FQwackSettingsDetails::OpenSupport))
				]
			]
		];
}

TSharedRef<SWidget> FQwackSettingsDetails::BuildTestConnectionRow()
{
	const TSharedRef<FQwackSettingsDetails> Self =
		StaticCastSharedRef<FQwackSettingsDetails>(AsShared());

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0, 4))
		[
			SNew(SButton)
			.HAlign(HAlign_Center)
			.Text(LOCTEXT("TestConnection", "Test Connection"))
			.ToolTipText(LOCTEXT("TestConnTip", "Calls GET /v1/game using the configured API Key."))
			.IsEnabled(TAttribute<bool>::Create(TAttribute<bool>::FGetter::CreateSP(Self, &FQwackSettingsDetails::IsTestEnabled)))
			.OnClicked(FOnClicked::CreateSP(Self, &FQwackSettingsDetails::OnTestConnectionClicked))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0, 4))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(Self, &FQwackSettingsDetails::GetConnStatusIcon)))
				.ColorAndOpacity(TAttribute<FSlateColor>::Create(TAttribute<FSlateColor>::FGetter::CreateSP(Self, &FQwackSettingsDetails::GetConnStatusColor)))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(FMargin(8, 0, 0, 0))
			[
				SNew(STextBlock)
				.Text(TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(Self, &FQwackSettingsDetails::GetConnStatusText)))
				.ColorAndOpacity(TAttribute<FSlateColor>::Create(TAttribute<FSlateColor>::FGetter::CreateSP(Self, &FQwackSettingsDetails::GetConnStatusColor)))
			]
		];
}

EVisibility FQwackSettingsDetails::GetApiKeyWarningVisibility()
{
	const UQwackSettings* S = GetDefault<UQwackSettings>();
	return (S && S->ApiKey.IsEmpty()) ? EVisibility::Visible : EVisibility::Collapsed;
}

void FQwackSettingsDetails::OpenDocs()
{
	if (const UQwackSettings* S = GetDefault<UQwackSettings>())
	{
		FPlatformProcess::LaunchURL(*S->DocumentationUrl, nullptr, nullptr);
	}
}

void FQwackSettingsDetails::OpenSupport()
{
	if (const UQwackSettings* S = GetDefault<UQwackSettings>())
	{
		FPlatformProcess::LaunchURL(*S->SupportUrl, nullptr, nullptr);
	}
}

FReply FQwackSettingsDetails::OnTestConnectionClicked()
{
	const UQwackSettings* S = GetDefault<UQwackSettings>();
	if (!S || S->ApiUrl.IsEmpty())
	{
		ConnState = EConnState::Failure;
		ConnStatusText = TEXT("ApiUrl is empty.");
		return FReply::Handled();
	}
	if (S->ApiKey.IsEmpty())
	{
		ConnState = EConnState::Failure;
		ConnStatusText = TEXT("ApiKey is empty.");
		return FReply::Handled();
	}

	FString Base = S->ApiUrl;
	if (Base.EndsWith(TEXT("/"))) Base.LeftChopInline(1);
	// /v1/game is the cheapest API-key-only check: no Bearer token required, and a 200
	// proves the key is valid AND bound to a game. /v1/player/auth-test looks similar
	// but actually requires a player JWT — using it from the editor returns 401.
	const FString Url = Base + TEXT("/v1/game");

	const TSharedRef<IHttpRequest> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(Url);
	Req->SetVerb(TEXT("GET"));
	Req->SetHeader(TEXT("X-Flock-API-Key"), S->ApiKey);
	if (!S->GameVersion.IsEmpty())
	{
		Req->SetHeader(TEXT("X-Game-Version-ID"), S->GameVersion);
	}
	const TSharedRef<FQwackSettingsDetails> Self =
		StaticCastSharedRef<FQwackSettingsDetails>(AsShared());
	Req->OnProcessRequestComplete().BindSP(Self, &FQwackSettingsDetails::OnGameCheckComplete);

	ConnState = EConnState::InFlight;
	ConnStatusText = FString::Printf(TEXT("Testing %s..."), *Url);
	ResolvedGameName.Reset();
	Req->ProcessRequest();
	return FReply::Handled();
}

void FQwackSettingsDetails::OnGameCheckComplete(FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bWasSuccessful)
{
	const int32 Code = Resp.IsValid() ? Resp->GetResponseCode() : 0;
	const FString Body = Resp.IsValid() ? Resp->GetContentAsString() : FString();

	if (!bWasSuccessful || Code < 200 || Code >= 300)
	{
		ConnState = EConnState::Failure;
		const FString Snippet = Body.Left(160).Replace(TEXT("\n"), TEXT(" "));
		ConnStatusText = Code == 0
			? FString(TEXT("Network error — could not reach server."))
			: FString::Printf(TEXT("Failed (HTTP %d): %s"), Code, *Snippet);
		return;
	}

	// /v1/game shape: { "result": { "id": "...", "name": "...", ... } }
	// The API key alone passing isn't enough — confirm it resolves to the same
	// game the user typed into "Game Id", otherwise this dialog gives a false
	// green light when ApiKey and GameId point at different games.
	FString ReturnedId;
	FString ReturnedName;
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
	{
		const TSharedPtr<FJsonObject>* ResultObj = nullptr;
		if (Root->TryGetObjectField(TEXT("result"), ResultObj) && ResultObj && ResultObj->IsValid())
		{
			(*ResultObj)->TryGetStringField(TEXT("id"), ReturnedId);
			(*ResultObj)->TryGetStringField(TEXT("name"), ReturnedName);
		}
	}

	const UQwackSettings* S = GetDefault<UQwackSettings>();
	const FString Configured = S ? S->GameId : FString();

	if (Configured.IsEmpty())
	{
		ConnState = EConnState::Failure;
		ConnStatusText = ReturnedName.IsEmpty()
			? FString(TEXT("API key valid, but Game Id is empty — fill it in."))
			: FString::Printf(TEXT("API key resolves to game '%s' (id: %s) — set Game Id to match."), *ReturnedName, *ReturnedId);
		return;
	}

	// Accept either the ULID or the human-friendly name. The field is local-only
	// (never sent on the wire), so this is just a "did the user configure the
	// right project" sanity check — no reason to force one form over the other.
	const bool bMatches = (Configured == ReturnedId) || (Configured == ReturnedName);
	if (!bMatches && !ReturnedId.IsEmpty())
	{
		ConnState = EConnState::Failure;
		ConnStatusText = FString::Printf(
			TEXT("Game Id mismatch — API key resolves to '%s' (id: %s), but Game Id is '%s'."),
			*ReturnedName, *ReturnedId, *Configured);
		return;
	}

	ResolvedGameName = ReturnedName;

	// Step 2: confirm the configured Game Version actually exists for this game.
	if (S->GameVersion.IsEmpty())
	{
		ConnState = EConnState::Failure;
		ConnStatusText = TEXT("Game Version is empty — fill it in.");
		return;
	}

	FString Base = S->ApiUrl;
	if (Base.EndsWith(TEXT("/"))) Base.LeftChopInline(1);
	const FString VersionUrl = Base + TEXT("/v1/game_version/by-name/")
		+ FGenericPlatformHttp::UrlEncode(S->GameVersion);

	const TSharedRef<IHttpRequest> VReq = FHttpModule::Get().CreateRequest();
	VReq->SetURL(VersionUrl);
	VReq->SetVerb(TEXT("GET"));
	VReq->SetHeader(TEXT("X-Flock-API-Key"), S->ApiKey);

	const TSharedRef<FQwackSettingsDetails> Self =
		StaticCastSharedRef<FQwackSettingsDetails>(AsShared());
	VReq->OnProcessRequestComplete().BindSP(Self, &FQwackSettingsDetails::OnGameVersionCheckComplete);

	ConnStatusText = FString::Printf(TEXT("Checking Game Version '%s'..."), *S->GameVersion);
	VReq->ProcessRequest();
}

void FQwackSettingsDetails::OnGameVersionCheckComplete(FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bWasSuccessful)
{
	const int32 Code = Resp.IsValid() ? Resp->GetResponseCode() : 0;
	const FString Body = Resp.IsValid() ? Resp->GetContentAsString() : FString();

	const UQwackSettings* S = GetDefault<UQwackSettings>();
	const FString ConfiguredVersion = S ? S->GameVersion : FString();

	if (Code == 404)
	{
		ConnState = EConnState::Failure;
		ConnStatusText = FString::Printf(
			TEXT("Game Version '%s' not found for game '%s'."),
			*ConfiguredVersion, *ResolvedGameName);
		return;
	}

	if (!bWasSuccessful || Code < 200 || Code >= 300)
	{
		ConnState = EConnState::Failure;
		const FString Snippet = Body.Left(160).Replace(TEXT("\n"), TEXT(" "));
		ConnStatusText = Code == 0
			? FString(TEXT("Network error during Game Version check."))
			: FString::Printf(TEXT("Game Version check failed (HTTP %d): %s"), Code, *Snippet);
		return;
	}

	// 200 + a parseable name confirms the version belongs to the same game.
	FString ReturnedVersionName;
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
	{
		const TSharedPtr<FJsonObject>* ResultObj = nullptr;
		if (Root->TryGetObjectField(TEXT("result"), ResultObj) && ResultObj && ResultObj->IsValid())
		{
			(*ResultObj)->TryGetStringField(TEXT("name"), ReturnedVersionName);
		}
	}

	ConnState = EConnState::Success;
	ConnStatusText = FString::Printf(
		TEXT("Connected — game '%s', version '%s'"),
		*ResolvedGameName,
		*(ReturnedVersionName.IsEmpty() ? ConfiguredVersion : ReturnedVersionName));
}

FText FQwackSettingsDetails::GetConnStatusText() const
{
	return FText::FromString(ConnStatusText);
}

FSlateColor FQwackSettingsDetails::GetConnStatusColor() const
{
	switch (ConnState)
	{
	case EConnState::Success:  return FSlateColor(FLinearColor(0.25f, 0.85f, 0.35f, 1.f));
	case EConnState::Failure:  return FSlateColor(FLinearColor(0.95f, 0.30f, 0.30f, 1.f));
	case EConnState::InFlight: return FSlateColor::UseSubduedForeground();
	default:                   return FSlateColor::UseSubduedForeground();
	}
}

FText FQwackSettingsDetails::GetConnStatusIcon() const
{
	switch (ConnState)
	{
	case EConnState::Success:  return FText::FromString(TEXT("✓"));   // ✓
	case EConnState::Failure:  return FText::FromString(TEXT("✕"));   // ✕
	case EConnState::InFlight: return FText::FromString(TEXT("…"));   // …
	default:                   return FText::GetEmpty();
	}
}

#undef LOCTEXT_NAMESPACE
