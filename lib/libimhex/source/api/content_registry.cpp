#include <hex/api/content_registry.hpp>

#include <hex/helpers/shared_data.hpp>
#include <hex/helpers/paths.hpp>
#include <hex/helpers/logger.hpp>

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

namespace hex {

    /* Settings */

    void ContentRegistry::Settings::load() {
        bool loaded = false;
        for (const auto &dir : hex::getPath(ImHexPath::Config)) {
            std::ifstream settingsFile(dir / "settings.json");

            if (settingsFile.good()) {
                settingsFile >> getSettingsData();
                loaded = true;
                break;
            }
        }

        if (!loaded)
            ContentRegistry::Settings::store();
    }

    void ContentRegistry::Settings::store() {
        for (const auto &dir : hex::getPath(ImHexPath::Config)) {
            std::ofstream settingsFile(dir / "settings.json", std::ios::trunc);

            if (settingsFile.good()) {
                settingsFile << getSettingsData();
                break;
            }
        }
    }

    void ContentRegistry::Settings::add(const std::string &unlocalizedCategory, const std::string &unlocalizedName, s64 defaultValue, const ContentRegistry::Settings::Callback &callback) {
        log::info("Registered new integer setting: [{}]: {}", unlocalizedCategory, unlocalizedName);

        ContentRegistry::Settings::getEntries()[unlocalizedCategory.c_str()].emplace_back(Entry{ unlocalizedName.c_str(), callback });

        auto &json = getSettingsData();

        if (!json.contains(unlocalizedCategory))
            json[unlocalizedCategory] = nlohmann::json::object();
        if (!json[unlocalizedCategory].contains(unlocalizedName) || !json[unlocalizedCategory][unlocalizedName].is_number())
            json[unlocalizedCategory][unlocalizedName] = int(defaultValue);
    }

    void ContentRegistry::Settings::add(const std::string &unlocalizedCategory, const std::string &unlocalizedName, const std::string &defaultValue, const ContentRegistry::Settings::Callback &callback) {
        log::info("Registered new string setting: [{}]: {}", unlocalizedCategory, unlocalizedName);

        ContentRegistry::Settings::getEntries()[unlocalizedCategory].emplace_back(Entry{ unlocalizedName, callback });

        auto &json = getSettingsData();

        if (!json.contains(unlocalizedCategory))
            json[unlocalizedCategory] = nlohmann::json::object();
        if (!json[unlocalizedCategory].contains(unlocalizedName) || !json[unlocalizedCategory][unlocalizedName].is_string())
            json[unlocalizedCategory][unlocalizedName] = std::string(defaultValue);
    }

    void ContentRegistry::Settings::write(const std::string &unlocalizedCategory, const std::string &unlocalizedName, s64 value) {
        auto &json = getSettingsData();

        if (!json.contains(unlocalizedCategory))
            json[unlocalizedCategory] = nlohmann::json::object();

        json[unlocalizedCategory][unlocalizedName] = value;
    }

    void ContentRegistry::Settings::write(const std::string &unlocalizedCategory, const std::string &unlocalizedName, const std::string &value) {
        auto &json = getSettingsData();

        if (!json.contains(unlocalizedCategory))
            json[unlocalizedCategory] = nlohmann::json::object();

        json[unlocalizedCategory][unlocalizedName] = value;
    }

    void ContentRegistry::Settings::write(const std::string &unlocalizedCategory, const std::string &unlocalizedName, const std::vector<std::string>& value) {
        auto &json = getSettingsData();

        if (!json.contains(unlocalizedCategory))
            json[unlocalizedCategory] = nlohmann::json::object();

        json[unlocalizedCategory][unlocalizedName] = value;
    }


    s64 ContentRegistry::Settings::read(const std::string &unlocalizedCategory, const std::string &unlocalizedName, s64 defaultValue) {
        auto &json = getSettingsData();

        if (!json.contains(unlocalizedCategory))
            return defaultValue;
        if (!json[unlocalizedCategory].contains(unlocalizedName))
            return defaultValue;

        if (!json[unlocalizedCategory][unlocalizedName].is_number())
            json[unlocalizedCategory][unlocalizedName] = defaultValue;

        return json[unlocalizedCategory][unlocalizedName].get<s64>();
    }

    std::string ContentRegistry::Settings::read(const std::string &unlocalizedCategory, const std::string &unlocalizedName, const std::string &defaultValue) {
        auto &json = getSettingsData();

        if (!json.contains(unlocalizedCategory))
            return defaultValue;
        if (!json[unlocalizedCategory].contains(unlocalizedName))
            return defaultValue;

        if (!json[unlocalizedCategory][unlocalizedName].is_string())
            json[unlocalizedCategory][unlocalizedName] = defaultValue;

        return json[unlocalizedCategory][unlocalizedName].get<std::string>();
    }

    std::vector<std::string> ContentRegistry::Settings::read(const std::string &unlocalizedCategory, const std::string &unlocalizedName, const std::vector<std::string>& defaultValue) {
        auto &json = getSettingsData();

        if (!json.contains(unlocalizedCategory))
            return defaultValue;
        if (!json[unlocalizedCategory].contains(unlocalizedName))
            return defaultValue;

        if (!json[unlocalizedCategory][unlocalizedName].is_array())
            json[unlocalizedCategory][unlocalizedName] = defaultValue;

        if (!json[unlocalizedCategory][unlocalizedName].array().empty() && !json[unlocalizedCategory][unlocalizedName][0].is_string())
            json[unlocalizedCategory][unlocalizedName] = defaultValue;

        return json[unlocalizedCategory][unlocalizedName].get<std::vector<std::string>>();
    }


    std::map<std::string, std::vector<ContentRegistry::Settings::Entry>>& ContentRegistry::Settings::getEntries() {
        return SharedData::settingsEntries;
    }

    nlohmann::json ContentRegistry::Settings::getSetting(const std::string &unlocalizedCategory, const std::string &unlocalizedName) {
        auto &settings = getSettingsData();

        if (!settings.contains(unlocalizedCategory)) return { };
        if (!settings[unlocalizedCategory].contains(unlocalizedName)) return { };

        return settings[unlocalizedCategory][unlocalizedName];
    }

    nlohmann::json& ContentRegistry::Settings::getSettingsData() {
        return SharedData::settingsJson;
    }


    /* Command Palette Commands */

    void ContentRegistry::CommandPaletteCommands::add(ContentRegistry::CommandPaletteCommands::Type type, const std::string &command, const std::string &unlocalizedDescription, const std::function<std::string(std::string)> &displayCallback, const std::function<void(std::string)> &executeCallback) {
        log::info("Registered new command palette command: {}", command);

        getEntries().push_back(ContentRegistry::CommandPaletteCommands::Entry{ type, command, unlocalizedDescription, displayCallback, executeCallback });
    }

    std::vector<ContentRegistry::CommandPaletteCommands::Entry>& ContentRegistry::CommandPaletteCommands::getEntries() {
        return SharedData::commandPaletteCommands;
    }


    /* Pattern Language Functions */


    static std::string getFunctionName(const ContentRegistry::PatternLanguage::Namespace &ns, const std::string &name) {
        std::string functionName;
        for (auto &scope : ns)
            functionName += scope + "::";

        functionName += name;

        return functionName;
    }

    void ContentRegistry::PatternLanguage::addFunction(const Namespace &ns, const std::string &name, u32 parameterCount, const ContentRegistry::PatternLanguage::Callback &func) {
        log::info("Registered new pattern language function: {}", getFunctionName(ns, name));

        getFunctions()[getFunctionName(ns, name)] = Function { parameterCount, func, false };
    }

    void ContentRegistry::PatternLanguage::addDangerousFunction(const Namespace &ns, const std::string &name, u32 parameterCount, const ContentRegistry::PatternLanguage::Callback &func) {
        log::info("Registered new dangerous pattern language function: {}", getFunctionName(ns, name));

        getFunctions()[getFunctionName(ns, name)] = Function { parameterCount, func, true };
    }

    std::map<std::string, ContentRegistry::PatternLanguage::Function>& ContentRegistry::PatternLanguage::getFunctions() {
        return SharedData::patternLanguageFunctions;
    }


    /* Views */

    void ContentRegistry::Views::impl::add(View *view) {
        log::info("Registered new view: {}", view->getUnlocalizedName());

        getEntries().insert({ view->getUnlocalizedName(), view });
    }

    std::map<std::string, View*>& ContentRegistry::Views::getEntries() {
        return SharedData::views;
    }

    View *ContentRegistry::Views::getViewByName(const std::string &unlocalizedName) {
        auto &views = getEntries();

        if (views.contains(unlocalizedName))
            return views[unlocalizedName];
        else
            return nullptr;
    }


    /* Tools */

    void ContentRegistry::Tools:: add(const std::string &unlocalizedName, const std::function<void()> &function) {
        log::info("Registered new tool: {}", unlocalizedName);

        getEntries().emplace_back(impl::Entry{ unlocalizedName, function });
    }

    std::vector<ContentRegistry::Tools::impl::Entry>& ContentRegistry::Tools::getEntries() {
        return SharedData::toolsEntries;
    }


    /* Data Inspector */

    void ContentRegistry::DataInspector::add(const std::string &unlocalizedName, size_t requiredSize, ContentRegistry::DataInspector::impl::GeneratorFunction function) {
        log::info("Registered new data inspector format: {}", unlocalizedName);

        getEntries().push_back({ unlocalizedName, requiredSize, std::move(function) });
    }

    std::vector<ContentRegistry::DataInspector::impl::Entry>& ContentRegistry::DataInspector::getEntries() {
        return SharedData::dataInspectorEntries;
    }

    /* Data Processor Nodes */

    void ContentRegistry::DataProcessorNode::impl::add(const impl::Entry &entry) {
        log::info("Registered new data processor node type: [{}]: ", entry.category, entry.name);

        getEntries().push_back(entry);
    }

    void ContentRegistry::DataProcessorNode::addSeparator() {
        getEntries().push_back({ "", "", []{ return nullptr; } });
    }

    std::vector<ContentRegistry::DataProcessorNode::impl::Entry>& ContentRegistry::DataProcessorNode::getEntries() {
        return SharedData::dataProcessorNodes;
    }

    /* Languages */

    void ContentRegistry::Language::registerLanguage(const std::string &name, const std::string &languageCode) {
        log::info("Registered new language: {} ({})", name, languageCode);

        getLanguages().insert({ languageCode, name });
    }

    void ContentRegistry::Language::addLocalizations(const std::string &languageCode, const LanguageDefinition &definition) {
        log::info("Registered new localization for language {} with {} entries", languageCode, definition.getEntries().size());

        getLanguageDefinitions()[languageCode].push_back(definition);
    }

    std::map<std::string, std::string>& ContentRegistry::Language::getLanguages() {
        return SharedData::languageNames;
    }

    std::map<std::string, std::vector<LanguageDefinition>>& ContentRegistry::Language::getLanguageDefinitions() {
        return SharedData::languageDefinitions;
    }



    /* Interface */

    u32 ContentRegistry::Interface::getDockSpaceId() {
        return SharedData::dockSpaceId;
    }

    void ContentRegistry::Interface::registerMainMenuItem(const std::string &unlocalizedName, const impl::DrawCallback &function) {
        log::info("Registered new main menu item: {}", unlocalizedName);

        getMainMenuItems().push_back({ unlocalizedName, function });
    }

    void ContentRegistry::Interface::addWelcomeScreenEntry(const ContentRegistry::Interface::impl::DrawCallback &function) {
        getWelcomeScreenEntries().push_back(function);
    }

    void ContentRegistry::Interface::addFooterItem(const ContentRegistry::Interface::impl::DrawCallback &function){
        getFooterItems().push_back(function);
    }

    void ContentRegistry::Interface::addToolbarItem(const ContentRegistry::Interface::impl::DrawCallback &function){
        getToolbarItems().push_back(function);
    }

    void ContentRegistry::Interface::addLayout(const std::string &unlocalizedName, const impl::LayoutFunction &function) {
        log::info("Added new layout: {}", unlocalizedName);

        getLayouts().push_back({ unlocalizedName, function });
    }


    std::vector<ContentRegistry::Interface::impl::MainMenuItem> &ContentRegistry::Interface::getMainMenuItems() {
        return SharedData::mainMenuItems;
    }

    std::vector<ContentRegistry::Interface::impl::DrawCallback>& ContentRegistry::Interface::getWelcomeScreenEntries() {
        return SharedData::welcomeScreenEntries;
    }
    std::vector<ContentRegistry::Interface::impl::DrawCallback>& ContentRegistry::Interface::getFooterItems() {
        return SharedData::footerItems;
    }
    std::vector<ContentRegistry::Interface::impl::DrawCallback>& ContentRegistry::Interface::getToolbarItems() {
        return SharedData::toolbarItems;
    }

    std::vector<ContentRegistry::Interface::impl::Layout>& ContentRegistry::Interface::getLayouts() {
        return SharedData::layouts;
    }


    /* Providers */

    void ContentRegistry::Provider::impl::addProviderName(const std::string &unlocalizedName) {
        log::info("Registered new provider: {}", unlocalizedName);

        SharedData::providerNames.push_back(unlocalizedName);
    }

    const std::vector<std::string> &ContentRegistry::Provider::getEntries() {
        return SharedData::providerNames;
    }



    /* Data Formatters */

    void ContentRegistry::DataFormatter::add(const std::string &unlocalizedName, const impl::Callback &callback) {
        log::info("Registered new data formatter: {}", unlocalizedName);

        ContentRegistry::DataFormatter::getEntries().push_back({ unlocalizedName, callback });
    }

    std::vector<ContentRegistry::DataFormatter::impl::Entry> &ContentRegistry::DataFormatter::getEntries() {
        return SharedData::dataFormatters;
    }



    /* File Handlers */

    void ContentRegistry::FileHandler::add(const std::vector<std::string> &extensions, const impl::Callback &callback) {
        for (const auto &extension : extensions)
            log::info("Registered new data handler for extensions: {}", extension);

        ContentRegistry::FileHandler::getEntries().push_back({  extensions, callback });
    }

    std::vector<ContentRegistry::FileHandler::impl::Entry> &ContentRegistry::FileHandler::getEntries() {
        return SharedData::fileHandlers;
    }
}