#include "window.hpp"

#include <hex.hpp>

#include <hex/helpers/utils.hpp>
#include <hex/helpers/paths.hpp>
#include <hex/helpers/logger.hpp>
#include <hex/helpers/file.hpp>

#include <chrono>
#include <csignal>
#include <iostream>
#include <numeric>
#include <thread>
#include <assert.h>

#include <romfs/romfs.hpp>

#include <imgui.h>
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_freetype.h>
#include <hex/ui/imgui_imhex_extensions.h>
#include <implot.h>
#include <implot_internal.h>
#include <imnodes.h>
#include <imnodes_internal.h>

#include <fontawesome_font.h>
#include <codicons_font.h>
#include <unifont_font.h>

#include "helpers/plugin_manager.hpp"
#include <hex/helpers/project_file_handler.hpp>
#include "init/tasks.hpp"

#include <GLFW/glfw3.h>

#include <nlohmann/json.hpp>

namespace hex {

    using namespace std::literals::chrono_literals;

    void *ImHexSettingsHandler_ReadOpenFn(ImGuiContext *ctx, ImGuiSettingsHandler *, const char *) {
        return ctx; // Unused, but the return value has to be non-null
    }

    void ImHexSettingsHandler_ReadLine(ImGuiContext*, ImGuiSettingsHandler *handler, void *, const char* line) {
        for (auto &[name, view] : ContentRegistry::Views::getEntries()) {
            std::string format = std::string(view->getUnlocalizedName()) + "=%d";
            sscanf(line, format.c_str(), &view->getWindowOpenState());
        }
    }

    void ImHexSettingsHandler_WriteAll(ImGuiContext* ctx, ImGuiSettingsHandler *handler, ImGuiTextBuffer *buf) {
        buf->reserve(buf->size() + 0x20); // Ballpark reserve

        buf->appendf("[%s][General]\n", handler->TypeName);

        for (auto &[name, view] : ContentRegistry::Views::getEntries()) {
            buf->appendf("%s=%d\n", name.c_str(), view->getWindowOpenState());
        }

        buf->append("\n");
    }

    Window::Window() {
        {
            for (const auto &[argument, value] : init::getInitArguments()) {
                if (argument == "update-available") {
                    this->m_availableUpdate = value;
                } else if (argument == "no-plugins") {
                    View::doLater([]{ ImGui::OpenPopup("No Plugins"); });
                } else if (argument == "tip-of-the-day") {
                    this->m_tipOfTheDay = value;

                    this->m_showTipOfTheDay = ContentRegistry::Settings::read("hex.builtin.setting.general", "hex.builtin.setting.general.show_tips", 1);
                    if (this->m_showTipOfTheDay)
                        View::doLater([]{ ImGui::OpenPopup("hex.welcome.tip_of_the_day"_lang); });
                }
            }
        }

        this->initGLFW();
        this->initImGui();
        this->setupNativeWindow();

        EventManager::subscribe<EventSettingsChanged>(this, [this]() {
            {
                auto theme = ContentRegistry::Settings::getSetting("hex.builtin.setting.interface", "hex.builtin.setting.interface.color");

                if (theme.is_number())
                    EventManager::post<RequestChangeTheme>(theme.get<int>());
            }

            {
                auto language = ContentRegistry::Settings::getSetting("hex.builtin.setting.interface", "hex.builtin.setting.interface.language");

                if (language.is_string()) {
                    LangEntry::loadLanguage(static_cast<std::string>(language));
                } else {
                    // If no language is specified, fall back to English.
                    LangEntry::loadLanguage("en-US");
                }
            }

            {
                auto targetFps = ContentRegistry::Settings::getSetting("hex.builtin.setting.interface", "hex.builtin.setting.interface.fps");

                if (targetFps.is_number())
                    this->m_targetFps = targetFps;
            }

            {
                if (ContentRegistry::Settings::read("hex.builtin.setting.imhex", "hex.builtin.setting.imhex.launched", 0) == 1)
                    this->m_layoutConfigured = true;
                else
                    ContentRegistry::Settings::write("hex.builtin.setting.imhex", "hex.builtin.setting.imhex.launched", 1);
            }
        });

        EventManager::subscribe<RequestChangeTheme>(this, [this](u32 theme) {
            if (this->m_bannerTexture.valid())
                ImGui::UnloadImage(this->m_bannerTexture);

            switch (theme) {
                default:
                case 1: /* Dark theme */
                {
                    ImGui::StyleColorsDark();
                    ImGui::StyleCustomColorsDark();
                    ImPlot::StyleColorsDark();

                    auto banner = romfs::get("banner_dark.png");
                    this->m_bannerTexture = ImGui::LoadImageFromMemory(reinterpret_cast<const ImU8*>(banner.data()), banner.size());

                    break;
                }
                case 2: /* Light theme */
                {
                    ImGui::StyleColorsLight();
                    ImGui::StyleCustomColorsLight();
                    ImPlot::StyleColorsLight();

                    auto banner = romfs::get("banner_light.png");
                    this->m_bannerTexture = ImGui::LoadImageFromMemory(reinterpret_cast<const ImU8*>(banner.data()), banner.size());

                    break;
                }
                case 3: /* Classic theme */
                {
                    ImGui::StyleColorsClassic();
                    ImGui::StyleCustomColorsClassic();
                    ImPlot::StyleColorsClassic();

                    auto banner = romfs::get("banner_dark.png");
                    this->m_bannerTexture = ImGui::LoadImageFromMemory(reinterpret_cast<const ImU8*>(banner.data()), banner.size());

                    break;
                }
            }

            ImGui::GetStyle().Colors[ImGuiCol_DockingEmptyBg] = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
            ImGui::GetStyle().Colors[ImGuiCol_TitleBg] = ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg];
            ImGui::GetStyle().Colors[ImGuiCol_TitleBgActive] = ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg];
            ImGui::GetStyle().Colors[ImGuiCol_TitleBgCollapsed] = ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg];

            if (!this->m_bannerTexture.valid()) {
                log::fatal("Failed to load banner texture!");
                std::abort();
            }
        });

        EventManager::subscribe<EventFileLoaded>(this, [](const auto &path){
            SharedData::recentFilePaths.push_front(path);

            {
                std::list<fs::path> uniques;
                for (auto &file : SharedData::recentFilePaths) {

                    bool exists = false;
                    for (auto &unique : uniques) {
                        if (file == unique)
                            exists = true;
                    }

                    if (!exists && !file.empty())
                        uniques.push_back(file);

                    if (uniques.size() > 5)
                        break;
                }
                SharedData::recentFilePaths = uniques;
            }

            {
                std::vector<std::string> recentFilesVector;
                for (const auto &recentPath : SharedData::recentFilePaths)
                    recentFilesVector.push_back(recentPath.string());

                ContentRegistry::Settings::write("hex.builtin.setting.imhex", "hex.builtin.setting.imhex.recent_files", recentFilesVector);
            }
        });

        EventManager::subscribe<EventFileUnloaded>(this, []{
            EventManager::post<RequestChangeWindowTitle>("");
        });

        EventManager::subscribe<RequestCloseImHex>(this, [this](bool noQuestions) {
            glfwSetWindowShouldClose(this->m_window, true);

            if (!noQuestions)
                EventManager::post<EventWindowClosing>(this->m_window);
        });

        EventManager::subscribe<RequestChangeWindowTitle>(this, [this](std::string windowTitle) {
            std::string title = "ImHex";

            if (ImHexApi::Provider::isValid()) {
                if (!windowTitle.empty())
                    title += " - " + windowTitle;

                if (ProjectFile::hasUnsavedChanges())
                    title += " (*)";
            }

            this->m_windowTitle = title;
            glfwSetWindowTitle(this->m_window, title.c_str());
        });

        constexpr auto CrashBackupFileName = "crash_backup.hexproj";

        EventManager::subscribe<EventAbnormalTermination>(this, [CrashBackupFileName](int signal) {
            if (!ProjectFile::hasUnsavedChanges())
                return;

            for (const auto &path : hex::getPath(ImHexPath::Config)) {
                if (ProjectFile::store((fs::path(path) / CrashBackupFileName).string()))
                    break;
            }
        });

        EventManager::subscribe<RequestOpenPopup>(this, [this](auto name){
            this->m_popupsToOpen.push_back(name);
        });

        for (const auto &path : hex::getPath(ImHexPath::Config)) {
            if (auto filePath = fs::path(path) / CrashBackupFileName; fs::exists(filePath)) {
                this->m_safetyBackupPath = filePath;
                View::doLater([]{ ImGui::OpenPopup("hex.safety_backup.title"_lang); });
            }
        }

        for (const auto &path : ContentRegistry::Settings::read("hex.builtin.setting.imhex", "hex.builtin.setting.imhex.recent_files"))
            SharedData::recentFilePaths.push_back(path);


        auto signalHandler = [](int signalNumber) {
            EventManager::post<EventAbnormalTermination>(signalNumber);

            if (std::uncaught_exceptions() > 0) {
                log::fatal("Uncaught exception thrown!");
            }

            // Let's not loop on this...
            std::signal(signalNumber, nullptr);

            #if defined(DEBUG)
                assert(false);
            #else
                std::raise(signalNumber);
            #endif

        };

        std::signal(SIGTERM, signalHandler);
        std::signal(SIGSEGV, signalHandler);
        std::signal(SIGINT,  signalHandler);
        std::signal(SIGILL,  signalHandler);
        std::signal(SIGABRT, signalHandler);
        std::signal(SIGFPE,  signalHandler);

        auto imhexLogo = romfs::get("logo.png");
        this->m_logoTexture = ImGui::LoadImageFromMemory(reinterpret_cast<const ImU8*>(imhexLogo.data()), imhexLogo.size());

        ContentRegistry::Settings::store();
        EventManager::post<EventSettingsChanged>();
    }

    Window::~Window() {
        this->exitImGui();
        this->exitGLFW();

        EventManager::unsubscribe<EventSettingsChanged>(this);
        EventManager::unsubscribe<EventFileLoaded>(this);
        EventManager::unsubscribe<EventFileUnloaded>(this);
        EventManager::unsubscribe<RequestCloseImHex>(this);
        EventManager::unsubscribe<RequestChangeWindowTitle>(this);
        EventManager::unsubscribe<EventAbnormalTermination>(this);
        EventManager::unsubscribe<RequestChangeTheme>(this);
        EventManager::unsubscribe<RequestOpenPopup>(this);

        ImGui::UnloadImage(this->m_bannerTexture);
        ImGui::UnloadImage(this->m_logoTexture);
    }

    void Window::loop() {
        this->m_lastFrameTime = glfwGetTime();
        while (!glfwWindowShouldClose(this->m_window)) {
            if (!glfwGetWindowAttrib(this->m_window, GLFW_VISIBLE) || glfwGetWindowAttrib(this->m_window, GLFW_ICONIFIED)) {
                glfwWaitEvents();

            } else {
                double timeout = (1.0 / 5.0) - (glfwGetTime() - this->m_lastFrameTime);
                timeout = timeout > 0 ? timeout : 0;
                glfwWaitEventsTimeout(ImGui::IsPopupOpen(ImGuiID(0), ImGuiPopupFlags_AnyPopupId) || !SharedData::runningTasks.empty() ? 0 : timeout);
            }


            this->frameBegin();
            this->frame();
            this->frameEnd();
        }
    }

    void Window::frameBegin() {

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_MenuBar     | ImGuiWindowFlags_NoDocking
                | ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoCollapse
                | ImGuiWindowFlags_NoMove      | ImGuiWindowFlags_NoResize
                | ImGuiWindowFlags_NoNavFocus  | ImGuiWindowFlags_NoBringToFrontOnFocus
                | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        if (ImGui::Begin("DockSpace", nullptr, windowFlags)) {
            ImGui::PopStyleVar();
            SharedData::dockSpaceId = ImGui::DockSpace(ImGui::GetID("MainDock"), ImVec2(0.0f, ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing() - ImGui::GetStyle().FramePadding.y * 2 - 1));

            ImGui::Separator();
            ImGui::SetCursorPosX(8);
            for (const auto &callback : ContentRegistry::Interface::getFooterItems()) {
                auto prevIdx = ImGui::GetWindowDrawList()->_VtxCurrentIdx;
                callback();
                auto currIdx = ImGui::GetWindowDrawList()->_VtxCurrentIdx;

                // Only draw separator if something was actually drawn
                if (prevIdx != currIdx) {
                    ImGui::SameLine();
                    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                    ImGui::SameLine();
                }
            }

            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            if (ImGui::BeginMainMenuBar()) {

                auto menuBarHeight = ImGui::GetCurrentWindow()->MenuBarHeight();
                ImGui::SetCursorPosX(5);
                ImGui::Image(this->m_logoTexture, ImVec2(menuBarHeight, menuBarHeight));

                for (const auto &[name, function] : ContentRegistry::Interface::getMainMenuItems()) {
                    if (ImGui::BeginMenu(LangEntry(name))) {
                        function();
                        ImGui::EndMenu();
                    }
                }

                for (auto &[name, view] : ContentRegistry::Views::getEntries()) {
                    view->drawMenu();
                }

                this->drawTitleBar();

                ImGui::EndMainMenuBar();
            }
            ImGui::PopStyleVar();

            // Draw toolbar
            if (ImGui::BeginMenuBar()) {

                for (const auto &callback : ContentRegistry::Interface::getToolbarItems()) {
                    callback();
                    ImGui::SameLine();
                }

                ImGui::EndMenuBar();
            }

            if (!ImHexApi::Provider::isValid()) {
                static char title[256];
                ImFormatString(title, IM_ARRAYSIZE(title), "%s/DockSpace_%08X", ImGui::GetCurrentWindow()->Name, ImGui::GetID("MainDock"));
                if (ImGui::Begin(title)) {
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10_scaled, 10_scaled));
                    if (ImGui::BeginChild("Welcome Screen", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollWithMouse)) {
                        this->drawWelcomeScreen();
                    }
                    ImGui::EndChild();
                    ImGui::PopStyleVar();
                }
                ImGui::End();
            } else if (!this->m_layoutConfigured) {
                this->m_layoutConfigured = true;
                this->resetLayout();
            }

            this->beginNativeWindowFrame();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);

        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
        ImGui::SetNextWindowSize(ImGui::GetMainViewport()->Size / 3, ImGuiCond_Appearing);
        if (ImGui::BeginPopup("hex.welcome.tip_of_the_day"_lang)) {
            ImGui::Header("hex.welcome.tip_of_the_day"_lang, true);

            ImGui::TextFormattedWrapped("{}", this->m_tipOfTheDay.c_str());
            ImGui::NewLine();

            bool dontShowAgain = !this->m_showTipOfTheDay;
            if (ImGui::Checkbox("hex.common.dont_show_again"_lang, &dontShowAgain)) {
                this->m_showTipOfTheDay = !dontShowAgain;
                ContentRegistry::Settings::write("hex.builtin.setting.general", "hex.builtin.setting.general.show_tips", this->m_showTipOfTheDay);
            }

            ImGui::SameLine((ImGui::GetMainViewport()->Size / 3 - ImGui::CalcTextSize("hex.common.close"_lang) - ImGui::GetStyle().FramePadding).x);

            if (ImGui::Button("hex.common.close"_lang))
                ImGui::CloseCurrentPopup();

            ImGui::EndPopup();
        }

        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
        if (ImGui::BeginPopupModal("No Plugins", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::TextUnformatted("No ImHex plugins loaded (including the built-in plugin)!");
            ImGui::TextUnformatted("Make sure you at least got the builtin plugin in your plugins folder.");
            ImGui::TextUnformatted("To find out where your plugin folder is, check ImHex' Readme.");
            ImGui::EndPopup();
        }

        // Popup for if there is a safety backup present because ImHex crashed
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
        if (ImGui::BeginPopupModal("hex.safety_backup.title"_lang, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::TextUnformatted("hex.safety_backup.desc"_lang);
            ImGui::NewLine();

            auto width = ImGui::GetWindowWidth();
            ImGui::SetCursorPosX(width / 9);
            if (ImGui::Button("hex.safety_backup.restore"_lang, ImVec2(width / 3, 0))) {
                ProjectFile::load(this->m_safetyBackupPath.string());
                ProjectFile::markDirty();

                ProjectFile::clearProjectFilePath();
                fs::remove(this->m_safetyBackupPath);

                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            ImGui::SetCursorPosX(width / 9 * 5);
            if (ImGui::Button("hex.safety_backup.delete"_lang, ImVec2(width / 3, 0))) {
                fs::remove(this->m_safetyBackupPath);

                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        this->m_popupsToOpen.remove_if([](const auto &name) {
            if (ImGui::IsPopupOpen(name.c_str()))
                return true;
            else
                ImGui::OpenPopup(name.c_str());

            return false;
        });

        EventManager::post<EventFrameBegin>();
    }

    void Window::frame() {
        for (const auto &call : View::getDeferedCalls())
            call();
        View::getDeferedCalls().clear();

        View::drawCommonInterfaces();

        for (auto &[name, view] : ContentRegistry::Views::getEntries()) {
            ImGui::GetCurrentContext()->NextWindowData.ClearFlags();

            view->drawAlwaysVisible();

            if (!view->shouldProcess())
                continue;

            if (view->isAvailable()) {
                ImGui::SetNextWindowSizeConstraints(scaled(view->getMinSize()), scaled(view->getMaxSize()));
                view->drawContent();
            }

            if (view->getWindowOpenState()) {
                auto window = ImGui::FindWindowByName(view->getName().c_str());
                bool hasWindow = window != nullptr;
                bool focused = false;


                if (hasWindow && !(window->Flags & ImGuiWindowFlags_Popup)) {
                    ImGui::Begin(View::toWindowName(name).c_str());

                    focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
                    ImGui::End();
                }

                auto &io = ImGui::GetIO();
                for (const auto &key : this->m_pressedKeys) {
                    ShortcutManager::process(view, io.KeyCtrl, io.KeyAlt, io.KeyShift, io.KeySuper, focused, key);
                }
            }
        }

        this->m_pressedKeys.clear();
    }

    void Window::frameEnd() {
        EventManager::post<EventFrameEnd>();

        this->endNativeWindowFrame();
        ImGui::Render();

        int displayWidth, displayHeight;
        glfwGetFramebufferSize(this->m_window, &displayWidth, &displayHeight);
        glViewport(0, 0, displayWidth, displayHeight);
        glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        GLFWwindow* backup_current_context = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backup_current_context);

        glfwSwapBuffers(this->m_window);

        if (this->m_targetFps <= 200)
            std::this_thread::sleep_for(std::chrono::milliseconds(u64((this->m_lastFrameTime + 1 / this->m_targetFps - glfwGetTime()) * 1000)));

        this->m_lastFrameTime = glfwGetTime();
    }

    void Window::drawWelcomeScreen() {
        const auto availableSpace = ImGui::GetContentRegionAvail();

        ImGui::Image(this->m_bannerTexture, this->m_bannerTexture.size() / (2 * (1.0F / SharedData::globalScale)));

        ImGui::Indent();
        if (ImGui::BeginTable("Welcome Left", 1, ImGuiTableFlags_NoBordersInBody, ImVec2(availableSpace.x / 2, 0))) {

            ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetTextLineHeightWithSpacing() * 3);
            ImGui::TableNextColumn();

            ImGui::TextFormattedWrapped("A Hex Editor for Reverse Engineers, Programmers and people who value their retinas when working at 3 AM.");

            ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetTextLineHeightWithSpacing() * 6);
            ImGui::TableNextColumn();


            ImGui::UnderlinedText("hex.welcome.header.start"_lang);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5_scaled);
            {
                if (ImGui::IconHyperlink(ICON_VS_NEW_FILE, "hex.welcome.start.create_file"_lang))
                    EventManager::post<RequestOpenWindow>("Create File");
                if (ImGui::IconHyperlink(ICON_VS_GO_TO_FILE, "hex.welcome.start.open_file"_lang))
                    EventManager::post<RequestOpenWindow>("Open File");
                if (ImGui::IconHyperlink(ICON_VS_NOTEBOOK, "hex.welcome.start.open_project"_lang))
                    EventManager::post<RequestOpenWindow>("Open Project");
                if (ImGui::IconHyperlink(ICON_VS_TELESCOPE, "hex.welcome.start.open_other"_lang))
                    ImGui::OpenPopup("hex.welcome.start.popup.open_other"_lang);
            }

            ImGui::SetNextWindowPos(ImGui::GetWindowPos() + ImGui::GetCursorPos());
            if (ImGui::BeginPopup("hex.welcome.start.popup.open_other"_lang)) {

                for (const auto &unlocalizedProviderName : ContentRegistry::Provider::getEntries()) {
                    if (ImGui::Hyperlink(LangEntry(unlocalizedProviderName))) {
                        EventManager::post<RequestCreateProvider>(unlocalizedProviderName, nullptr);
                        ImGui::CloseCurrentPopup();
                    }
                }

                ImGui::EndPopup();
            }

            ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetTextLineHeightWithSpacing() * 9);
            ImGui::TableNextColumn();
            ImGui::UnderlinedText("hex.welcome.start.recent"_lang);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5_scaled);
            {
                if (!SharedData::recentFilePaths.empty()) {
                    for (auto &path : SharedData::recentFilePaths) {
                        if (ImGui::BulletHyperlink(fs::path(path).filename().string().c_str())) {
                            EventManager::post<RequestOpenFile>(path);
                            break;
                        }
                    }
                }
            }

            if (!this->m_availableUpdate.empty()) {
                ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetTextLineHeightWithSpacing() * 5);
                ImGui::TableNextColumn();
                ImGui::UnderlinedText("hex.welcome.header.update"_lang);
                {
                    if (ImGui::DescriptionButton("hex.welcome.update.title"_lang, hex::format("hex.welcome.update.desc"_lang, this->m_availableUpdate).c_str(), ImVec2(ImGui::GetContentRegionAvail().x * 0.8F, 0)))
                        hex::openWebpage("hex.welcome.update.link"_lang);
                }
            }

            ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetTextLineHeightWithSpacing() * 6);
            ImGui::TableNextColumn();
            ImGui:: UnderlinedText("hex.welcome.header.help"_lang);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5_scaled);
            {
                if (ImGui::IconHyperlink(ICON_VS_GITHUB, "hex.welcome.help.repo"_lang)) hex::openWebpage("hex.welcome.help.repo.link"_lang);
                if (ImGui::IconHyperlink(ICON_VS_ORGANIZATION, "hex.welcome.help.gethelp"_lang)) hex::openWebpage("hex.welcome.help.gethelp.link"_lang);
                if (ImGui::IconHyperlink(ICON_VS_COMMENT_DISCUSSION, "hex.welcome.help.discord"_lang)) hex::openWebpage("hex.welcome.help.discord.link"_lang);
            }

            ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetTextLineHeightWithSpacing() * 5);
            ImGui::TableNextColumn();
            ImGui::UnderlinedText("hex.welcome.header.plugins"_lang);
            {
                const auto &plugins = PluginManager::getPlugins();

                if (!plugins.empty()) {
                    if (ImGui::BeginTable("plugins", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2((ImGui::GetContentRegionAvail().x * 5) / 6, ImGui::GetTextLineHeightWithSpacing() * 5))) {
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableSetupColumn("hex.welcome.plugins.plugin"_lang);
                        ImGui::TableSetupColumn("hex.welcome.plugins.author"_lang);
                        ImGui::TableSetupColumn("hex.welcome.plugins.desc"_lang);

                        ImGui::TableHeadersRow();

                        ImGuiListClipper clipper;
                        clipper.Begin(plugins.size());

                        while (clipper.Step()) {
                            for (u64 i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::TextUnformatted((plugins[i].getPluginName() + "   ").c_str());
                                ImGui::TableNextColumn();
                                ImGui::TextUnformatted((plugins[i].getPluginAuthor() + "   ").c_str());
                                ImGui::TableNextColumn();
                                ImGui::TextUnformatted(plugins[i].getPluginDescription().c_str());
                            }
                        }

                        clipper.End();

                        ImGui::EndTable();
                    }
                }
            }

            ImGui::EndTable();
        }
        ImGui::SameLine();
        if (ImGui::BeginTable("Welcome Right", 1, ImGuiTableFlags_NoBordersInBody, ImVec2(availableSpace.x / 2, 0))) {
            ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetTextLineHeightWithSpacing() * 5);
            ImGui::TableNextColumn();
            ImGui::UnderlinedText("hex.welcome.header.customize"_lang);
            {
                if (ImGui::DescriptionButton("hex.welcome.customize.settings.title"_lang, "hex.welcome.customize.settings.desc"_lang, ImVec2(ImGui::GetContentRegionAvail().x * 0.8F, 0)))
                    EventManager::post<RequestOpenWindow>("Settings");
            }
            ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetTextLineHeightWithSpacing() * 5);
            ImGui::TableNextColumn();
            ImGui::UnderlinedText("hex.welcome.header.learn"_lang);
            {
                if (ImGui::DescriptionButton("hex.welcome.learn.latest.title"_lang, "hex.welcome.learn.latest.desc"_lang, ImVec2(ImGui::GetContentRegionAvail().x * 0.8F, 0)))
                    hex::openWebpage("hex.welcome.learn.latest.link"_lang);
                if (ImGui::DescriptionButton("hex.welcome.learn.pattern.title"_lang, "hex.welcome.learn.pattern.desc"_lang, ImVec2(ImGui::GetContentRegionAvail().x * 0.8F, 0)))
                    hex::openWebpage("hex.welcome.learn.pattern.link"_lang);
                if (ImGui::DescriptionButton("hex.welcome.learn.plugins.title"_lang, "hex.welcome.learn.plugins.desc"_lang, ImVec2(ImGui::GetContentRegionAvail().x * 0.8F, 0)))
                    hex::openWebpage("hex.welcome.learn.plugins.link"_lang);
            }

            auto extraWelcomeScreenEntries = ContentRegistry::Interface::getWelcomeScreenEntries();
            if (!extraWelcomeScreenEntries.empty()) {
                ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetTextLineHeightWithSpacing() * 5);
                ImGui::TableNextColumn();
                ImGui::UnderlinedText("hex.welcome.header.various"_lang);
                {
                    for (const auto &callback : extraWelcomeScreenEntries)
                        callback();
                }
            }


            ImGui::EndTable();
        }
    }

    void Window::resetLayout() const {

        if (auto &layouts = ContentRegistry::Interface::getLayouts(); !layouts.empty()) {
            auto &[name, function] = layouts[0];

            function(ContentRegistry::Interface::getDockSpaceId());
        }

    }

    void Window::initGLFW() {
        glfwSetErrorCallback([](int error, const char* desc) {
            log::error("GLFW Error [{}] : {}", error, desc);
        });

        if (!glfwInit()) {
            log::fatal("Failed to initialize GLFW!");
            std::abort();
        }

        #if defined(OS_WINDOWS)
            glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
        #elif defined(OS_MACOS)
            glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
        #endif

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

        this->m_windowTitle = "ImHex";
        this->m_window = glfwCreateWindow(1280_scaled, 720_scaled, this->m_windowTitle.c_str(), nullptr, nullptr);

        glfwSetWindowUserPointer(this->m_window, this);

        if (this->m_window == nullptr) {
            log::fatal("Failed to create window!");
            std::abort();
        }

        glfwMakeContextCurrent(this->m_window);
        glfwSwapInterval(1);

        GLFWmonitor *monitor = glfwGetPrimaryMonitor();
        if (monitor != nullptr) {
            const GLFWvidmode *mode = glfwGetVideoMode(monitor);
            if (mode != nullptr) {
                int monitorX, monitorY;
                glfwGetMonitorPos(monitor, &monitorX, &monitorY);

                int windowWidth, windowHeight;
                glfwGetWindowSize(this->m_window, &windowWidth, &windowHeight);

                glfwSetWindowPos(this->m_window, monitorX + (mode->width - windowWidth) / 2, monitorY + (mode->height - windowHeight) / 2);
            }
        }

        {
            int x = 0, y = 0;
            glfwGetWindowPos(this->m_window, &x, &y);
            SharedData::windowPos = ImVec2(x, y);
        }

        {
            int width = 0, height = 0;
            glfwGetWindowSize(this->m_window, &width, &height);
            glfwSetWindowSize(this->m_window, width, height);
            SharedData::windowSize = ImVec2(width, height);
        }

        glfwSetWindowPosCallback(this->m_window, [](GLFWwindow *window, int x, int y) {
            SharedData::windowPos = ImVec2(x, y);

            if (auto g = ImGui::GetCurrentContext(); g == nullptr || g->WithinFrameScope) return;

            auto win = static_cast<Window*>(glfwGetWindowUserPointer(window));
            win->frameBegin();
            win->frame();
            win->frameEnd();
        });

        glfwSetWindowSizeCallback(this->m_window, [](GLFWwindow *window, int width, int height) {
            SharedData::windowSize = ImVec2(width, height);

            if (auto g = ImGui::GetCurrentContext(); g == nullptr || g->WithinFrameScope) return;

            auto win = static_cast<Window*>(glfwGetWindowUserPointer(window));
            win->frameBegin();
            win->frame();
            win->frameEnd();
        });

        glfwSetKeyCallback(this->m_window, [](GLFWwindow *window, int key, int scancode, int action, int mods) {

            auto keyName = glfwGetKeyName(key, scancode);
            if (keyName != nullptr)
                key = std::toupper(keyName[0]);

            auto win = static_cast<Window*>(glfwGetWindowUserPointer(window));

            if (action == GLFW_PRESS) {
                auto &io = ImGui::GetIO();

                win->m_pressedKeys.push_back(key);
                io.KeysDown[key] = true;
                io.KeyCtrl  = (mods & GLFW_MOD_CONTROL) != 0;
                io.KeyShift = (mods & GLFW_MOD_SHIFT) != 0;
                io.KeyAlt   = (mods & GLFW_MOD_ALT) != 0;
            }
            else if (action == GLFW_RELEASE) {
                auto &io = ImGui::GetIO();
                io.KeysDown[key] = false;
                io.KeyCtrl  = (mods & GLFW_MOD_CONTROL) != 0;
                io.KeyShift = (mods & GLFW_MOD_SHIFT) != 0;
                io.KeyAlt   = (mods & GLFW_MOD_ALT) != 0;
            }
        });

        glfwSetDropCallback(this->m_window, [](GLFWwindow *window, int count, const char **paths) {
            if (count != 1)
                return;

            for (u32 i = 0; i < count; i++) {
                auto path = std::filesystem::path(paths[i]);

                bool handled = false;
                for (const auto &[extensions, handler] : ContentRegistry::FileHandler::getEntries()) {
                    for (const auto &extension : extensions) {
                        if (path.extension() == extension) {
                            if (!handler(path))
                                View::showMessagePopup("hex.message.file_handler_failed"_lang);

                            handled = true;
                            break;
                        }
                    }
                }

                if (!handled)
                    EventManager::post<RequestOpenFile>(path.string());
            }
        });

        glfwSetWindowCloseCallback(this->m_window, [](GLFWwindow *window) {
            EventManager::post<EventWindowClosing>(window);
        });

        glfwSetWindowSizeLimits(this->m_window, 720_scaled, 480_scaled, GLFW_DONT_CARE, GLFW_DONT_CARE);

        glfwShowWindow(this->m_window);
    }

    void Window::initImGui() {
        IMGUI_CHECKVERSION();

        GImGui = ImGui::CreateContext(SharedData::fontAtlas);
        GImPlot = ImPlot::CreateContext();
        GImNodes = ImNodes::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        ImGuiStyle& style = ImGui::GetStyle();

        style.Alpha = 1.0F;
        style.WindowRounding = 0.0F;

        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
        #if !defined(OS_LINUX)
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        #endif

        for (auto &entry : SharedData::fontAtlas->ConfigData)
            io.Fonts->ConfigData.push_back(entry);

        io.ConfigViewportsNoTaskBarIcon = false;
        io.KeyMap[ImGuiKey_Tab]         = GLFW_KEY_TAB;
        io.KeyMap[ImGuiKey_LeftArrow]   = GLFW_KEY_LEFT;
        io.KeyMap[ImGuiKey_RightArrow]  = GLFW_KEY_RIGHT;
        io.KeyMap[ImGuiKey_UpArrow]     = GLFW_KEY_UP;
        io.KeyMap[ImGuiKey_DownArrow]   = GLFW_KEY_DOWN;
        io.KeyMap[ImGuiKey_PageUp]      = GLFW_KEY_PAGE_UP;
        io.KeyMap[ImGuiKey_PageDown]    = GLFW_KEY_PAGE_DOWN;
        io.KeyMap[ImGuiKey_Home]        = GLFW_KEY_HOME;
        io.KeyMap[ImGuiKey_End]         = GLFW_KEY_END;
        io.KeyMap[ImGuiKey_Insert]      = GLFW_KEY_INSERT;
        io.KeyMap[ImGuiKey_Delete]      = GLFW_KEY_DELETE;
        io.KeyMap[ImGuiKey_Backspace]   = GLFW_KEY_BACKSPACE;
        io.KeyMap[ImGuiKey_Space]       = GLFW_KEY_SPACE;
        io.KeyMap[ImGuiKey_Enter]       = GLFW_KEY_ENTER;
        io.KeyMap[ImGuiKey_Escape]      = GLFW_KEY_ESCAPE;
        io.KeyMap[ImGuiKey_KeyPadEnter] = GLFW_KEY_KP_ENTER;
        io.KeyMap[ImGuiKey_A]           = GLFW_KEY_A;
        io.KeyMap[ImGuiKey_C]           = GLFW_KEY_C;
        io.KeyMap[ImGuiKey_V]           = GLFW_KEY_V;
        io.KeyMap[ImGuiKey_X]           = GLFW_KEY_X;
        io.KeyMap[ImGuiKey_Y]           = GLFW_KEY_Y;
        io.KeyMap[ImGuiKey_Z]           = GLFW_KEY_Z;

        ImNodes::PushAttributeFlag(ImNodesAttributeFlags_EnableLinkDetachWithDragClick);
        ImNodes::PushAttributeFlag(ImNodesAttributeFlags_EnableLinkCreationOnSnap);

        {
            static bool always = true;
            ImNodes::GetIO().LinkDetachWithModifierClick.Modifier = &always;
        }

        io.UserData = new ImGui::ImHexCustomData();

        style.ScaleAllSizes(SharedData::globalScale);

        {
            GLsizei width, height;
            u8 *fontData;

            io.Fonts->GetTexDataAsRGBA32(&fontData, &width, &height);

            // Create new font atlas
            GLuint tex;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA8, GL_UNSIGNED_INT, fontData);
            io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(tex));
        }

        style.WindowMenuButtonPosition = ImGuiDir_None;
        style.IndentSpacing = 10.0F;

        // Install custom settings handler
        ImGuiSettingsHandler handler;
        handler.TypeName = "ImHex";
        handler.TypeHash = ImHashStr("ImHex");
        handler.ReadOpenFn = ImHexSettingsHandler_ReadOpenFn;
        handler.ReadLineFn = ImHexSettingsHandler_ReadLine;
        handler.WriteAllFn = ImHexSettingsHandler_WriteAll;
        handler.UserData   = this;
        ImGui::GetCurrentContext()->SettingsHandlers.push_back(handler);

        static std::string iniFileName;
        for (const auto &dir : hex::getPath(ImHexPath::Config)) {
            if (std::filesystem::exists(dir)) {
                iniFileName = (dir / "interface.ini").string();
                break;
            }
        }
        io.IniFilename = iniFileName.c_str();

        ImGui_ImplGlfw_InitForOpenGL(this->m_window, true);

        ImGui_ImplOpenGL3_Init("#version 150");

        for (const auto &plugin : PluginManager::getPlugins())
            plugin.setImGuiContext(ImGui::GetCurrentContext());
    }

    void Window::exitGLFW() {
        glfwDestroyWindow(this->m_window);
        glfwTerminate();
    }

    void Window::exitImGui() {
        delete static_cast<ImGui::ImHexCustomData*>(ImGui::GetIO().UserData);

        ImNodes::PopAttributeFlag();
        ImNodes::PopAttributeFlag();

        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImNodes::DestroyContext();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
    }

}
