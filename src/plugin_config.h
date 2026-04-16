#pragma once

#include "plugin_interface.h"

namespace BetterSavingConfig
{
	static const ConfigEntry CONFIG_ENTRIES[] = {
		// ----- General -----
		{ "General", "Enabled",        ConfigValueType::Boolean, "true", "Enable or disable the BetterSaving plugin" },
		// ----- UI -----
		{ "UI",      "ShowSaveWidget", ConfigValueType::Boolean, "true", "Show the on-screen save progress widget" },
	};

	static const ConfigSchema SCHEMA = {
		CONFIG_ENTRIES,
		sizeof(CONFIG_ENTRIES) / sizeof(ConfigEntry)
	};

	class Config
	{
	public:
		static void Initialize(IPluginSelf* self)
		{
			s_self = self;
			if (s_self)
				s_self->config->InitializeFromSchema(s_self, &SCHEMA);
		}

		static bool IsEnabled()
		{
			return s_self ? s_self->config->ReadBool(s_self, "General", "Enabled", true) : true;
		}

		static bool IsShowSaveWidget()
		{
			return s_self ? s_self->config->ReadBool(s_self, "UI", "ShowSaveWidget", true) : true;
		}

		static void SetShowSaveWidget(bool value)
		{
			if (s_self) s_self->config->WriteBool(s_self, "UI", "ShowSaveWidget", value);
		}

	private:
		static IPluginSelf* s_self;
	};
}
