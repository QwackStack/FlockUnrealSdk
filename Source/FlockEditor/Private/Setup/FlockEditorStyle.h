// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

class FSlateStyleSet;
class ISlateStyle;

/**
 * Slate brushes for the Flock editor UI.
 *
 * Exists so the plugin's own icon can be used in editor chrome. `Resources/Icon128.png` already ships
 * (the Plugins browser reads it), but Slate cannot draw a loose file — it needs a registered style set
 * pointing at a content root, which is what this is.
 */
class FFlockEditorStyle
{
public:
	/** Builds and registers the style set. Idempotent. */
	static void Register();

	/** Unregisters and destroys it. Safe when Register was never called. */
	static void Unregister();

	static const ISlateStyle& Get();

	/** The name an FSlateIcon needs to find these brushes. */
	static FName GetStyleSetName();

private:
	static TSharedRef<FSlateStyleSet> Create();

	static TSharedPtr<FSlateStyleSet> StyleInstance;
};
