#pragma once
#include "save_stage.h"

// Thin wrapper around the SDK widget for showing async save progress.
// All public functions are safe to call from any thread.
namespace SaveWidget
{
    // Register the ImGui widget with the modloader (client builds only).
    void Initialize();

    // Unregister the widget on plugin shutdown.
    void Shutdown();

    // Update the displayed stage and show/hide the widget accordingly.
    // Called from the background save thread; posts visibility changes to
    // the game thread via PostToGameThread to keep the SDK call thread-safe.
    void SetStage(SaveStage stage);
}
