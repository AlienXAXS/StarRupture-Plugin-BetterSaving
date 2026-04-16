#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "save_hook.h"
#include "save_widget.h"

static IPluginSelf* g_self = nullptr;

IPluginSelf* GetSelf() { return g_self; }

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "0.1"
#endif

static PluginInfo s_pluginInfo = {
	"BetterSaving",
	MODLOADER_BUILD_TAG,
	"AlienX",
#ifdef MODLOADER_CLIENT_BUILD
	"Client-side async saving with on-screen progress widget",
#else
	"Server-side async saving",
#endif
	PLUGIN_INTERFACE_VERSION
};

// ---------------------------------------------------------------------------
// Config panel (client-only)
// ---------------------------------------------------------------------------
#ifdef MODLOADER_CLIENT_BUILD

static PanelHandle g_configPanelHandle = nullptr;

static void OnConfigPanelRender(IModLoaderImGui* imgui)
{
	imgui->SeparatorText("Save Widget");

	bool show = BetterSavingConfig::Config::IsShowSaveWidget();
	if (imgui->Checkbox("Show save progress widget", &show))
		BetterSavingConfig::Config::SetShowSaveWidget(show);

	imgui->Spacing();
	imgui->TextDisabled("Async saves run on a background thread.");
	imgui->TextDisabled("Compression and disk I/O no longer block the game.");
}

static void RegisterConfigPanel(IPluginSelf* self)
{
	if (!self->hooks->UI) return;
	static const PluginPanelDesc desc{ "BetterSaving", "BetterSaving Settings", OnConfigPanelRender };
	g_configPanelHandle = self->hooks->UI->RegisterPanel(&desc);
}

static void UnregisterConfigPanel(IPluginSelf* self)
{
	if (self->hooks->UI && g_configPanelHandle)
	{
		self->hooks->UI->UnregisterPanel(g_configPanelHandle);
		g_configPanelHandle = nullptr;
	}
}

#endif // MODLOADER_CLIENT_BUILD

// ---------------------------------------------------------------------------
// Plugin entry points
// ---------------------------------------------------------------------------

extern "C" {

	__declspec(dllexport) PluginInfo* GetPluginInfo()
	{
		return &s_pluginInfo;
	}

	__declspec(dllexport) bool PluginInit(IPluginSelf* self)
	{
		g_self = self;

		LOG_INFO("BetterSaving plugin initializing...");

		BetterSavingConfig::Config::Initialize(self);

		if (!BetterSavingConfig::Config::IsEnabled())
		{
			LOG_WARN("BetterSaving is disabled in config — skipping initialisation");
			return true;
		}

		// Register the save progress widget (client builds only — server stub is a no-op)
		SaveWidget::Initialize();

		// Install the WriteUserFile detour
		if (!SaveHook::Initialize())
		{
			LOG_ERROR("BetterSaving: Failed to install hook — plugin will not function");
			SaveWidget::Shutdown();
			return false;
		}

#ifdef MODLOADER_CLIENT_BUILD
		RegisterConfigPanel(self);
#endif

		LOG_INFO("BetterSaving plugin initialized successfully");
		return true;
	}

	__declspec(dllexport) void PluginShutdown()
	{
		LOG_INFO("BetterSaving plugin shutting down...");

		SaveHook::Shutdown();
		SaveWidget::Shutdown();

#ifdef MODLOADER_CLIENT_BUILD
		if (auto* self = g_self)
			UnregisterConfigPanel(self);
#endif

		g_self = nullptr;
	}

} // extern "C"
