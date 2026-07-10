/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#ifdef ENABLE_SCRIPTING

    #include "PluginStore.h"

    #include "../Context.h"
    #include "../Diagnostic.h"
    #include "../PlatformEnvironment.h"
    #include "../config/Config.h"
    #include "../core/File.h"
    #include "../core/FileSystem.hpp"
    #include "../core/Http.h"
    #include "../core/Json.hpp"
    #include "../core/Path.hpp"
    #include "../core/String.hpp"
    #include "../core/Zip.h"

    #include <algorithm>
    #include <mutex>
    #include <optional>
    #include <stdexcept>
    #include <system_error>

namespace OpenRCT2::Scripting::PluginStore
{
    static constexpr const utf8* kManifestFileName = u8"plugin-store.json";
    static constexpr const utf8* kStoreSubDirectory = u8"store";
    static constexpr const char* kGitHubIdPrefix = "github:";
    static constexpr const char* kGitHubIndexUrl = "https://api.github.com/search/"
                                                   "repositories?q=topic%3Aopenrct2-plugin&sort=stars&order=desc&per_page=100";

    // Downloaded plugin code is executed, so bound what we fetch from untrusted sources.
    static constexpr size_t kMaxJsonResponseBytes = 16 * 1024 * 1024;
    static constexpr size_t kMaxDownloadBytes = 32 * 1024 * 1024;
    // Total bytes buffered in memory across a single install (downloaded assets plus
    // everything extracted from zips). Caps peak memory against a release that ships
    // many assets or a single highly compressed one.
    static constexpr size_t kMaxInstallBytes = 64 * 1024 * 1024;
    static constexpr int32_t kHttpTimeoutSeconds = 30;
    // Reject pathologically nested JSON before handing it to the recursive parser.
    static constexpr int32_t kMaxJsonDepth = 100;

    // Tracks how much of a single install's byte budget remains, throwing once a
    // download or extraction would exceed it.
    class SizeBudget
    {
        size_t _remaining;

    public:
        explicit SizeBudget(size_t total)
            : _remaining(total)
        {
        }

        size_t remaining() const
        {
            return _remaining;
        }

        void consume(size_t bytes)
        {
            if (bytes > _remaining)
            {
                throw std::runtime_error("Plugin install exceeded the maximum allowed size");
            }
            _remaining -= bytes;
        }
    };

    // Thrown when an entry is definitively not installable (no release, no usable
    // assets). Distinct from generic failures like a network error, which leave
    // installability unknown rather than proving a negative.
    class NotInstallableException : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    // Guards reads and writes of the manifest file, which can happen from both the
    // UI thread and download worker threads.
    static std::mutex _manifestMutex;

    // Guards reads and writes of Config::Get().plugin.storeSources, which the fetch
    // worker thread reads while the UI thread may add/remove sources concurrently.
    static std::mutex _sourcesMutex;

    static u8string GetPluginDirectory()
    {
        auto& env = GetContext()->GetPlatformEnvironment();
        return env.GetDirectoryPath(DirBase::user, DirId::plugins);
    }

    static u8string GetManifestPath()
    {
        return Path::Combine(GetPluginDirectory(), kManifestFileName);
    }

    static json_t ReadManifest()
    {
        auto path = GetManifestPath();
        if (File::Exists(path))
        {
            try
            {
                auto manifest = Json::ReadFromFile(path);
                if (manifest.is_object())
                {
                    return manifest;
                }
            }
            catch (const std::exception& e)
            {
                LOG_WARNING("Unable to read plugin store manifest: %s", e.what());
            }
        }
        return json_t::object();
    }

    static void WriteManifest(const json_t& manifest)
    {
        Json::WriteToFile(GetManifestPath(), manifest);
    }

    static std::string GetJsonString(const json_t& obj, const char* key)
    {
        auto it = obj.find(key);
        return it != obj.end() ? Json::GetString(*it) : std::string();
    }

    template<typename T>
    static T GetJsonNumber(const json_t& obj, const char* key)
    {
        auto it = obj.find(key);
        return it != obj.end() ? Json::GetNumber<T>(*it) : T{};
    }

    static std::string SanitiseFileName(std::string_view name)
    {
        std::string result;
        result.reserve(name.size());
        for (auto c : name)
        {
            auto isSafe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'
                || c == '.';
            result.push_back(isSafe ? c : '_');
        }
        // Guard against names consisting only of dots ("." / "..")
        if (result.find_first_not_of('.') == std::string::npos)
        {
            result = "_";
        }
        return result;
    }

    static std::string NormalisePathSeparators(std::string_view path)
    {
        std::string result(path);
        for (auto& c : result)
        {
            if (c == '\\')
            {
                c = '/';
            }
        }
        return result;
    }

    // Returns true only if 'candidate' resolves to a location strictly inside 'base'
    // (not base itself, not an escape via ".."). Used to contain manifest-driven deletes.
    static bool IsStrictlyWithin(std::string_view base, std::string_view candidate)
    {
        auto b = fs::u8path(base).lexically_normal();
        auto c = fs::u8path(candidate).lexically_normal();
        auto rel = c.lexically_relative(b);
        if (rel.empty())
            return false;
        auto it = rel.begin();
        if (*it == fs::u8path(".."))
            return false; // escapes base
        if (*it == fs::u8path(".") && std::next(it) == rel.end())
            return false; // equals base
        return true;
    }

    std::vector<std::string> GetCustomSources()
    {
        std::vector<std::string> result;
        // Snapshot the config string under the lock before splitting: String::split
        // returns string_views into the buffer, so we must not let the UI thread
        // reassign storeSources (reallocating it) while the worker iterates them.
        u8string sources;
        {
            std::lock_guard guard(_sourcesMutex);
            sources = Config::Get().plugin.storeSources;
        }
        // Sources are stored space-separated; URLs never contain literal spaces
        for (auto part : String::split(sources, " "))
        {
            if (!part.empty())
            {
                result.emplace_back(part);
            }
        }
        return result;
    }

    static void SetCustomSources(const std::vector<std::string>& sources)
    {
        u8string value;
        for (const auto& source : sources)
        {
            if (!value.empty())
            {
                value.push_back(' ');
            }
            value.append(source);
        }
        {
            std::lock_guard guard(_sourcesMutex);
            Config::Get().plugin.storeSources = value;
        }
        Config::Save();
    }

    void AddCustomSource(const std::string& url)
    {
        auto sources = GetCustomSources();
        if (std::find(sources.begin(), sources.end(), url) == sources.end())
        {
            sources.push_back(url);
            SetCustomSources(sources);
        }
    }

    void RemoveCustomSource(const std::string& url)
    {
        auto sources = GetCustomSources();
        sources.erase(std::remove(sources.begin(), sources.end(), url), sources.end());
        SetCustomSources(sources);
    }

    #ifndef DISABLE_HTTP

    // Plugin code is downloaded and executed, so require an authenticated, encrypted
    // transport end to end. Rejects http:// and any non-https scheme.
    static void RequireHttps(const std::string& url)
    {
        if (!String::startsWith(url, "https://", true))
        {
            throw std::runtime_error("Refusing non-HTTPS plugin URL: " + url);
        }
    }

    // Rejects JSON whose bracket nesting exceeds kMaxJsonDepth before it reaches the
    // recursive-descent parser, preventing a stack-overflow crash from a hostile source.
    static json_t ParseJsonChecked(const std::string& body)
    {
        int32_t depth = 0;
        bool inString = false;
        bool escaped = false;
        for (char c : body)
        {
            if (inString)
            {
                if (escaped)
                    escaped = false;
                else if (c == '\\')
                    escaped = true;
                else if (c == '"')
                    inString = false;
                continue;
            }
            if (c == '"')
                inString = true;
            else if (c == '[' || c == '{')
            {
                if (++depth > kMaxJsonDepth)
                    throw std::runtime_error("JSON nesting too deep");
            }
            else if (c == ']' || c == '}')
            {
                if (depth > 0)
                    depth--;
            }
        }
        return Json::FromString(body);
    }

    /**
     * Like FetchJson, but returns std::nullopt when the server responds 404 so callers
     * can fall back rather than fail.
     */
    static std::optional<json_t> FetchJsonIfFound(const std::string& url)
    {
        RequireHttps(url);
        Http::Request request;
        request.url = url;
        request.header["Accept"] = "application/vnd.github+json, application/json";
        request.maxSize = kMaxJsonResponseBytes;
        request.timeoutSeconds = kHttpTimeoutSeconds;
        auto response = Http::Do(request);
        if (response.status == Http::Status::NotFound)
        {
            return std::nullopt;
        }
        if (response.status != Http::Status::Ok)
        {
            throw std::runtime_error("Server returned status " + std::to_string(static_cast<int32_t>(response.status)));
        }
        return ParseJsonChecked(response.body);
    }

    static json_t FetchJson(const std::string& url)
    {
        auto result = FetchJsonIfFound(url);
        if (!result.has_value())
        {
            throw std::runtime_error(
                "Server returned status " + std::to_string(static_cast<int32_t>(Http::Status::NotFound)));
        }
        return std::move(*result);
    }

    /**
     * Returns the release to install from: the latest stable release when one exists,
     * otherwise the most recent prerelease, otherwise std::nullopt. GitHub's
     * releases/latest endpoint ignores prereleases entirely and responds 404 for
     * repositories that only publish them.
     */
    static std::optional<json_t> FetchLatestRelease(const std::string& repository)
    {
        auto apiBase = "https://api.github.com/repos/" + repository;
        auto latest = FetchJsonIfFound(apiBase + "/releases/latest");
        if (latest.has_value())
        {
            return latest;
        }
        auto releases = FetchJsonIfFound(apiBase + "/releases?per_page=10");
        if (releases.has_value() && releases->is_array())
        {
            // Newest first. Drafts are not visible to unauthenticated requests, but
            // skip them defensively in case the request went through authenticated.
            for (auto& release : *releases)
            {
                if (!release.is_object())
                    continue;
                auto draft = release.find("draft");
                if (draft != release.end() && Json::GetBoolean(*draft))
                    continue;
                return std::move(release);
            }
        }
        return std::nullopt;
    }

    /**
     * Resolves .js files at the root of the repository's source tree into
     * outDownloads, pinned to an immutable ref: the given tag, or the branch head
     * commit when no ref is supplied. Pinning keeps the install reproducible — the
     * downloaded code cannot change after the user confirmed it. Returns the
     * installed version (the tag, or "git-" + the short commit hash).
     */
    static std::string GetGitHubTreeDownloads(
        const std::string& repository, const std::string& tag, std::vector<std::pair<std::string, std::string>>& outDownloads)
    {
        auto apiBase = "https://api.github.com/repos/" + repository;
        auto ref = tag;
        auto version = tag;
        if (ref.empty())
        {
            auto commits = FetchJsonIfFound(apiBase + "/commits?per_page=1");
            if (!commits.has_value() || !commits->is_array() || commits->empty())
            {
                throw NotInstallableException("'" + repository + "' has no published releases or reachable commits");
            }
            auto sha = GetJsonString((*commits)[0], "sha");
            if (sha.empty())
            {
                throw std::runtime_error("Unexpected response for commits of '" + repository + "'");
            }
            ref = sha;
            version = "git-" + sha.substr(0, 7);
        }

        // ref is a tag from remote JSON (git tags may contain '#', '%', '+', ...) or a
        // commit hash; encode it so it can't alter the query or inject path segments.
        auto contents = FetchJsonIfFound(apiBase + "/contents/?ref=" + String::urlEncode(ref));
        if (contents.has_value() && contents->is_array())
        {
            for (const auto& item : *contents)
            {
                if (!item.is_object() || GetJsonString(item, "type") != "file")
                    continue;

                auto name = GetJsonString(item, "name");
                // With ?ref set, download_url points at the pinned raw file
                auto downloadUrl = GetJsonString(item, "download_url");
                if (!downloadUrl.empty() && String::endsWith(name, ".js", true))
                {
                    outDownloads.emplace_back(SanitiseFileName(name), downloadUrl);
                }
            }
        }
        return version;
    }

    static void FetchGitHubIndex(std::vector<Entry>& outEntries)
    {
        auto root = FetchJson(kGitHubIndexUrl);
        auto it = root.find("items");
        if (it == root.end() || !it->is_array())
        {
            throw std::runtime_error("Unexpected response from plugin index");
        }
        for (const auto& item : *it)
        {
            if (!item.is_object())
                continue;

            auto fullName = GetJsonString(item, "full_name");
            if (fullName.empty())
                continue;

            Entry entry;
            entry.id = kGitHubIdPrefix + fullName;
            entry.repository = fullName;
            entry.name = GetJsonString(item, "name");
            entry.description = GetJsonString(item, "description");
            entry.websiteUrl = GetJsonString(item, "html_url");
            entry.stars = GetJsonNumber<int32_t>(item, "stargazers_count");
            entry.updatedAt = GetJsonString(item, "pushed_at");
            auto owner = item.find("owner");
            if (owner != item.end() && owner->is_object())
            {
                entry.author = GetJsonString(*owner, "login");
            }
            outEntries.push_back(std::move(entry));
        }
    }

    static void FetchCustomSource(const std::string& url, std::vector<Entry>& outEntries)
    {
        auto root = FetchJson(url);
        auto it = root.find("plugins");
        if (it == root.end() || !it->is_array())
        {
            throw std::runtime_error("Source does not contain a 'plugins' array: " + url);
        }
        for (const auto& item : *it)
        {
            if (!item.is_object())
                continue;

            Entry entry;
            entry.name = GetJsonString(item, "name");
            entry.downloadUrl = GetJsonString(item, "url");
            if (entry.name.empty() || entry.downloadUrl.empty())
                continue;

            entry.id = entry.downloadUrl;
            entry.description = GetJsonString(item, "description");
            entry.version = GetJsonString(item, "version");
            entry.websiteUrl = GetJsonString(item, "website");
            entry.updatedAt = GetJsonString(item, "updated");
            auto author = item.find("author");
            if (author != item.end())
            {
                entry.author = Json::GetString(*author);
            }
            outEntries.push_back(std::move(entry));
        }
    }

    std::vector<Entry> FetchAvailablePlugins()
    {
        std::vector<Entry> entries;
        std::string firstError;

        try
        {
            FetchGitHubIndex(entries);
        }
        catch (const std::exception& e)
        {
            LOG_WARNING("Unable to fetch plugin index: %s", e.what());
            firstError = e.what();
        }

        for (const auto& source : GetCustomSources())
        {
            try
            {
                FetchCustomSource(source, entries);
            }
            catch (const std::exception& e)
            {
                LOG_WARNING("Unable to fetch plugin source '%s': %s", source.c_str(), e.what());
                if (firstError.empty())
                {
                    firstError = e.what();
                }
            }
        }

        if (entries.empty() && !firstError.empty())
        {
            throw std::runtime_error(firstError);
        }
        return entries;
    }

    static std::string DownloadFile(const std::string& url, SizeBudget& budget)
    {
        RequireHttps(url);
        // Never fetch more than the per-file limit, nor more than the install budget
        // has left. A zero cap would read as "unlimited" downstream, so fail instead.
        auto cap = std::min(kMaxDownloadBytes, budget.remaining());
        if (cap == 0)
        {
            throw std::runtime_error("Plugin install exceeded the maximum allowed size");
        }
        Http::Request request;
        request.url = url;
        request.maxSize = cap;
        request.timeoutSeconds = kHttpTimeoutSeconds;
        auto response = Http::Do(request);
        if (response.status != Http::Status::Ok)
        {
            throw std::runtime_error(
                "Download of '" + url + "' returned status " + std::to_string(static_cast<int32_t>(response.status)));
        }
        budget.consume(response.body.size());
        return std::move(response.body);
    }

    static std::string FileNameFromUrl(const std::string& url)
    {
        auto path = url;
        auto queryPos = path.find_first_of("?#");
        if (queryPos != std::string::npos)
        {
            path = path.substr(0, queryPos);
        }
        auto fileName = SanitiseFileName(Path::GetFileName(NormalisePathSeparators(path)));
        if (!String::endsWith(fileName, ".js", true))
        {
            fileName += ".js";
        }
        return fileName;
    }

    /**
     * Extracts the .js files from a downloaded zip release asset. The zip API only
     * reads from disk, so the body is staged to a temporary file in the store
     * directory first.
     */
    static void ExtractJsFromZip(
        const std::string& assetName, const std::string& zipBody, std::vector<std::pair<std::string, std::string>>& outFiles,
        SizeBudget& budget)
    {
        auto storeDir = Path::Combine(GetPluginDirectory(), kStoreSubDirectory);
        if (!Path::CreateDirectory(storeDir))
        {
            throw std::runtime_error("Unable to create directory '" + storeDir + "'");
        }
        auto tempPath = Path::Combine(storeDir, ".staged-" + SanitiseFileName(assetName) + ".tmp");
        // Arm the cleanup before the write, so a failed/partial write is still removed
        std::shared_ptr<void> __(nullptr, [&tempPath](...) { File::Delete(tempPath); });
        File::WriteAllBytes(tempPath, zipBody.data(), zipBody.size());

        auto zip = Zip::TryOpen(tempPath, ZipAccess::read);
        if (zip == nullptr)
        {
            throw std::runtime_error("'" + assetName + "' is not a valid zip archive");
        }

        auto numFiles = zip->GetNumFiles();
        for (size_t i = 0; i < numFiles; i++)
        {
            auto path = zip->GetFileName(i);
            // Skip metadata directories added by macOS archivers
            if (String::startsWith(path, "__MACOSX", true))
                continue;

            auto fileName = SanitiseFileName(Path::GetFileName(NormalisePathSeparators(path)));
            if (!String::endsWith(fileName, ".js", true))
                continue;

            // Entries in different folders can flatten to the same name; first one wins
            auto exists = std::any_of(
                outFiles.begin(), outFiles.end(), [&fileName](const auto& f) { return f.first == fileName; });
            if (exists)
                continue;

            // The declared uncompressed size drives GetFileData's allocation and
            // inflation, and the central directory can lie (zip bombs). Refuse before
            // any memory is committed if it would blow the budget, then account for the
            // bytes actually produced as a backstop against an understated header.
            if (zip->GetFileSize(i) > budget.remaining())
            {
                throw std::runtime_error("'" + assetName + "' decompresses beyond the maximum allowed size");
            }
            auto data = zip->GetFileData(path);
            budget.consume(data.size());
            outFiles.emplace_back(std::move(fileName), std::string(reinterpret_cast<const char*>(data.data()), data.size()));
        }
    }

    /**
     * Resolves what to download for a GitHub repository, preferring the most
     * deliberate distribution the author published: loose .js release assets, then
     * .zip release assets to extract, then .js files at the root of the source tree
     * (at the release tag, or the head commit for repositories without releases).
     * Returns the version that will be installed.
     */
    static std::string GetGitHubDownloads(
        const std::string& repository, std::vector<std::pair<std::string, std::string>>& outDownloads,
        std::vector<std::pair<std::string, std::string>>& outZipDownloads)
    {
        std::string tag;
        auto release = FetchLatestRelease(repository);
        if (release.has_value())
        {
            tag = GetJsonString(*release, "tag_name");
            auto assets = release->find("assets");
            if (assets != release->end() && assets->is_array())
            {
                for (const auto& asset : *assets)
                {
                    if (!asset.is_object())
                        continue;

                    auto name = GetJsonString(asset, "name");
                    auto downloadUrl = GetJsonString(asset, "browser_download_url");
                    if (downloadUrl.empty())
                        continue;
                    if (String::endsWith(name, ".js", true))
                    {
                        outDownloads.emplace_back(SanitiseFileName(name), downloadUrl);
                    }
                    else if (String::endsWith(name, ".zip", true))
                    {
                        outZipDownloads.emplace_back(name, downloadUrl);
                    }
                }
            }
            // Loose .js assets are the canonical form; only fall back to zips without them
            if (!outDownloads.empty())
            {
                outZipDownloads.clear();
            }
            if (!outDownloads.empty() || !outZipDownloads.empty())
            {
                return tag;
            }
        }

        // No release, or a release without usable assets (e.g. tag-only releases):
        // fall back to .js files at the root of the source tree, pinned to the
        // release tag when there is one, or to the branch head commit otherwise.
        auto version = GetGitHubTreeDownloads(repository, tag, outDownloads);
        if (outDownloads.empty())
        {
            throw NotInstallableException(
                "'" + repository + "' does not ship any installable .js files in its latest release or repository root");
        }
        return version;
    }

    InstallResult InstallPlugin(const Entry& entry)
    {
        InstallResult result;
        try
        {
            std::vector<std::pair<std::string, std::string>> downloads;
            std::vector<std::pair<std::string, std::string>> zipDownloads;
            auto version = entry.version;
            if (!entry.repository.empty())
            {
                version = GetGitHubDownloads(entry.repository, downloads, zipDownloads);
            }
            else if (!entry.downloadUrl.empty())
            {
                downloads.emplace_back(FileNameFromUrl(entry.downloadUrl), entry.downloadUrl);
            }
            else
            {
                throw std::runtime_error("Plugin has no download location");
            }

            // Download everything before touching the plugin directory
            std::vector<std::pair<std::string, std::string>> files;
            SizeBudget budget(kMaxInstallBytes);
            for (const auto& [fileName, url] : downloads)
            {
                files.emplace_back(fileName, DownloadFile(url, budget));
            }
            for (const auto& [assetName, url] : zipDownloads)
            {
                auto body = DownloadFile(url, budget);
                ExtractJsFromZip(assetName, body, files, budget);
            }
            if (files.empty())
            {
                throw std::runtime_error("The downloaded release assets do not contain any .js files");
            }

            auto pluginDir = GetPluginDirectory();
            auto dirName = SanitiseFileName(entry.repository.empty() ? entry.name : entry.repository);
            auto targetDir = Path::Combine(pluginDir, kStoreSubDirectory, dirName);
            if (!Path::CreateDirectory(targetDir))
            {
                throw std::runtime_error("Unable to create directory '" + targetDir + "'");
            }

            std::vector<std::string> relativePaths;
            for (const auto& [fileName, body] : files)
            {
                File::WriteAllBytes(Path::Combine(targetDir, fileName), body.data(), body.size());
                relativePaths.push_back(std::string(kStoreSubDirectory) + "/" + dirName + "/" + fileName);
            }

            {
                std::lock_guard guard(_manifestMutex);
                auto manifest = ReadManifest();
                auto& plugins = manifest["plugins"];

                // Remove files from a previous install that are no longer part of the plugin
                auto previous = plugins.find(entry.id);
                if (previous != plugins.end() && previous->is_object())
                {
                    auto previousFiles = previous->find("files");
                    if (previousFiles != previous->end() && previousFiles->is_array())
                    {
                        for (const auto& file : *previousFiles)
                        {
                            auto relativePath = Json::GetString(file);
                            if (relativePath.empty()
                                || std::find(relativePaths.begin(), relativePaths.end(), relativePath) != relativePaths.end())
                            {
                                continue;
                            }
                            auto absolutePath = Path::Combine(pluginDir, relativePath);
                            if (IsStrictlyWithin(pluginDir, absolutePath))
                            {
                                File::Delete(absolutePath);
                            }
                        }
                    }
                }

                json_t manifestEntry = json_t::object();
                manifestEntry["name"] = entry.name;
                manifestEntry["version"] = version;
                manifestEntry["files"] = relativePaths;
                plugins[entry.id] = manifestEntry;
                WriteManifest(manifest);
            }

            result.success = true;
            result.message = version;
        }
        catch (const std::exception& e)
        {
            result.success = false;
            result.message = e.what();
        }
        return result;
    }

    InstallabilityResult CheckInstallability(const Entry& entry)
    {
        InstallabilityResult result;
        if (entry.repository.empty())
        {
            // Custom-source entries are a direct .js download; nothing to resolve
            result.installability = entry.downloadUrl.empty() ? Installability::notInstallable : Installability::installable;
            result.version = entry.version;
            if (entry.downloadUrl.empty())
            {
                result.message = "Plugin has no download location";
            }
            return result;
        }
        try
        {
            std::vector<std::pair<std::string, std::string>> downloads;
            std::vector<std::pair<std::string, std::string>> zipDownloads;
            result.version = GetGitHubDownloads(entry.repository, downloads, zipDownloads);
            result.installability = Installability::installable;
        }
        catch (const NotInstallableException& e)
        {
            // Proven negative: there is genuinely nothing to install.
            result.installability = Installability::notInstallable;
            result.message = e.what();
        }
        catch (const std::exception& e)
        {
            // Network error, rate limit, etc. — we cannot prove either way, so leave it
            // unknown rather than falsely blocking Install.
            result.installability = Installability::unknown;
            result.message = e.what();
        }
        return result;
    }

    #else

    std::vector<Entry> FetchAvailablePlugins()
    {
        throw std::runtime_error("OpenRCT2 was built without HTTP support");
    }

    InstallResult InstallPlugin(const Entry& entry)
    {
        return { false, "OpenRCT2 was built without HTTP support" };
    }

    InstallabilityResult CheckInstallability(const Entry& entry)
    {
        return { Installability::notInstallable, {}, "OpenRCT2 was built without HTTP support" };
    }

    #endif // DISABLE_HTTP

    bool UninstallPlugin(const std::string& id)
    {
        std::lock_guard guard(_manifestMutex);
        auto manifest = ReadManifest();
        auto plugins = manifest.find("plugins");
        if (plugins == manifest.end() || !plugins->is_object())
        {
            return false;
        }
        auto it = plugins->find(id);
        if (it == plugins->end() || !it->is_object())
        {
            return false;
        }

        auto pluginDir = GetPluginDirectory();
        auto files = it->find("files");
        if (files != it->end() && files->is_array())
        {
            std::vector<u8string> directories;
            for (const auto& file : *files)
            {
                auto relativePath = Json::GetString(file);
                if (relativePath.empty())
                    continue;

                auto absolutePath = Path::Combine(pluginDir, relativePath);
                // Defence in depth: never act on a manifest path that escapes the plugin
                // directory (e.g. a tampered "../../.." entry).
                if (!IsStrictlyWithin(pluginDir, absolutePath))
                {
                    LOG_WARNING("Skipping plugin-store path outside plugin directory: %s", absolutePath.c_str());
                    continue;
                }
                File::Delete(absolutePath);

                auto directory = Path::GetDirectory(absolutePath);
                if (std::find(directories.begin(), directories.end(), directory) == directories.end())
                {
                    directories.push_back(std::move(directory));
                }
            }

            // Remove the plugin's now-empty directories. is_empty is used directly
            // because the engine's wildcard matcher does not support a bare "*".
            for (const auto& directory : directories)
            {
                if (!IsStrictlyWithin(pluginDir, directory))
                    continue;

                std::error_code ec;
                auto fsPath = fs::u8path(directory);
                if (fs::is_empty(fsPath, ec) && !ec)
                {
                    fs::remove(fsPath, ec);
                }
            }
        }

        plugins->erase(id);
        WriteManifest(manifest);
        return true;
    }

    bool IsInstalled(const std::string& id)
    {
        std::lock_guard guard(_manifestMutex);
        auto manifest = ReadManifest();
        auto plugins = manifest.find("plugins");
        return plugins != manifest.end() && plugins->is_object() && plugins->contains(id);
    }

    std::vector<std::string> GetInstalledIds()
    {
        std::vector<std::string> result;
        std::lock_guard guard(_manifestMutex);
        auto manifest = ReadManifest();
        auto plugins = manifest.find("plugins");
        if (plugins != manifest.end() && plugins->is_object())
        {
            for (const auto& [id, value] : plugins->items())
            {
                result.push_back(id);
            }
        }
        return result;
    }

    std::string GetIdForPath(std::string_view pluginPath)
    {
        auto pluginDir = NormalisePathSeparators(GetPluginDirectory());
        auto normalisedPath = NormalisePathSeparators(pluginPath);

        std::lock_guard guard(_manifestMutex);
        auto manifest = ReadManifest();
        auto plugins = manifest.find("plugins");
        if (plugins == manifest.end() || !plugins->is_object())
        {
            return {};
        }
        for (const auto& [id, value] : plugins->items())
        {
            if (!value.is_object())
                continue;

            auto files = value.find("files");
            if (files == value.end() || !files->is_array())
                continue;

            for (const auto& file : *files)
            {
                auto relativePath = Json::GetString(file);
                if (!relativePath.empty() && NormalisePathSeparators(Path::Combine(pluginDir, relativePath)) == normalisedPath)
                {
                    return id;
                }
            }
        }
        return {};
    }
} // namespace OpenRCT2::Scripting::PluginStore

#endif // ENABLE_SCRIPTING
