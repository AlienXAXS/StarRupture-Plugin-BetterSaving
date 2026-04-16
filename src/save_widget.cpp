#include "save_widget.h"
#include "plugin_helpers.h"
#include "plugin_config.h"

#include <atomic>

// ---------------------------------------------------------------------------
// Client-only implementation — server builds compile empty stubs at the bottom
// ---------------------------------------------------------------------------
#ifdef MODLOADER_CLIENT_BUILD

static WidgetHandle            g_widgetHandle = nullptr;
static std::atomic<SaveStage>  g_stage{ SaveStage::Idle };

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char* StageNameStr(SaveStage stage)
{
    switch (stage)
    {
    case SaveStage::Idle:        return "Idle";
    case SaveStage::Queued:      return "Queued";
    case SaveStage::Compressing: return "Compressing";
    case SaveStage::Writing:     return "Writing";
    case SaveStage::Done:        return "Done";
    case SaveStage::Failed:      return "Failed";
    default:                     return "Unknown";
    }
}

static const char* StageLabelText(SaveStage stage)
{
    switch (stage)
    {
    case SaveStage::Queued:      return "Save: Queued...         ";
    case SaveStage::Compressing: return "Save: Compressing...    ";
    case SaveStage::Writing:     return "Save: Writing to disk...";
    case SaveStage::Done:        return "Save: Complete!         ";
    case SaveStage::Failed:      return "Save: FAILED!           ";
    default:                     return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Widget render callback — called on the game thread by the modloader each frame
// ---------------------------------------------------------------------------

static void OnWidgetRender(IModLoaderImGui* imgui)
{
    SaveStage stage = g_stage.load(std::memory_order_relaxed);

    const char* label = StageLabelText(stage);
    if (!label)
        return;

    if (stage == SaveStage::Done)
        imgui->TextColored(0.20f, 1.00f, 0.20f, 1.0f, label);
    else if (stage == SaveStage::Failed)
        imgui->TextColored(1.00f, 0.20f, 0.20f, 1.0f, label);
    else
        imgui->TextColored(1.00f, 0.85f, 0.10f, 1.0f, label);  // amber while in-progress
}

// ---------------------------------------------------------------------------
// PostToGameThread helper for visibility changes
//
// NOTE: The SDK's widget system is managed on the game thread. We post
// SetWidgetVisible calls here so background threads don't touch it directly.
// ---------------------------------------------------------------------------

struct VisibilityRequest
{
    WidgetHandle handle;
    bool         visible;
};

static void GameThread_SetVisibility(void* ctx)
{
    auto* req = static_cast<VisibilityRequest*>(ctx);

    LOG_DEBUG("BetterSaving: [game thread] SetWidgetVisible handle=0x%p  visible=%d",
              req->handle, static_cast<int>(req->visible));

    if (auto* self = GetSelf())
    {
        if (self->hooks->UI && req->handle)
            self->hooks->UI->SetWidgetVisible(req->handle, req->visible);
        else
            LOG_WARN("BetterSaving: GameThread_SetVisibility — UI or handle null (UI=0x%p handle=0x%p)",
                     self->hooks->UI, req->handle);
    }
    else
    {
        LOG_WARN("BetterSaving: GameThread_SetVisibility — GetSelf() null, skipping");
    }

    delete req;
}

static void PostVisibility(bool visible)
{
    LOG_TRACE("BetterSaving: PostVisibility(%d) — handle=0x%p", static_cast<int>(visible), g_widgetHandle);

    auto* self = GetSelf();
    if (!self || !self->hooks->Engine || !g_widgetHandle)
    {
        LOG_DEBUG("BetterSaving: PostVisibility skipped (self=0x%p engine=0x%p handle=0x%p)",
                  self,
                  self ? self->hooks->Engine : nullptr,
                  g_widgetHandle);
        return;
    }

    auto* req = new VisibilityRequest{ g_widgetHandle, visible };
    LOG_TRACE("BetterSaving: Posting visibility request to game thread (req=0x%p)", req);
    self->hooks->Engine->PostToGameThread(GameThread_SetVisibility, req);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

namespace SaveWidget
{

void Initialize()
{
    LOG_DEBUG("BetterSaving: SaveWidget::Initialize");

    auto* self = GetSelf();
    if (!self)
    {
        LOG_ERROR("BetterSaving: SaveWidget::Initialize — GetSelf() null");
        return;
    }
    if (!self->hooks->UI)
    {
        LOG_WARN("BetterSaving: SaveWidget::Initialize — UI interface null (server build? skipping)");
        return;
    }

    // NOTE: The SDK's PluginWidgetDesc does not expose ImGui window flags or a
    // fixed position, so we cannot enforce bottom-right placement or prevent
    // dragging through this interface — the modloader controls the window.
    static const PluginWidgetDesc desc{ "BetterSaving Status", OnWidgetRender };
    LOG_DEBUG("BetterSaving: Calling RegisterWidget...");
    g_widgetHandle = self->hooks->UI->RegisterWidget(&desc);

    if (g_widgetHandle)
    {
        LOG_DEBUG("BetterSaving: Widget registered — handle=0x%p, setting initially hidden", g_widgetHandle);
        self->hooks->UI->SetWidgetVisible(g_widgetHandle, false);
        LOG_INFO("BetterSaving: Save status widget registered successfully");
    }
    else
    {
        LOG_WARN("BetterSaving: RegisterWidget returned null — status widget unavailable");
    }
}

void Shutdown()
{
    LOG_DEBUG("BetterSaving: SaveWidget::Shutdown — handle=0x%p", g_widgetHandle);

    auto* self = GetSelf();
    if (self && self->hooks->UI && g_widgetHandle)
    {
        LOG_DEBUG("BetterSaving: Unregistering widget handle=0x%p", g_widgetHandle);
        self->hooks->UI->UnregisterWidget(g_widgetHandle);
        g_widgetHandle = nullptr;
        LOG_DEBUG("BetterSaving: Widget unregistered");
    }
    else
    {
        LOG_DEBUG("BetterSaving: SaveWidget::Shutdown — nothing to unregister "
                  "(self=0x%p  UI=0x%p  handle=0x%p)",
                  self, self ? self->hooks->UI : nullptr, g_widgetHandle);
    }
}

void SetStage(SaveStage stage)
{
    const SaveStage prev = g_stage.exchange(stage, std::memory_order_relaxed);

    if (prev != stage)
        LOG_DEBUG("BetterSaving: SaveStage %s → %s", StageNameStr(prev), StageNameStr(stage));
    else
        LOG_TRACE("BetterSaving: SaveStage unchanged (%s)", StageNameStr(stage));

    const bool shouldShow = (stage != SaveStage::Idle)
                          && BetterSavingConfig::Config::IsShowSaveWidget();

    LOG_TRACE("BetterSaving: SetStage — shouldShow=%d (widgetEnabled=%d)",
              static_cast<int>(shouldShow),
              static_cast<int>(BetterSavingConfig::Config::IsShowSaveWidget()));

    PostVisibility(shouldShow);
}

} // namespace SaveWidget

#else // !MODLOADER_CLIENT_BUILD

// Server builds — no UI available, everything is a no-op.
namespace SaveWidget
{
void Initialize()           {}
void Shutdown()             {}
void SetStage(SaveStage)    {}
} // namespace SaveWidget

#endif // MODLOADER_CLIENT_BUILD
