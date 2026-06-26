/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#ifdef ENABLE_SCRIPTING

    #include <cstdint>
    #include <string>
    #include <vector>

namespace OpenRCT2::Scripting::PluginStore
{
    /**
     * A plugin that is available for download from a plugin source.
     */
    struct Entry
    {
        // Unique identifier, e.g. "github:owner/repo" for entries from the built-in
        // GitHub index, or the download URL for entries from custom sources.
        std::string id;
        std::string name;
        std::string description;
        std::string author;
        // Known version, only available for entries from custom sources. Entries from
        // the GitHub index resolve their version when the latest release is fetched.
        std::string version;
        std::string websiteUrl;
        // Direct download URL for a single .js file (custom sources only).
        std::string downloadUrl;
        // GitHub repository ("owner/repo") to install release assets from.
        std::string repository;
        int32_t stars{};
        // ISO 8601 last-updated timestamp ("pushed_at" from GitHub, or "updated" from
        // a custom source). Lexicographic order matches chronological order, so it can
        // be sorted directly. May be empty for sources that don't provide it.
        std::string updatedAt;
    };

    struct InstallResult
    {
        bool success{};
        // Version that was installed on success, otherwise an error description.
        std::string message;
    };

    /**
     * Returns the user-managed plugin source URLs from the config.
     */
    std::vector<std::string> GetCustomSources();
    void AddCustomSource(const std::string& url);
    void RemoveCustomSource(const std::string& url);

    /**
     * Fetches the list of available plugins from the built-in GitHub index and all
     * custom sources. Blocking, call from a worker thread.
     * @throws std::runtime_error if no source could be fetched.
     */
    std::vector<Entry> FetchAvailablePlugins();

    /**
     * Downloads and installs the given plugin into the user's plugin directory.
     * Blocking, call from a worker thread.
     */
    InstallResult InstallPlugin(const Entry& entry);

    /**
     * Removes a plugin that was installed via the plugin store.
     */
    bool UninstallPlugin(const std::string& id);

    bool IsInstalled(const std::string& id);

    /**
     * Returns the ids of all plugins that were installed via the plugin store.
     */
    std::vector<std::string> GetInstalledIds();

    /**
     * Returns the store id for an installed plugin file, or an empty string if the
     * file was not installed via the plugin store.
     */
    std::string GetIdForPath(std::string_view pluginPath);
} // namespace OpenRCT2::Scripting::PluginStore

#endif
