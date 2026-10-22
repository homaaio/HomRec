// plugin_log.h - shared log for the plugin system (logs\plugins.log).
#pragma once

#include <string>

namespace HrPluginLog {
    // Settings > Security > "Plugin logging (logs/plugins.log)" toggle.
    void SetEnabled(bool enabled);
    bool IsEnabled();

    // level: "INFO"/"WARN"/"ERROR", same convention as HrLog. plugin_id
    // may be empty for engine-level events (plugin system starting up,
    // shutting down) that aren't about any one plugin.
    void Write(const std::string &plugin_id, const char *level, const std::string &message);

    inline void Info(const std::string &plugin_id, const std::string &message)  { Write(plugin_id, "INFO", message); }
    inline void Warn(const std::string &plugin_id, const std::string &message)  { Write(plugin_id, "WARN", message); }
    inline void Error(const std::string &plugin_id, const std::string &message) { Write(plugin_id, "ERROR", message); }
}
