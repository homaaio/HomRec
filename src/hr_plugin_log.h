// hr_plugin_log.h - shared log for the plugin system (logs\plugins.log).
//
// Everything plugin-related used to either go nowhere (a plugin erroring
// out silently) or into homrec.log mixed in with the app's own events,
// which made "did any plugin do something weird" a grep-and-filter job.
// This is one line per plugin event/print, each tagged with which plugin
// it came from, in its own file. Plugins that want a log entirely of
// their own (not mixed with other plugins' output either) can open one
// via homrec.log_to() in lua_api.cpp instead.
#pragma once

#include <string>

namespace HrPluginLog {
    // level: "INFO"/"WARN"/"ERROR", same convention as HrLog. plugin_id
    // may be empty for engine-level events (plugin system starting up,
    // shutting down) that aren't about any one plugin.
    void Write(const std::string &plugin_id, const char *level, const std::string &message);

    inline void Info(const std::string &plugin_id, const std::string &message)  { Write(plugin_id, "INFO", message); }
    inline void Warn(const std::string &plugin_id, const std::string &message)  { Write(plugin_id, "WARN", message); }
    inline void Error(const std::string &plugin_id, const std::string &message) { Write(plugin_id, "ERROR", message); }
}
