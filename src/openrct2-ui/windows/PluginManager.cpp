/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#ifdef ENABLE_SCRIPTING

    #include "../UiStringIds.h"

    #include <algorithm>
    #include <chrono>
    #include <future>
    #include <openrct2-ui/interface/Widget.h>
    #include <openrct2-ui/windows/Windows.h>
    #include <openrct2/Context.h>
    #include <openrct2/PlatformEnvironment.h>
    #include <openrct2/SpriteIds.h>
    #include <openrct2/core/File.h>
    #include <openrct2/core/Path.hpp>
    #include <openrct2/core/String.hpp>
    #include <openrct2/drawing/ColourMap.h>
    #include <openrct2/drawing/Drawing.String.h>
    #include <openrct2/drawing/Drawing.h>
    #include <openrct2/drawing/Rectangle.h>
    #include <openrct2/drawing/Text.h>
    #include <openrct2/localisation/Formatter.h>
    #include <openrct2/localisation/Language.h>
    #include <openrct2/localisation/StringIds.h>
    #include <openrct2/scripting/PluginStore.h>
    #include <openrct2/scripting/ScriptEngine.h>
    #include <openrct2/ui/UiContext.h>
    #include <openrct2/ui/WindowManager.h>
    #include <set>
    #include <string>
    #include <vector>

using namespace OpenRCT2::Drawing;

namespace OpenRCT2::Ui::Windows
{
    using namespace OpenRCT2::Scripting;

    static constexpr ScreenSize kMinimumWindowSize = { 500, 300 };
    static constexpr ScreenSize kMaximumWindowSize = { 1200, 800 };
    static constexpr int32_t kTabHeight = 50;
    static constexpr int32_t kItemHeight = (3 + 9 + 3);
    static constexpr int32_t kMaxSourceUrlLength = 1024;

    enum
    {
        PAGE_INSTALLED,
        PAGE_AVAILABLE,
        PAGE_SOURCES,
        PAGE_COUNT,
    };

    enum WindowPluginManagerWidgetIdx : WidgetIndex
    {
        WIDX_BACKGROUND,
        WIDX_TITLE,
        WIDX_CLOSE,
        WIDX_PAGE_BACKGROUND,
        WIDX_TAB_INSTALLED,
        WIDX_TAB_AVAILABLE,
        WIDX_TAB_SOURCES,
        WIDX_LIST,

        // Installed page
        WIDX_OPEN_FOLDER,
        WIDX_UNINSTALL,

        // Available page
        WIDX_REFRESH = WIDX_OPEN_FOLDER,
        WIDX_INSTALL,
        WIDX_OPEN_WEBPAGE,

        // Sources page
        WIDX_ADD_SOURCE = WIDX_OPEN_FOLDER,
        WIDX_REMOVE_SOURCE,
    };

    // clang-format off
    static constexpr auto kMainWidgets = makeWidgets(
        makeWindowShim (STR_PLUGIN_MANAGER_TITLE, kMinimumWindowSize),
        makeWidget     ({  0, kTabHeight }, { kMinimumWindowSize.width, kMinimumWindowSize.height - kTabHeight }, WidgetType::frame,  WindowColour::secondary),
        makeRemapWidget({  3, 17         }, { 91, kTabHeight - 16 },                                              WidgetType::tab,    WindowColour::secondary, SPR_TAB_LARGE),
        makeRemapWidget({ 94, 17         }, { 91, kTabHeight - 16 },                                              WidgetType::tab,    WindowColour::secondary, SPR_TAB_LARGE),
        makeRemapWidget({185, 17         }, { 91, kTabHeight - 16 },                                              WidgetType::tab,    WindowColour::secondary, SPR_TAB_LARGE),
        makeWidget     ({  6, kTabHeight + 6 }, { 488, 200 },                                                     WidgetType::scroll, WindowColour::secondary)
    );

    static constexpr auto kInstalledPageWidgets = makeWidgets(
        kMainWidgets,
        makeWidget({  6, 262 }, { 130, 14 }, WidgetType::button, WindowColour::secondary, STR_PLUGIN_MANAGER_OPEN_FOLDER),
        makeWidget({140, 262 }, { 130, 14 }, WidgetType::button, WindowColour::secondary, STR_PLUGIN_MANAGER_UNINSTALL)
    );

    static constexpr auto kAvailablePageWidgets = makeWidgets(
        kMainWidgets,
        makeWidget({  6, 262 }, { 130, 14 }, WidgetType::button, WindowColour::secondary, STR_PLUGIN_MANAGER_REFRESH),
        makeWidget({140, 262 }, { 130, 14 }, WidgetType::button, WindowColour::secondary, STR_PLUGIN_MANAGER_INSTALL),
        makeWidget({274, 262 }, { 130, 14 }, WidgetType::button, WindowColour::secondary, STR_PLUGIN_MANAGER_OPEN_WEBPAGE)
    );

    static constexpr auto kSourcesPageWidgets = makeWidgets(
        kMainWidgets,
        makeWidget({  6, 262 }, { 130, 14 }, WidgetType::button, WindowColour::secondary, STR_PLUGIN_MANAGER_ADD_SOURCE),
        makeWidget({140, 262 }, { 130, 14 }, WidgetType::button, WindowColour::secondary, STR_PLUGIN_MANAGER_REMOVE_SOURCE)
    );
    // clang-format on

    static constexpr std::span<const Widget> kPageWidgets[] = {
        kInstalledPageWidgets,
        kAvailablePageWidgets,
        kSourcesPageWidgets,
    };

    static constexpr StringId kTabNames[] = {
        STR_PLUGIN_MANAGER_TAB_INSTALLED,
        STR_PLUGIN_MANAGER_TAB_AVAILABLE,
        STR_PLUGIN_MANAGER_TAB_SOURCES,
    };

    class PluginManagerWindow final : public Window
    {
    private:
        struct InstalledItem
        {
            std::string name;
            std::string version;
            std::string authors;
            std::string path;
        };

        std::vector<InstalledItem> _installed;
        std::vector<PluginStore::Entry> _available;
        std::set<std::string> _installedStoreIds;
        std::vector<std::string> _customSources;

        std::future<std::pair<std::vector<PluginStore::Entry>, std::string>> _fetchFuture;
        std::future<PluginStore::InstallResult> _installFuture;
        bool _hasFetched = false;
        int32_t _selectedItem = -1;

        StringId _availableStatus = kStringIdNone;
        std::string _statusDetail;
        std::string _lastError;

    public:
    #pragma region Window Override Events

        void onOpen() override
        {
            refreshInstalled();
            refreshSources();
            setPage(PAGE_INSTALLED);
        }

        void onClose() override
        {
            // Blocks until any in-flight download has finished
            _fetchFuture = {};
            _installFuture = {};
        }

        void onMouseUp(WidgetIndex widgetIndex) override
        {
            switch (widgetIndex)
            {
                case WIDX_CLOSE:
                    close();
                    break;
                case WIDX_TAB_INSTALLED:
                case WIDX_TAB_AVAILABLE:
                case WIDX_TAB_SOURCES:
                    setPage(widgetIndex - WIDX_TAB_INSTALLED);
                    break;
                default:
                    switch (page)
                    {
                        case PAGE_INSTALLED:
                            onInstalledPageMouseUp(widgetIndex);
                            break;
                        case PAGE_AVAILABLE:
                            onAvailablePageMouseUp(widgetIndex);
                            break;
                        case PAGE_SOURCES:
                            onSourcesPageMouseUp(widgetIndex);
                            break;
                    }
                    break;
            }
        }

        void onResize() override
        {
            WindowSetResize(*this, kMinimumWindowSize, kMaximumWindowSize);
        }

        void onUpdate() override
        {
            checkFetchComplete();
            checkInstallComplete();
        }

        void onTextInput(WidgetIndex widgetIndex, std::string_view text) override
        {
            if (page != PAGE_SOURCES || widgetIndex != WIDX_ADD_SOURCE || text.empty())
                return;

            auto url = String::trim(std::string(text));
            if (String::startsWith(url, "http://", true) || String::startsWith(url, "https://", true))
            {
                PluginStore::AddCustomSource(url);
                refreshSources();
                invalidate();
            }
        }

        ScreenSize onScrollGetSize(int32_t scrollIndex) override
        {
            return { 0, numListItems * kItemHeight };
        }

        void onScrollMouseDown(int32_t scrollIndex, const ScreenCoordsXY& screenCoords) override
        {
            auto itemIndex = screenCoords.y / kItemHeight;
            _selectedItem = (itemIndex >= 0 && static_cast<size_t>(itemIndex) < currentListSize()) ? itemIndex : -1;
            invalidate();
        }

        void onPrepareDraw() override
        {
            constexpr int32_t margin = 6;
            constexpr int32_t buttonHeight = 14;
            constexpr int32_t statusHeight = 13;

            widgets[WIDX_PAGE_BACKGROUND].right = width - 1;
            widgets[WIDX_PAGE_BACKGROUND].bottom = height - 1;

            int32_t buttonTop = height - margin - statusHeight - buttonHeight;
            auto layoutButton = [&](WidgetIndex widgetIndex, int32_t left, int32_t buttonWidth) {
                widgets[widgetIndex].left = left;
                widgets[widgetIndex].right = left + buttonWidth - 1;
                widgets[widgetIndex].top = buttonTop;
                widgets[widgetIndex].bottom = buttonTop + buttonHeight - 1;
                return widgets[widgetIndex].right + 1 + 4;
            };

            widgets[WIDX_LIST].left = margin;
            widgets[WIDX_LIST].right = width - margin;
            widgets[WIDX_LIST].top = kTabHeight + margin;
            widgets[WIDX_LIST].bottom = buttonTop - margin;

            switch (page)
            {
                case PAGE_INSTALLED:
                {
                    auto x = layoutButton(WIDX_OPEN_FOLDER, margin, 130);
                    layoutButton(WIDX_UNINSTALL, x, 130);
                    widgetSetDisabled(*this, WIDX_UNINSTALL, !isValidSelection());
                    numListItems = static_cast<uint16_t>(_installed.size());
                    break;
                }
                case PAGE_AVAILABLE:
                {
                    auto x = layoutButton(WIDX_REFRESH, margin, 130);
                    x = layoutButton(WIDX_INSTALL, x, 130);
                    layoutButton(WIDX_OPEN_WEBPAGE, x, 130);

                    auto hasSelection = isValidSelection();
                    auto installing = _installFuture.valid();
                    widgetSetDisabled(*this, WIDX_REFRESH, _fetchFuture.valid() || installing);
                    widgetSetDisabled(*this, WIDX_INSTALL, !hasSelection || installing);
                    widgetSetDisabled(*this, WIDX_OPEN_WEBPAGE, !hasSelection);
                    widgets[WIDX_INSTALL].text = hasSelection && isStoreInstalled(_available[_selectedItem].id)
                        ? STR_PLUGIN_MANAGER_REINSTALL
                        : STR_PLUGIN_MANAGER_INSTALL;
                    numListItems = static_cast<uint16_t>(_available.size());
                    break;
                }
                case PAGE_SOURCES:
                {
                    auto x = layoutButton(WIDX_ADD_SOURCE, margin, 130);
                    layoutButton(WIDX_REMOVE_SOURCE, x, 130);
                    // The first entry is the built-in source, which cannot be removed
                    widgetSetDisabled(*this, WIDX_REMOVE_SOURCE, _selectedItem < 1);
                    numListItems = static_cast<uint16_t>(1 + _customSources.size());
                    break;
                }
            }
        }

        void onDraw(RenderTarget& rt) override
        {
            drawWidgets(rt);
            drawTabNames(rt);
            drawStatusText(rt);
        }

        void onScrollDraw(int32_t scrollIndex, RenderTarget& rt) override
        {
            auto paletteIndex = getColourMap(colours[1].colour).midLight;
            GfxClear(rt, paletteIndex);

            switch (page)
            {
                case PAGE_INSTALLED:
                    drawInstalledList(rt);
                    break;
                case PAGE_AVAILABLE:
                    drawAvailableList(rt);
                    break;
                case PAGE_SOURCES:
                    drawSourcesList(rt);
                    break;
            }
        }

    #pragma endregion

    private:
        void setPage(int32_t newPage)
        {
            // Skip setting the page if we are already on it, unless we are initialising the window
            if (page == newPage && !widgets.empty())
                return;

            page = newPage;
            _selectedItem = -1;

            setWidgets(kPageWidgets[newPage]);
    #ifdef DISABLE_HTTP
            widgets[WIDX_TAB_AVAILABLE].type = WidgetType::empty;
            widgets[WIDX_TAB_SOURCES].type = WidgetType::empty;
    #endif
            initScrollWidgets();
            WindowSetResize(*this, kMinimumWindowSize, kMaximumWindowSize);
            widgetSetPressedExclusive(
                *this, { WIDX_TAB_INSTALLED, WIDX_TAB_AVAILABLE, WIDX_TAB_SOURCES },
                static_cast<WidgetIndex>(WIDX_TAB_INSTALLED + newPage));

            if (newPage == PAGE_AVAILABLE && !_hasFetched)
            {
                fetchAvailableBegin();
            }
            invalidate();
        }

        size_t currentListSize() const
        {
            switch (page)
            {
                case PAGE_INSTALLED:
                    return _installed.size();
                case PAGE_AVAILABLE:
                    return _available.size();
                case PAGE_SOURCES:
                    return 1 + _customSources.size();
                default:
                    return 0;
            }
        }

        bool isValidSelection() const
        {
            return _selectedItem >= 0 && static_cast<size_t>(_selectedItem) < currentListSize();
        }

        bool isStoreInstalled(const std::string& id) const
        {
            return _installedStoreIds.find(id) != _installedStoreIds.end();
        }

        void refreshInstalled()
        {
            _installed.clear();
            auto& scriptEngine = GetContext()->GetScriptEngine();
            for (const auto& plugin : scriptEngine.GetPlugins())
            {
                if (!plugin->HasPath())
                    continue;

                const auto& metadata = plugin->GetMetadata();
                InstalledItem item;
                item.path = plugin->GetPath();
                item.name = metadata.Name.empty() ? Path::GetFileName(item.path) : metadata.Name;
                item.version = metadata.Version;
                for (const auto& author : metadata.Authors)
                {
                    if (!item.authors.empty())
                    {
                        item.authors += ", ";
                    }
                    item.authors += author;
                }
                _installed.push_back(std::move(item));
            }
            std::sort(_installed.begin(), _installed.end(), [](const InstalledItem& a, const InstalledItem& b) {
                return String::compare(a.name, b.name, true) < 0;
            });

            _installedStoreIds.clear();
            for (auto& id : PluginStore::GetInstalledIds())
            {
                _installedStoreIds.insert(std::move(id));
            }
        }

        void refreshSources()
        {
            _customSources = PluginStore::GetCustomSources();
        }

        void onInstalledPageMouseUp(WidgetIndex widgetIndex)
        {
            switch (widgetIndex)
            {
                case WIDX_OPEN_FOLDER:
                {
                    auto context = GetContext();
                    auto pluginDirectory = context->GetPlatformEnvironment().GetDirectoryPath(DirBase::user, DirId::plugins);
                    Path::CreateDirectory(pluginDirectory);
                    context->GetUiContext().OpenFolder(pluginDirectory);
                    break;
                }
                case WIDX_UNINSTALL:
                {
                    if (!isValidSelection())
                        break;

                    const auto& item = _installed[_selectedItem];
                    auto storeId = PluginStore::GetIdForPath(item.path);
                    if (!storeId.empty())
                    {
                        PluginStore::UninstallPlugin(storeId);
                    }
                    else
                    {
                        File::Delete(item.path);
                    }
                    GetContext()->GetScriptEngine().SynchronisePluginsWithDisk();
                    refreshInstalled();
                    _selectedItem = -1;
                    invalidate();
                    break;
                }
            }
        }

        void onAvailablePageMouseUp(WidgetIndex widgetIndex)
        {
            switch (widgetIndex)
            {
                case WIDX_REFRESH:
                    fetchAvailableBegin();
                    break;
                case WIDX_INSTALL:
                    installBegin();
                    break;
                case WIDX_OPEN_WEBPAGE:
                {
                    if (!isValidSelection())
                        break;

                    const auto& entry = _available[_selectedItem];
                    const auto& url = entry.websiteUrl.empty() ? entry.downloadUrl : entry.websiteUrl;
                    if (!url.empty())
                    {
                        GetContext()->GetUiContext().OpenURL(url);
                    }
                    break;
                }
            }
        }

        void onSourcesPageMouseUp(WidgetIndex widgetIndex)
        {
            switch (widgetIndex)
            {
                case WIDX_ADD_SOURCE:
                    textInputOpen(
                        WIDX_ADD_SOURCE, STR_PLUGIN_MANAGER_ADD_SOURCE, STR_PLUGIN_MANAGER_ENTER_SOURCE_URL, {}, kStringIdNone,
                        0, kMaxSourceUrlLength);
                    break;
                case WIDX_REMOVE_SOURCE:
                {
                    auto sourceIndex = _selectedItem - 1;
                    if (sourceIndex >= 0 && sourceIndex < static_cast<int32_t>(_customSources.size()))
                    {
                        PluginStore::RemoveCustomSource(_customSources[sourceIndex]);
                        refreshSources();
                        _selectedItem = -1;
                        invalidate();
                    }
                    break;
                }
            }
        }

        void fetchAvailableBegin()
        {
            if (_fetchFuture.valid())
                return;

            _availableStatus = STR_PLUGIN_MANAGER_FETCHING;
            _fetchFuture = std::async(std::launch::async, [] {
                std::pair<std::vector<PluginStore::Entry>, std::string> result;
                try
                {
                    result.first = PluginStore::FetchAvailablePlugins();
                }
                catch (const std::exception& e)
                {
                    result.second = e.what();
                }
                return result;
            });
            invalidate();
        }

        void checkFetchComplete()
        {
            if (!_fetchFuture.valid() || _fetchFuture.wait_for(std::chrono::seconds::zero()) != std::future_status::ready)
                return;

            auto [entries, error] = _fetchFuture.get();
            _fetchFuture = {};
            _hasFetched = true;
            if (error.empty())
            {
                _available = std::move(entries);
                _availableStatus = STR_PLUGIN_MANAGER_X_AVAILABLE;
            }
            else
            {
                _statusDetail = error;
                _availableStatus = STR_PLUGIN_MANAGER_FETCH_FAILED;
            }
            _selectedItem = -1;
            invalidate();
        }

        void installBegin()
        {
            if (_installFuture.valid() || !isValidSelection())
                return;

            auto entry = _available[_selectedItem];
            _statusDetail = entry.name;
            _availableStatus = STR_PLUGIN_MANAGER_INSTALLING;
            _installFuture = std::async(std::launch::async, [entry] { return PluginStore::InstallPlugin(entry); });
            invalidate();
        }

        void checkInstallComplete()
        {
            if (!_installFuture.valid() || _installFuture.wait_for(std::chrono::seconds::zero()) != std::future_status::ready)
                return;

            auto result = _installFuture.get();
            _installFuture = {};
            if (result.success)
            {
                GetContext()->GetScriptEngine().SynchronisePluginsWithDisk();
                refreshInstalled();
                if (!result.message.empty())
                {
                    _statusDetail += " " + result.message;
                }
                _availableStatus = STR_PLUGIN_MANAGER_INSTALL_SUCCESS;
            }
            else
            {
                _availableStatus = STR_PLUGIN_MANAGER_X_AVAILABLE;
                _lastError = result.message;
                auto ft = Formatter();
                ft.Add<const char*>(_lastError.c_str());
                ContextShowError(STR_PLUGIN_MANAGER_INSTALL_FAILED_TITLE, STR_STRING, ft);
            }
            invalidate();
        }

        void drawTabNames(RenderTarget& rt)
        {
            for (int32_t i = 0; i < PAGE_COUNT; i++)
            {
                const auto& tabWidget = widgets[WIDX_TAB_INSTALLED + i];
                if (tabWidget.type == WidgetType::empty)
                    continue;

                auto ft = Formatter();
                ft.Add<StringId>(kTabNames[i]);
                drawTextWrapped(
                    rt, { windowPos.x + tabWidget.left + 45, windowPos.y + tabWidget.midY() - 3 }, 87,
                    STR_WINDOW_COLOUR_2_STRINGID, ft, { Drawing::Colour::lightWater, TextAlignment::centre });
            }
        }

        void drawStatusText(RenderTarget& rt)
        {
            auto coords = windowPos + ScreenCoordsXY{ 8, height - 15 };
            auto ft = Formatter();
            switch (page)
            {
                case PAGE_INSTALLED:
                    ft.Add<uint16_t>(static_cast<uint16_t>(_installed.size()));
                    drawText(rt, coords, STR_PLUGIN_MANAGER_X_INSTALLED, ft, { colours[1] });
                    break;
                case PAGE_AVAILABLE:
                    if (_availableStatus == STR_PLUGIN_MANAGER_X_AVAILABLE)
                    {
                        ft.Add<uint16_t>(static_cast<uint16_t>(_available.size()));
                    }
                    else
                    {
                        ft.Add<const char*>(_statusDetail.c_str());
                    }
                    if (_availableStatus != kStringIdNone)
                    {
                        drawText(rt, coords, _availableStatus, ft, { colours[1] });
                    }
                    break;
                case PAGE_SOURCES:
                    break;
            }
        }

        void drawHighlightIfSelected(RenderTarget& rt, int32_t itemIndex, int32_t listWidth, int32_t y)
        {
            if (itemIndex == _selectedItem)
            {
                Rectangle::filter(rt, { 0, y, listWidth, y + kItemHeight }, FilterPaletteID::paletteDarken1);
            }
        }

        void drawInstalledList(RenderTarget& rt)
        {
            const auto& listWidget = widgets[WIDX_LIST];
            int32_t listWidth = listWidget.width() - 1;
            int32_t nameWidth = listWidth * 2 / 5;
            int32_t versionWidth = listWidth / 8;

            int32_t y = 0;
            for (size_t i = 0; i < _installed.size(); i++, y += kItemHeight)
            {
                if (y + kItemHeight < rt.y || y >= rt.y + rt.height)
                    continue;

                drawHighlightIfSelected(rt, static_cast<int32_t>(i), listWidth, y);

                const auto& item = _installed[i];
                int32_t x = 3;
                drawTextEllipsised(rt, { x, y + 3 }, nameWidth - 5, item.name, { colours[1] });
                x += nameWidth;
                drawTextEllipsised(rt, { x, y + 3 }, versionWidth - 5, item.version, { colours[1] });
                x += versionWidth;
                drawTextEllipsised(rt, { x, y + 3 }, listWidth - x - kScrollBarWidth - 4, item.authors, { colours[1] });
            }
        }

        void drawAvailableList(RenderTarget& rt)
        {
            const auto& listWidget = widgets[WIDX_LIST];
            int32_t listWidth = listWidget.width() - 1;
            int32_t markerWidth = getStringWidth(LanguageGetString(STR_PLUGIN_MANAGER_TAB_INSTALLED), FontStyle::medium) + 6;
            int32_t authorWidth = listWidth / 5;

            int32_t y = 0;
            for (size_t i = 0; i < _available.size(); i++, y += kItemHeight)
            {
                if (y + kItemHeight < rt.y || y >= rt.y + rt.height)
                    continue;

                auto itemIndex = static_cast<int32_t>(i);
                drawHighlightIfSelected(rt, itemIndex, listWidth, y);

                const auto& entry = _available[i];

                int32_t right = listWidth - kScrollBarWidth - 4;
                if (isStoreInstalled(entry.id))
                {
                    drawText(
                        rt, { right - markerWidth, y + 3 }, STR_PLUGIN_MANAGER_TAB_INSTALLED, { Drawing::Colour::mossGreen });
                }
                right -= markerWidth + 4;

                int32_t authorLeft = right - authorWidth;
                drawTextEllipsised(rt, { authorLeft, y + 3 }, authorWidth, entry.author, { colours[1] });

                // Show the description instead of the name while the row is selected
                const auto& text = (itemIndex == _selectedItem && !entry.description.empty()) ? entry.description : entry.name;
                drawTextEllipsised(rt, { 3, y + 3 }, authorLeft - 8, text, { colours[1] });
            }
        }

        void drawSourcesList(RenderTarget& rt)
        {
            const auto& listWidget = widgets[WIDX_LIST];
            int32_t listWidth = listWidget.width() - 1;
            int32_t textWidth = listWidth - kScrollBarWidth - 6;

            drawHighlightIfSelected(rt, 0, listWidth, 0);
            drawTextEllipsised(rt, { 3, 3 }, textWidth, STR_PLUGIN_MANAGER_BUILTIN_SOURCE, { colours[1] });

            int32_t y = kItemHeight;
            for (size_t i = 0; i < _customSources.size(); i++, y += kItemHeight)
            {
                if (y + kItemHeight < rt.y || y >= rt.y + rt.height)
                    continue;

                drawHighlightIfSelected(rt, static_cast<int32_t>(i + 1), listWidth, y);
                drawTextEllipsised(rt, { 3, y + 3 }, textWidth, _customSources[i], { colours[1] });
            }
        }
    };

    WindowBase* PluginManagerOpen()
    {
        auto* windowMgr = GetWindowManager();
        auto* window = windowMgr->BringToFrontByClass(WindowClass::pluginManager);
        if (window == nullptr)
        {
            window = windowMgr->Create<PluginManagerWindow>(
                WindowClass::pluginManager, kMinimumWindowSize,
                { WindowFlag::higherContrastOnPress, WindowFlag::resizable, WindowFlag::centreScreen });
        }
        return window;
    }
} // namespace OpenRCT2::Ui::Windows

#endif // ENABLE_SCRIPTING
