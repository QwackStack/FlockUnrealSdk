// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Setup/FlockEditorStyle.h"
#include "FlockEditor.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateTypes.h"

TSharedPtr<FSlateStyleSet> FFlockEditorStyle::StyleInstance;

FName FFlockEditorStyle::GetStyleSetName()
{
	static const FName Name(TEXT("FlockEditorStyle"));
	return Name;
}

void FFlockEditorStyle::Register()
{
	if (StyleInstance.IsValid())
	{
		return;
	}

	StyleInstance = Create();
	FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
}

void FFlockEditorStyle::Unregister()
{
	if (!StyleInstance.IsValid())
	{
		return;
	}

	FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
	StyleInstance.Reset();
}

const ISlateStyle& FFlockEditorStyle::Get()
{
	check(StyleInstance.IsValid());
	return *StyleInstance;
}

TSharedRef<FSlateStyleSet> FFlockEditorStyle::Create()
{
	TSharedRef<FSlateStyleSet> Style = MakeShared<FSlateStyleSet>(GetStyleSetName());

	// Resolved through the plugin manager rather than hardcoded: the plugin can be installed under the
	// project, the engine, or a marketplace location, and only it knows which.
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Flock")))
	{
		Style->SetContentRoot(Plugin->GetBaseDir() / TEXT("Resources"));
	}
	else
	{
		// A missing plugin descriptor means the icon simply will not draw. Worth a line in the log,
		// never worth failing editor startup over.
		UE_LOG(LogFlockEditor, Warning,
			TEXT("Flock: could not locate the plugin directory; editor icons will fall back to defaults."));
	}

	// 16x16 is the tab/menu icon size. The source art is 128x128 — the same file the Plugins browser
	// shows — so there is one icon to keep in step rather than a set of per-size copies.
	const FVector2D IconSize(16.0f, 16.0f);
	Style->Set("Flock.TabIcon",
		new FSlateImageBrush(Style->RootToContentDir(TEXT("Icon128"), TEXT(".png")), IconSize));

	return Style;
}
