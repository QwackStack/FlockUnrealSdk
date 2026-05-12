#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"
#include "Interfaces/IHttpRequest.h"
#include "Layout/Visibility.h"
#include "Styling/SlateColor.h"

class IDetailLayoutBuilder;
struct FSlateBrush;
class SWidget;

class FQwackSettingsDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	TSharedRef<SWidget> BuildHeader();
	TSharedRef<SWidget> BuildTestConnectionRow();

	static EVisibility GetApiKeyWarningVisibility();
	static void OpenDocs();
	static void OpenSupport();

	FReply OnTestConnectionClicked();
	void OnGameCheckComplete(FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bWasSuccessful);
	void OnGameVersionCheckComplete(FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bWasSuccessful);

	enum class EConnState : uint8 { Idle, InFlight, Success, Failure };
	EConnState ConnState = EConnState::Idle;
	FString ConnStatusText;

	// Carried from step 1 to step 2 so the final success message can reference the game name.
	FString ResolvedGameName;

	FText GetConnStatusText() const;
	FSlateColor GetConnStatusColor() const;
	FText GetConnStatusIcon() const;
	bool IsTestEnabled() const { return ConnState != EConnState::InFlight; }

	TSharedPtr<FSlateBrush> IconBrush;
};
