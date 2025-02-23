#include "content/views/view_hexeditor.hpp"

#include <hex/api/imhex_api.hpp>
#include <hex/providers/provider.hpp>
#include <hex/helpers/crypto.hpp>
#include <hex/helpers/file.hpp>
#include <hex/helpers/paths.hpp>
#include <hex/helpers/patches.hpp>
#include <hex/helpers/project_file_handler.hpp>
#include <hex/helpers/loader_script_handler.hpp>

#include <hex/pattern_language/pattern_data.hpp>
#include <content/providers/file_provider.hpp>


#include <GLFW/glfw3.h>

#include <nlohmann/json.hpp>

#include <thread>
#include <filesystem>

namespace hex::plugin::builtin {

    ViewHexEditor::ViewHexEditor() : View("hex.builtin.view.hexeditor.name"_lang) {

        this->m_searchStringBuffer.resize(0xFFF, 0x00);
        this->m_searchHexBuffer.resize(0xFFF, 0x00);

        ContentRegistry::FileHandler::add({ ".hexproj" }, [](const auto &path) {
            return ProjectFile::load(path.string());
        });

        ContentRegistry::FileHandler::add({ ".tbl" }, [this](const auto &path) {
            this->m_currEncodingFile = EncodingFile(EncodingFile::Type::Thingy, path);

            return true;
        });

        this->m_memoryEditor.ReadFn = [](const ImU8 *data, size_t off) -> ImU8 {
            auto provider = ImHexApi::Provider::get();
            if (!provider->isAvailable() || !provider->isReadable())
                return 0x00;

            ImU8 byte;
            provider->read(off + provider->getBaseAddress() + provider->getCurrentPageAddress(), &byte, sizeof(ImU8));

            return byte;
        };

        this->m_memoryEditor.WriteFn = [](ImU8 *data, size_t off, ImU8 d) -> void {
            auto provider = ImHexApi::Provider::get();
            if (!provider->isAvailable() || !provider->isWritable())
                return;

            provider->write(off + provider->getBaseAddress() + provider->getCurrentPageAddress(), &d, sizeof(ImU8));
            EventManager::post<EventDataChanged>();
            ProjectFile::markDirty();
        };

        this->m_memoryEditor.HighlightFn = [](const ImU8 *data, size_t off, bool next) -> bool {
            ViewHexEditor *_this = (ViewHexEditor *) data;

            std::optional<u32> currColor, prevColor;

            auto provider = ImHexApi::Provider::get();

            off += provider->getBaseAddress() + provider->getCurrentPageAddress();

            u32 alpha = static_cast<u32>(_this->m_highlightAlpha) << 24;

            for (const auto &[region, name, comment, color, locked] : ImHexApi::Bookmarks::getEntries()) {
                if (off >= region.address && off < (region.address + region.size))
                    currColor = (color & 0x00FFFFFF) | alpha;
                if ((off - 1) >= region.address && (off - 1) < (region.address + region.size))
                    prevColor = (color & 0x00FFFFFF) | alpha;
            }

            {
                for (const auto &pattern : SharedData::patternData) {
                    auto child = pattern->getPattern(off);
                    if (child != nullptr) {
                        auto color = (child->getColor() & 0x00FFFFFF) | alpha;
                        currColor = currColor.has_value() ? ImAlphaBlendColors(color, currColor.value()) : color;
                        break;
                    }
                }

                for (const auto &pattern : SharedData::patternData) {
                    auto child = pattern->getPattern(off - 1);
                    if (child != nullptr) {
                        auto color = (child->getColor() & 0x00FFFFFF) | alpha;
                        prevColor = prevColor.has_value() ? ImAlphaBlendColors(color, currColor.value()) : color;
                        break;
                    }
                }
            }

            if (next && prevColor != currColor) {
                return false;
            }

            if (currColor.has_value() && (currColor.value() & 0x00FFFFFF) != 0x00) {
                _this->m_memoryEditor.HighlightColor = (currColor.value() & 0x00FFFFFF) | alpha;
                return true;
            }

            _this->m_memoryEditor.HighlightColor = 0x60C08080;
            return false;
        };

        this->m_memoryEditor.HoverFn = [](const ImU8 *data, size_t off) {
            bool tooltipShown = false;

            off += ImHexApi::Provider::get()->getBaseAddress();

            for (const auto &[region, name, comment, color, locked] : ImHexApi::Bookmarks::getEntries()) {
                if (off >= region.address && off < (region.address + region.size)) {
                    if (!tooltipShown) {
                        ImGui::BeginTooltip();
                        tooltipShown = true;
                    }
                    ImGui::ColorButton(name.data(), ImColor(color).Value);
                    ImGui::SameLine(0, 10);
                    ImGui::TextUnformatted(name.data());
                }
            }

            if (tooltipShown)
                ImGui::EndTooltip();
        };

        this->m_memoryEditor.DecodeFn = [](const ImU8 *data, size_t addr) -> MemoryEditor::DecodeData {
            ViewHexEditor *_this = (ViewHexEditor *) data;

            if (_this->m_currEncodingFile.getLongestSequence() == 0)
                return { ".", 1, 0xFFFF8000 };

            auto provider = ImHexApi::Provider::get();
            size_t size = std::min<size_t>(_this->m_currEncodingFile.getLongestSequence(), provider->getActualSize() - addr);

            std::vector<u8> buffer(size);
            provider->read(addr + provider->getBaseAddress() + provider->getCurrentPageAddress(), buffer.data(), size);

            auto [decoded, advance] = _this->m_currEncodingFile.getEncodingFor(buffer);

            ImColor color;
            if (decoded.length() == 1 && std::isalnum(decoded[0])) color = 0xFFFF8000;
            else if (decoded.length() == 1 && advance == 1) color = 0xFF0000FF;
            else if (decoded.length() > 1 && advance == 1) color = 0xFF00FFFF;
            else if (advance > 1) color = 0xFFFFFFFF;
            else color = 0xFFFF8000;

            return { std::string(decoded), advance, color };
        };

        registerEvents();
        registerShortcuts();
    }

    ViewHexEditor::~ViewHexEditor() {
        EventManager::unsubscribe<RequestOpenFile>(this);
        EventManager::unsubscribe<RequestSelectionChange>(this);
        EventManager::unsubscribe<EventProjectFileLoad>(this);
        EventManager::unsubscribe<EventWindowClosing>(this);
        EventManager::unsubscribe<RequestOpenWindow>(this);
        EventManager::unsubscribe<EventSettingsChanged>(this);
    }

    void ViewHexEditor::drawContent() {
        auto provider = ImHexApi::Provider::get();

        size_t dataSize = (!ImHexApi::Provider::isValid() || !provider->isReadable()) ? 0x00 : provider->getSize();

        this->m_memoryEditor.DrawWindow(View::toWindowName("hex.builtin.view.hexeditor.name").c_str(), &this->getWindowOpenState(), this, dataSize, dataSize == 0 ? 0x00 : provider->getBaseAddress() + provider->getCurrentPageAddress());

        if (dataSize != 0x00) {
            this->m_memoryEditor.OptShowAdvancedDecoding = this->m_advancedDecodingEnabled && this->m_currEncodingFile.valid();

            if (ImGui::Begin(View::toWindowName("hex.builtin.view.hexeditor.name").c_str())) {

                if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
                    ImGui::OpenPopup("hex.builtin.menu.edit"_lang);

                if (ImGui::BeginPopup("hex.builtin.menu.edit"_lang)) {
                    this->drawEditPopup();
                    ImGui::EndPopup();
                }

                if (provider->getPageCount() > 1) {
                    ImGui::NewLine();

                    auto linePos = ImGui::GetCursorPosY() - 15_scaled;

                    ImGui::SetCursorPosY(linePos);

                    if (ImGui::ArrowButton("prevPage", ImGuiDir_Left)) {
                        provider->setCurrentPage(provider->getCurrentPage() - 1);

                        EventManager::post<EventRegionSelected>(Region { std::min(this->m_memoryEditor.DataPreviewAddr, this->m_memoryEditor.DataPreviewAddrEnd), 1 });
                    }

                    ImGui::SameLine();

                    if (ImGui::ArrowButton("nextPage", ImGuiDir_Right)) {
                        provider->setCurrentPage(provider->getCurrentPage() + 1);

                        EventManager::post<EventRegionSelected>(Region { std::min(this->m_memoryEditor.DataPreviewAddr, this->m_memoryEditor.DataPreviewAddrEnd), 1 });
                    }

                    ImGui::SameLine();
                    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(linePos);

                    ImGui::TextFormatted("hex.builtin.view.hexeditor.page"_lang, provider->getCurrentPage() + 1, provider->getPageCount());
                }

                this->drawSearchPopup();
                this->drawGotoPopup();

            }
            ImGui::End();
        }
    }

    static void save() {
        ImHexApi::Provider::get()->save();
    }

    static void saveAs() {
        hex::openFileBrowser("hex.builtin.view.hexeditor.save_as"_lang, DialogMode::Save, { }, [](const auto &path) {
            ImHexApi::Provider::get()->saveAs(path);
        });
    }

    void ViewHexEditor::drawAlwaysVisible() {
        auto provider = ImHexApi::Provider::get();

        if (ImGui::BeginPopupModal("hex.builtin.view.hexeditor.exit_application.title"_lang, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::NewLine();
            ImGui::TextUnformatted("hex.builtin.view.hexeditor.exit_application.desc"_lang);
            ImGui::NewLine();

            confirmButtons("hex.common.yes"_lang, "hex.common.no"_lang, [] {
                ImHexApi::Common::closeImHex(true);
            },
            [] {
                ImGui::CloseCurrentPopup();
            });

            if (ImGui::IsKeyDown(ImGui::GetKeyIndex(ImGuiKey_Escape)))
                ImGui::CloseCurrentPopup();

            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("hex.builtin.view.hexeditor.script.title"_lang, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SetCursorPosX(10);
            ImGui::TextFormattedWrapped("{}", static_cast<const char *>("hex.builtin.view.hexeditor.script.desc"_lang));

            ImGui::NewLine();
            ImGui::InputText("##nolabel", this->m_loaderScriptScriptPath.data(), this->m_loaderScriptScriptPath.length(), ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine();
            if (ImGui::Button("hex.builtin.view.hexeditor.script.script"_lang)) {
                hex::openFileBrowser("hex.builtin.view.hexeditor.script.script.title"_lang, DialogMode::Open, { { "Python Script", "py" } }, [this](const auto &path) {
                    this->m_loaderScriptScriptPath = path.string();
                });
            }
            ImGui::InputText("##nolabel", this->m_loaderScriptFilePath.data(), this->m_loaderScriptFilePath.length(), ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine();
            if (ImGui::Button("hex.builtin.view.hexeditor.script.file"_lang)) {
                hex::openFileBrowser("hex.builtin.view.hexeditor.script.file.title"_lang, DialogMode::Open, { }, [this](const auto &path) {
                    this->m_loaderScriptFilePath = path.string();
                });
            }
            if (ImGui::IsKeyDown(ImGui::GetKeyIndex(ImGuiKey_Escape)))
                ImGui::CloseCurrentPopup();

            ImGui::NewLine();

            confirmButtons("hex.common.load"_lang, "hex.common.cancel"_lang,
               [this, &provider] {
                   if (!this->m_loaderScriptScriptPath.empty() && !this->m_loaderScriptFilePath.empty()) {
                       EventManager::post<RequestOpenFile>(this->m_loaderScriptFilePath);
                       LoaderScript::setFilePath(this->m_loaderScriptFilePath);
                       LoaderScript::setDataProvider(provider);
                       LoaderScript::processFile(this->m_loaderScriptScriptPath);
                       ImGui::CloseCurrentPopup();
                   }
               },
               [] {
                   ImGui::CloseCurrentPopup();
               }
            );

            ImGui::EndPopup();

        }

        if (ImGui::BeginPopupModal("hex.builtin.view.hexeditor.menu.edit.set_base"_lang, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::InputText("hex.common.address"_lang, this->m_baseAddressBuffer, 16, ImGuiInputTextFlags_CharsHexadecimal);
            ImGui::NewLine();

            confirmButtons("hex.common.set"_lang, "hex.common.cancel"_lang,
                           [this, &provider]{
                               provider->setBaseAddress(strtoull(this->m_baseAddressBuffer, nullptr, 16));
                               ImGui::CloseCurrentPopup();
                           }, []{
                        ImGui::CloseCurrentPopup();
                    });

            if (ImGui::IsKeyDown(ImGui::GetKeyIndex(ImGuiKey_Escape)))
                ImGui::CloseCurrentPopup();

            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("hex.builtin.view.hexeditor.menu.edit.resize"_lang, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("0x");
            ImGui::SameLine();
            ImGui::InputScalar("hex.common.size"_lang, ImGuiDataType_U64, &this->m_resizeSize, nullptr, nullptr, "%llx", ImGuiInputTextFlags_CharsHexadecimal);
            ImGui::NewLine();

            confirmButtons("hex.common.set"_lang, "hex.common.cancel"_lang,
                [this, &provider]{
                    provider->resize(this->m_resizeSize);
                    ImGui::CloseCurrentPopup();
                },
                [] {
                    ImGui::CloseCurrentPopup();
                });

            if (ImGui::IsKeyDown(ImGui::GetKeyIndex(ImGuiKey_Escape)))
                ImGui::CloseCurrentPopup();

            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("hex.builtin.view.hexeditor.menu.edit.insert"_lang, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("0x");
            ImGui::SameLine();
            ImGui::InputScalar("hex.common.size"_lang, ImGuiDataType_U64, &this->m_resizeSize, nullptr, nullptr, "%llx", ImGuiInputTextFlags_CharsHexadecimal);
            ImGui::NewLine();

            confirmButtons("hex.common.set"_lang, "hex.common.cancel"_lang,
                [this, &provider] {
                    provider->insert(std::min(this->m_memoryEditor.DataPreviewAddr, this->m_memoryEditor.DataPreviewAddrEnd), this->m_resizeSize);
                    ImGui::CloseCurrentPopup();
                },
                [] {
                    ImGui::CloseCurrentPopup();
                });

            if (ImGui::IsKeyDown(ImGui::GetKeyIndex(ImGuiKey_Escape)))
                ImGui::CloseCurrentPopup();

            ImGui::EndPopup();
        }
    }

    void ViewHexEditor::drawMenu() {
        auto provider = ImHexApi::Provider::get();
        bool providerValid = ImHexApi::Provider::isValid();

        if (ImGui::BeginMenu("hex.builtin.menu.file"_lang)) {
            if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.file.open_file"_lang, "CTRL + O")) {

                hex::openFileBrowser("hex.builtin.view.hexeditor.open_file"_lang, DialogMode::Open, { }, [](const auto &path) {
                    EventManager::post<RequestOpenFile>(path);
                });
            }

            if (ImGui::BeginMenu("hex.builtin.view.hexeditor.menu.file.open_recent"_lang, !SharedData::recentFilePaths.empty())) {
                for (auto &path : SharedData::recentFilePaths) {
                    if (ImGui::MenuItem(fs::path(path).filename().string().c_str())) {
                        EventManager::post<RequestOpenFile>(path);
                    }
                }

                ImGui::Separator();
                if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.file.clear_recent"_lang)) {
                    SharedData::recentFilePaths.clear();
                    ContentRegistry::Settings::write(
                            "hex.builtin.setting.imhex",
                            "hex.builtin.setting.imhex.recent_files",
                            std::vector<std::string>{});
                }

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("hex.builtin.view.hexeditor.menu.file.open_other"_lang)) {

                for (const auto &unlocalizedProviderName : ContentRegistry::Provider::getEntries()) {
                    if (ImGui::MenuItem(LangEntry(unlocalizedProviderName))) {
                        EventManager::post<RequestCreateProvider>(unlocalizedProviderName, nullptr);
                    }
                }

                ImGui::EndMenu();
            }

            if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.file.save"_lang, "CTRL + S", false, providerValid && provider->isWritable())) {
                save();
            }

            if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.file.save_as"_lang, "CTRL + SHIFT + S", false, providerValid && provider->isWritable())) {
                saveAs();
            }

            if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.file.close"_lang, "", false, providerValid)) {
                EventManager::post<EventFileUnloaded>();
                ImHexApi::Provider::remove(ImHexApi::Provider::get());
                providerValid = false;
            }

            if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.file.quit"_lang, "", false)) {
                ImHexApi::Common::closeImHex();
            }

            ImGui::Separator();

            if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.file.open_project"_lang, "")) {
                hex::openFileBrowser("hex.builtin.view.hexeditor.menu.file.open_project"_lang, DialogMode::Open, { { "Project File", "hexproj" } }, [](const auto &path) {
                    ProjectFile::load(path);
                });
            }

            if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.file.save_project"_lang, "", false, providerValid && provider->isWritable())) {
                if (ProjectFile::getProjectFilePath() == "") {
                    hex::openFileBrowser("hex.builtin.view.hexeditor.save_project"_lang, DialogMode::Save, { { "Project File", "hexproj" } }, [](const auto &path) {
                        if (path.extension() == ".hexproj") {
                            ProjectFile::store(path);
                        }
                        else {
                            ProjectFile::store(path.string() + ".hexproj");
                        }
                    });
                }
                else
                    ProjectFile::store();
            }

            if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.file.load_encoding_file"_lang)) {
                hex::openFileBrowser("hex.builtin.view.hexeditor.load_enconding_file"_lang, DialogMode::Open, { { "Thingy Table File", "tbl" } }, [this](const auto &path) {
                    this->m_currEncodingFile = EncodingFile(EncodingFile::Type::Thingy, path);
                });
            }

            ImGui::Separator();

            if (ImGui::BeginMenu("hex.builtin.view.hexeditor.menu.file.import"_lang)) {
                if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.file.import.base64"_lang)) {

                    hex::openFileBrowser("hex.builtin.view.hexeditor.menu.file.import.base64"_lang, DialogMode::Open, { }, [this](const auto &path) {
                        File file(path, File::Mode::Read);
                        if (!file.isValid()) {
                            View::showErrorPopup("hex.builtin.view.hexeditor.error.open"_lang);
                            return;
                        }

                        auto base64 = file.readBytes();

                        if (!base64.empty()) {
                            this->m_dataToSave = crypt::decode64(base64);

                            if (this->m_dataToSave.empty())
                                View::showErrorPopup("hex.builtin.view.hexeditor.base64.import_error"_lang);
                            else
                                ImGui::OpenPopup("hex.builtin.view.hexeditor.save_data"_lang);
                            this->getWindowOpenState() = true;
                        } else View::showErrorPopup("hex.builtin.view.hexeditor.file_open_error"_lang);
                    });
                }

                ImGui::Separator();

                if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.file.import.ips"_lang, nullptr, false, !this->m_processingImportExport)) {

                   hex::openFileBrowser("hex.builtin.view.hexeditor.open_file"_lang, DialogMode::Open, { }, [this](const auto &path) {
                       this->m_processingImportExport = true;
                       std::thread([this, path] {
                           auto task = ImHexApi::Tasks::createTask("hex.builtin.view.hexeditor.processing", 0);

                           auto patchData = File(path, File::Mode::Read).readBytes();
                           auto patch = hex::loadIPSPatch(patchData);

                           task.setMaxValue(patch.size());

                           auto provider = ImHexApi::Provider::get();

                           u64 progress = 0;
                           for (auto &[address, value] : patch) {
                               provider->addPatch(address, &value, 1);
                               progress++;
                               task.update(progress);
                           }

                           provider->createUndoPoint();
                           this->m_processingImportExport = false;
                       }).detach();

                       this->getWindowOpenState() = true;
                   });


                }

                if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.file.import.ips32"_lang, nullptr, false, !this->m_processingImportExport)) {
                    hex::openFileBrowser("hex.builtin.view.hexeditor.open_file"_lang, DialogMode::Open, { }, [this](const auto &path) {
                        this->m_processingImportExport = true;
                        std::thread([this, path] {
                            auto task = ImHexApi::Tasks::createTask("hex.builtin.view.hexeditor.processing", 0);

                            auto patchData = File(path, File::Mode::Read).readBytes();
                            auto patch = hex::loadIPS32Patch(patchData);

                            task.setMaxValue(patch.size());

                            auto provider = ImHexApi::Provider::get();

                            u64 progress = 0;
                            for (auto &[address, value] : patch) {
                                provider->addPatch(address, &value, 1);
                                progress++;
                                task.update(progress);
                            }

                            provider->createUndoPoint();
                            this->m_processingImportExport = false;
                        }).detach();

                        this->getWindowOpenState() = true;
                    });
                }

                if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.file.import.script"_lang)) {
                    this->m_loaderScriptFilePath.clear();
                    this->m_loaderScriptScriptPath.clear();
                    View::doLater([]{ ImGui::OpenPopup("hex.builtin.view.hexeditor.script.title"_lang); });
                }

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("hex.builtin.view.hexeditor.menu.file.export"_lang, providerValid && provider->isWritable())) {
                if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.file.export.ips"_lang, nullptr, false, !this->m_processingImportExport)) {
                    Patches patches = provider->getPatches();
                    if (!patches.contains(0x00454F45) && patches.contains(0x00454F46)) {
                        u8 value = 0;
                        provider->read(0x00454F45, &value, sizeof(u8));
                        patches[0x00454F45] = value;
                    }

                    this->m_processingImportExport = true;
                    std::thread([this, patches]{
                        auto task = ImHexApi::Tasks::createTask("hex.builtin.view.hexeditor.processing", 0);

                        this->m_dataToSave = generateIPSPatch(patches);
                        this->m_processingImportExport = false;

                        View::doLater([this]{
                            hex::openFileBrowser("hex.builtin.view.hexeditor.menu.file.export.title"_lang, DialogMode::Save, { }, [this](const auto &path) {
                                auto file = File(path, File::Mode::Create);
                                if (!file.isValid()) {
                                    View::showErrorPopup("hex.builtin.view.hexeditor.error.create"_lang);
                                    return;
                                }

                                file.write(this->m_dataToSave);
                            });
                        });
                    }).detach();
                }
                if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.file.export.ips32"_lang, nullptr, false, !this->m_processingImportExport)) {
                    Patches patches = provider->getPatches();
                    if (!patches.contains(0x00454F45) && patches.contains(0x45454F46)) {
                        u8 value = 0;
                        provider->read(0x45454F45, &value, sizeof(u8));
                        patches[0x45454F45] = value;
                    }

                    this->m_processingImportExport = true;
                    std::thread([this, patches]{
                        auto task = ImHexApi::Tasks::createTask("hex.builtin.view.hexeditor.processing", 0);

                        this->m_dataToSave = generateIPS32Patch(patches);
                        this->m_processingImportExport = false;

                        View::doLater([this]{
                            hex::openFileBrowser("hex.builtin.view.hexeditor.menu.file.export.title"_lang, DialogMode::Save, { }, [this](const auto &path) {
                                auto file = File(path, File::Mode::Create);
                                if (!file.isValid()) {
                                    View::showErrorPopup("hex.builtin.view.hexeditor.error.create"_lang);
                                    return;
                                }

                                file.write(this->m_dataToSave);
                            });
                        });
                    }).detach();
                }

                ImGui::EndMenu();
            }



            ImGui::Separator();

            if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.file.search"_lang, "CTRL + F")) {
                this->getWindowOpenState() = true;
                ImGui::OpenPopupInWindow(View::toWindowName("hex.builtin.view.hexeditor.name").c_str(), "hex.builtin.view.hexeditor.menu.file.search"_lang);
            }

            if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.file.goto"_lang, "CTRL + G")) {
                this->getWindowOpenState() = true;
                ImGui::OpenPopupInWindow(View::toWindowName("hex.builtin.view.hexeditor.name").c_str(), "hex.builtin.view.hexeditor.menu.file.goto"_lang);
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("hex.builtin.menu.edit"_lang)) {
            this->drawEditPopup();
            ImGui::EndMenu();
        }
    }

    void ViewHexEditor::openFile(const fs::path &path) {
        hex::prv::Provider *provider = nullptr;
        EventManager::post<RequestCreateProvider>("hex.builtin.provider.file", &provider);

        if (auto fileProvider = dynamic_cast<prv::FileProvider*>(provider)) {
            fileProvider->setPath(path);
            if (!fileProvider->open()) {
                View::showErrorPopup("hex.builtin.view.hexeditor.error.open"_lang);
                ImHexApi::Provider::remove(provider);

                return;
            }
        }

        if (!provider->isWritable()) {
            this->m_memoryEditor.ReadOnly = true;
            View::showErrorPopup("hex.builtin.view.hexeditor.error.read_only"_lang);
        } else {
            this->m_memoryEditor.ReadOnly = false;
        }

        if (!provider->isAvailable()) {
            View::showErrorPopup("hex.builtin.view.hexeditor.error.open"_lang);
            ImHexApi::Provider::remove(provider);

            return;
        }

        ProjectFile::setFilePath(path);

        this->getWindowOpenState() = true;

        EventManager::post<EventFileLoaded>(path);
        EventManager::post<EventDataChanged>();

        {
            std::vector<pl::PatternData*> patterns;
            EventManager::post<EventPatternChanged>(patterns);
        }
    }

    void ViewHexEditor::copyBytes() const {
        auto provider = ImHexApi::Provider::get();

        size_t start = std::min(this->m_memoryEditor.DataPreviewAddr, this->m_memoryEditor.DataPreviewAddrEnd);
        size_t end = std::max(this->m_memoryEditor.DataPreviewAddr, this->m_memoryEditor.DataPreviewAddrEnd);

        size_t copySize = (end - start) + 1;

        std::vector<u8> buffer(copySize, 0x00);
        provider->read(start + provider->getBaseAddress() + provider->getCurrentPageAddress(), buffer.data(), buffer.size());

        std::string str;
        for (const auto &byte : buffer)
            str += hex::format("{0:02X} ", byte);
        str.pop_back();

        ImGui::SetClipboardText(str.c_str());
    }

    void ViewHexEditor::pasteBytes() const {
        auto provider = ImHexApi::Provider::get();

        size_t start = std::min(this->m_memoryEditor.DataPreviewAddr, this->m_memoryEditor.DataPreviewAddrEnd);
        size_t end = std::max(this->m_memoryEditor.DataPreviewAddr, this->m_memoryEditor.DataPreviewAddrEnd);

        std::string clipboard = ImGui::GetClipboardText();

        // Check for non-hex characters
        bool isValidHexString = std::find_if(clipboard.begin(), clipboard.end(), [](char c) {
            return !std::isxdigit(c) && !std::isspace(c);
        }) == clipboard.end();

        if (!isValidHexString) return;

        // Remove all whitespace
        std::erase_if(clipboard, [](char c) { return std::isspace(c); });

        // Only paste whole bytes
        if (clipboard.length() % 2 != 0) return;

        // Convert hex string to bytes
        std::vector<u8> buffer(clipboard.length() / 2, 0x00);
        u32 stringIndex = 0;
        for (u8 &byte : buffer) {
            for (u8 i = 0; i < 2; i++) {
                byte <<= 4;

                char c = clipboard[stringIndex];

                if (c >= '0' && c <= '9') byte |= (c - '0');
                else if (c >= 'a' && c <= 'f') byte |= (c - 'a') + 0xA;
                else if (c >= 'A' && c <= 'F') byte |= (c - 'A') + 0xA;

                stringIndex++;
            }
        }

        // Write bytes
        provider->read(start + provider->getBaseAddress() + provider->getCurrentPageAddress(), buffer.data(), std::min(end - start + 1, buffer.size()));
    }

    void ViewHexEditor::copyString() const {
        auto provider = ImHexApi::Provider::get();

        size_t start = std::min(this->m_memoryEditor.DataPreviewAddr, this->m_memoryEditor.DataPreviewAddrEnd);
        size_t end = std::max(this->m_memoryEditor.DataPreviewAddr, this->m_memoryEditor.DataPreviewAddrEnd);

        size_t copySize = (end - start) + 1;

        std::string buffer(copySize, 0x00);
        buffer.reserve(copySize);
        provider->read(start + provider->getBaseAddress() + provider->getCurrentPageAddress(), buffer.data(), copySize);

        ImGui::SetClipboardText(buffer.c_str());
    }

    static std::vector<std::pair<u64, u64>> findString(hex::prv::Provider* &provider, std::string string) {
        std::vector<std::pair<u64, u64>> results;

        u32 foundCharacters = 0;

        std::vector<u8> buffer(1024, 0x00);
        size_t dataSize = provider->getSize();
        for (u64 offset = 0; offset < dataSize; offset += 1024) {
            size_t usedBufferSize = std::min(u64(buffer.size()), dataSize - offset);
            provider->read(offset + provider->getBaseAddress() + provider->getCurrentPageAddress(), buffer.data(), usedBufferSize);

            for (u64 i = 0; i < usedBufferSize; i++) {
                if (buffer[i] == string[foundCharacters])
                    foundCharacters++;
                else
                    foundCharacters = 0;

                if (foundCharacters == string.size()) {
                    results.emplace_back(offset + i - foundCharacters + 1, offset + i);
                    foundCharacters = 0;
                }
            }
        }

        return results;
    }

    static std::vector<std::pair<u64, u64>> findHex(hex::prv::Provider* &provider, std::string string) {
        std::vector<std::pair<u64, u64>> results;

        if ((string.size() % 2) == 1)
            string = "0" + string;

        std::vector<u8> hex;
        hex.reserve(string.size() / 2);

        for (u32 i = 0; i < string.size(); i += 2) {
            char byte[3] = { string[i], string[i + 1], 0 };
            hex.push_back(strtoul(byte, nullptr, 16));
        }

        u32 foundCharacters = 0;

        std::vector<u8> buffer(1024, 0x00);
        size_t dataSize = provider->getSize();
        for (u64 offset = 0; offset < dataSize; offset += 1024) {
            size_t usedBufferSize = std::min(u64(buffer.size()), dataSize - offset);
            provider->read(offset + provider->getBaseAddress() + provider->getCurrentPageAddress(), buffer.data(), usedBufferSize);

            for (u64 i = 0; i < usedBufferSize; i++) {
                if (buffer[i] == hex[foundCharacters])
                    foundCharacters++;
                else
                    foundCharacters = 0;

                if (foundCharacters == hex.size()) {
                    results.emplace_back(offset + i - foundCharacters + 1, offset + i);
                    foundCharacters = 0;
                }
            }
        }

        return results;
    }


    void ViewHexEditor::drawSearchPopup() {
        static auto InputCallback = [](ImGuiInputTextCallbackData* data) -> int {
            auto _this = static_cast<ViewHexEditor*>(data->UserData);
            auto provider = ImHexApi::Provider::get();

            *_this->m_lastSearchBuffer = _this->m_searchFunction(provider, data->Buf);
            _this->m_lastSearchIndex = 0;

            if (!_this->m_lastSearchBuffer->empty())
                _this->m_memoryEditor.GotoAddrAndSelect((*_this->m_lastSearchBuffer)[0].first, (*_this->m_lastSearchBuffer)[0].second);

            return 0;
        };

        static auto Find = [this](char *buffer) {
            auto provider = ImHexApi::Provider::get();

            *this->m_lastSearchBuffer = this->m_searchFunction(provider, buffer);
            this->m_lastSearchIndex = 0;

            if (!this->m_lastSearchBuffer->empty())
                this->m_memoryEditor.GotoAddrAndSelect((*this->m_lastSearchBuffer)[0].first, (*this->m_lastSearchBuffer)[0].second);
        };

        static auto FindNext = [this]() {
            if (!this->m_lastSearchBuffer->empty()) {
                ++this->m_lastSearchIndex %= this->m_lastSearchBuffer->size();
                this->m_memoryEditor.GotoAddrAndSelect((*this->m_lastSearchBuffer)[this->m_lastSearchIndex].first,
                                                          (*this->m_lastSearchBuffer)[this->m_lastSearchIndex].second);
            }
        };

        static auto FindPrevious = [this]() {
            if (!this->m_lastSearchBuffer->empty()) {
                this->m_lastSearchIndex--;

                if (this->m_lastSearchIndex < 0)
                    this->m_lastSearchIndex = this->m_lastSearchBuffer->size() - 1;

                this->m_lastSearchIndex %= this->m_lastSearchBuffer->size();

                this->m_memoryEditor.GotoAddrAndSelect((*this->m_lastSearchBuffer)[this->m_lastSearchIndex].first,
                                                          (*this->m_lastSearchBuffer)[this->m_lastSearchIndex].second);
            }
        };


        ImGui::SetNextWindowPos(ImGui::GetWindowPos() + ImGui::GetWindowContentRegionMin() - ImGui::GetStyle().WindowPadding);
        if (ImGui::BeginPopup("hex.builtin.view.hexeditor.menu.file.search"_lang)) {
            if (ImGui::BeginTabBar("searchTabs")) {
                std::vector<char> *currBuffer = nullptr;
                if (ImGui::BeginTabItem("hex.builtin.view.hexeditor.search.string"_lang)) {
                    this->m_searchFunction = findString;
                    this->m_lastSearchBuffer = &this->m_lastStringSearch;
                    currBuffer = &this->m_searchStringBuffer;

                    ImGui::InputText("##nolabel", currBuffer->data(), currBuffer->size(), ImGuiInputTextFlags_CallbackCompletion,
                                     InputCallback, this);
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("hex.builtin.view.hexeditor.search.hex"_lang)) {
                    this->m_searchFunction = findHex;
                    this->m_lastSearchBuffer = &this->m_lastHexSearch;
                    currBuffer = &this->m_searchHexBuffer;

                    ImGui::InputText("##nolabel", currBuffer->data(), currBuffer->size(),
                                     ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CallbackCompletion,
                                     InputCallback, this);
                    ImGui::EndTabItem();
                }

                if (currBuffer != nullptr) {
                    if (ImGui::Button("hex.builtin.view.hexeditor.search.find"_lang))
                        Find(currBuffer->data());

                    if (!this->m_lastSearchBuffer->empty()) {
                        if ((ImGui::Button("hex.builtin.view.hexeditor.search.find_next"_lang)))
                            FindNext();

                        ImGui::SameLine();

                        if ((ImGui::Button("hex.builtin.view.hexeditor.search.find_prev"_lang)))
                            FindPrevious();
                    }
                }

                ImGui::EndTabBar();
            }

            ImGui::EndPopup();
        }
    }

    void ViewHexEditor::drawGotoPopup() {
        auto provider = ImHexApi::Provider::get();
        auto baseAddress = provider->getBaseAddress();
        auto dataSize = provider->getActualSize();

        ImGui::SetNextWindowPos(ImGui::GetWindowPos() + ImGui::GetWindowContentRegionMin() - ImGui::GetStyle().WindowPadding);
        if (ImGui::BeginPopup("hex.builtin.view.hexeditor.menu.file.goto"_lang)) {
            if (ImGui::BeginTabBar("gotoTabs")) {
                u64 newOffset = 0;
                if (ImGui::BeginTabItem("hex.builtin.view.hexeditor.goto.offset.absolute"_lang)) {
                    ImGui::InputScalar("hex", ImGuiDataType_U64, &this->m_gotoAddress, nullptr, nullptr, "%llx", ImGuiInputTextFlags_CharsHexadecimal);

                    if (this->m_gotoAddress < baseAddress || this->m_gotoAddress > baseAddress + dataSize)
                        this->m_gotoAddress = baseAddress;

                    newOffset = this->m_gotoAddress;

                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("hex.builtin.view.hexeditor.goto.offset.begin"_lang)) {
                    ImGui::InputScalar("hex", ImGuiDataType_U64, &this->m_gotoAddress, nullptr, nullptr, "%llx", ImGuiInputTextFlags_CharsHexadecimal);

                    if (this->m_gotoAddress < 0 || this->m_gotoAddress > dataSize)
                        this->m_gotoAddress = 0;

                    newOffset = this->m_gotoAddress + baseAddress;

                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("hex.builtin.view.hexeditor.goto.offset.current"_lang)) {
                    ImGui::InputScalar("dec", ImGuiDataType_S64, &this->m_gotoAddress, nullptr, nullptr, "%lld", ImGuiInputTextFlags_CharsDecimal);

                    s64 currSelectionOffset = std::min(this->m_memoryEditor.DataPreviewAddr, this->m_memoryEditor.DataPreviewAddrEnd);

                    if (currSelectionOffset + this->m_gotoAddress < 0)
                        this->m_gotoAddress = -currSelectionOffset;
                    else if (currSelectionOffset + this->m_gotoAddress > dataSize)
                        this->m_gotoAddress = dataSize - currSelectionOffset;

                    newOffset = currSelectionOffset + this->m_gotoAddress + baseAddress;

                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("hex.builtin.view.hexeditor.goto.offset.end"_lang)) {
                    ImGui::InputScalar("hex", ImGuiDataType_U64, &this->m_gotoAddress, nullptr, nullptr, "%llx", ImGuiInputTextFlags_CharsHexadecimal);

                    if (this->m_gotoAddress < 0 || this->m_gotoAddress > dataSize)
                        this->m_gotoAddress = 0;

                    newOffset = (baseAddress + dataSize) - this->m_gotoAddress - 1;

                    ImGui::EndTabItem();
                }

                if (ImGui::Button("hex.builtin.view.hexeditor.menu.file.goto"_lang)) {
                    provider->setCurrentPage(std::floor(double(newOffset - baseAddress) / hex::prv::Provider::PageSize));
                    EventManager::post<RequestSelectionChange>(Region { newOffset, 1 });
                }

                ImGui::EndTabBar();
            }

            ImGui::EndPopup();
        }
    }

    void ViewHexEditor::drawEditPopup() {
        auto provider = ImHexApi::Provider::get();
        bool providerValid = ImHexApi::Provider::isValid();
        if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.edit.undo"_lang, "CTRL + Z", false, providerValid))
            provider->undo();
        if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.edit.redo"_lang, "CTRL + Y", false, providerValid))
            provider->redo();

        ImGui::Separator();

        bool bytesSelected = this->m_memoryEditor.DataPreviewAddr != -1 && this->m_memoryEditor.DataPreviewAddrEnd != -1;

        if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.edit.copy"_lang, "CTRL + C", false, bytesSelected))
            this->copyBytes();

        if (ImGui::BeginMenu("hex.builtin.view.hexeditor.menu.edit.copy_as"_lang, bytesSelected)) {
            if (ImGui::MenuItem("hex.builtin.view.hexeditor.copy.hex"_lang, "CTRL + SHIFT + C"))
                this->copyString();

            ImGui::Separator();

            for (const auto&[unlocalizedName, callback] : ContentRegistry::DataFormatter::getEntries()) {
                if (ImGui::MenuItem(LangEntry(unlocalizedName))) {
                    size_t start = std::min(this->m_memoryEditor.DataPreviewAddr, this->m_memoryEditor.DataPreviewAddrEnd);
                    size_t end = std::max(this->m_memoryEditor.DataPreviewAddr, this->m_memoryEditor.DataPreviewAddrEnd);

                    size_t copySize = (end - start) + 1;

                    ImGui::SetClipboardText(callback(provider, start + provider->getBaseAddress() + provider->getCurrentPageAddress(), copySize).c_str());
                }
            }

            ImGui::EndMenu();
        }

        if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.edit.paste"_lang, "CTRL + V", false, bytesSelected))
            this->pasteBytes();

        if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.edit.select_all"_lang, "CTRL + A", false, providerValid))
            EventManager::post<RequestSelectionChange>(Region { provider->getBaseAddress(), provider->getActualSize() });

        ImGui::Separator();

        if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.edit.bookmark"_lang, nullptr, false, this->m_memoryEditor.DataPreviewAddr != -1 && this->m_memoryEditor.DataPreviewAddrEnd != -1)) {
            auto base = ImHexApi::Provider::get()->getBaseAddress();

            size_t start = base + std::min(this->m_memoryEditor.DataPreviewAddr, this->m_memoryEditor.DataPreviewAddrEnd);
            size_t end = base + std::max(this->m_memoryEditor.DataPreviewAddr, this->m_memoryEditor.DataPreviewAddrEnd);

            ImHexApi::Bookmarks::add(start, end - start + 1, { }, { });
        }

        if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.edit.set_base"_lang, nullptr, false, providerValid && provider->isReadable())) {
            std::memset(this->m_baseAddressBuffer, 0x00, sizeof(this->m_baseAddressBuffer));
            View::doLater([]{ ImGui::OpenPopup("hex.builtin.view.hexeditor.menu.edit.set_base"_lang); });
        }

        if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.edit.resize"_lang, nullptr, false, providerValid && provider->isResizable())) {
            View::doLater([this]{
                this->m_resizeSize = ImHexApi::Provider::get()->getActualSize();
                ImGui::OpenPopup("hex.builtin.view.hexeditor.menu.edit.resize"_lang);
            });
        }

        if (ImGui::MenuItem("hex.builtin.view.hexeditor.menu.edit.insert"_lang, nullptr, false, providerValid && provider->isResizable())) {
            View::doLater([this]{
                this->m_resizeSize = 0;
                ImGui::OpenPopup("hex.builtin.view.hexeditor.menu.edit.insert"_lang);
            });
        }
    }

    void ViewHexEditor::registerEvents() {
        EventManager::subscribe<RequestOpenFile>(this, [this](const auto &path) {
            this->openFile(path);
            this->getWindowOpenState() = true;
        });

        EventManager::subscribe<RequestSelectionChange>(this, [this](Region region) {
            auto provider = ImHexApi::Provider::get();
            auto page = provider->getPageOfAddress(region.address);

            if (!page.has_value())
                return;

            if (region.size != 0) {
                provider->setCurrentPage(page.value());
                u64 start = region.address - provider->getBaseAddress() - provider->getCurrentPageAddress();
                this->m_memoryEditor.GotoAddrAndSelect(start, start + region.size - 1);
            }

            EventManager::post<EventRegionSelected>(Region { this->m_memoryEditor.DataPreviewAddr, (this->m_memoryEditor.DataPreviewAddrEnd - this->m_memoryEditor.DataPreviewAddr) + 1});
        });

        EventManager::subscribe<EventProjectFileLoad>(this, []() {
            EventManager::post<RequestOpenFile>(ProjectFile::getFilePath());
        });

        EventManager::subscribe<EventWindowClosing>(this, [](GLFWwindow *window) {
            if (ProjectFile::hasUnsavedChanges()) {
                glfwSetWindowShouldClose(window, GLFW_FALSE);
                View::doLater([] { ImGui::OpenPopup("hex.builtin.view.hexeditor.exit_application.title"_lang); });
            }
        });

        EventManager::subscribe<RequestOpenWindow>(this, [this](std::string name) {
            if (name == "Create File") {
                hex::openFileBrowser("hex.builtin.view.hexeditor.create_file"_lang, DialogMode::Save, { }, [this](const auto &path) {
                    File file(path, File::Mode::Create);

                    if (!file.isValid()) {
                        View::showErrorPopup("hex.builtin.view.hexeditor.error.create"_lang);
                        return;
                    }

                    file.setSize(1);

                    EventManager::post<RequestOpenFile>(path);
                    this->getWindowOpenState() = true;
                });
            } else if (name == "Open File") {
                hex::openFileBrowser("hex.builtin.view.hexeditor.open_file"_lang, DialogMode::Open, { }, [this](const auto &path) {
                    EventManager::post<RequestOpenFile>(path);
                    this->getWindowOpenState() = true;
                });
            } else if (name == "Open Project") {
                hex::openFileBrowser("hex.builtin.view.hexeditor.open_project"_lang, DialogMode::Open, { { "Project File", "hexproj" } }, [this](const auto &path) {
                    ProjectFile::load(path);
                    this->getWindowOpenState() = true;
                });
            }
        });

        EventManager::subscribe<EventSettingsChanged>(this, [this] {
            {
                auto alpha = ContentRegistry::Settings::getSetting("hex.builtin.setting.interface", "hex.builtin.setting.interface.highlight_alpha");

                if (alpha.is_number())
                    this->m_highlightAlpha = alpha;
            }

            {
                auto columnCount = ContentRegistry::Settings::getSetting("hex.builtin.setting.hex_editor", "hex.builtin.setting.hex_editor.column_count");

                if (columnCount.is_number())
                    this->m_memoryEditor.Cols = static_cast<int>(columnCount);
            }

            {
                auto hexii = ContentRegistry::Settings::getSetting("hex.builtin.setting.hex_editor", "hex.builtin.setting.hex_editor.hexii");

                if (hexii.is_number())
                    this->m_memoryEditor.OptShowHexII = static_cast<int>(hexii);
            }

            {
                auto ascii = ContentRegistry::Settings::getSetting("hex.builtin.setting.hex_editor", "hex.builtin.setting.hex_editor.ascii");

                if (ascii.is_number())
                    this->m_memoryEditor.OptShowAscii = static_cast<int>(ascii);
            }

            {
                auto advancedDecoding = ContentRegistry::Settings::getSetting("hex.builtin.setting.hex_editor", "hex.builtin.setting.hex_editor.advanced_decoding");

                if (advancedDecoding.is_number())
                    this->m_advancedDecodingEnabled = static_cast<int>(advancedDecoding);
            }

            {
                auto greyOutZeros = ContentRegistry::Settings::getSetting("hex.builtin.setting.hex_editor", "hex.builtin.setting.hex_editor.grey_zeros");

                if (greyOutZeros.is_number())
                    this->m_memoryEditor.OptGreyOutZeroes = static_cast<int>(greyOutZeros);
            }

            {
                auto upperCaseHex = ContentRegistry::Settings::getSetting("hex.builtin.setting.hex_editor", "hex.builtin.setting.hex_editor.uppercase_hex");

                if (upperCaseHex.is_number())
                    this->m_memoryEditor.OptUpperCaseHex = static_cast<int>(upperCaseHex);
            }

            {
                auto showExtraInfo = ContentRegistry::Settings::getSetting("hex.builtin.setting.hex_editor", "hex.builtin.setting.hex_editor.extra_info");

                if (showExtraInfo.is_number())
                    this->m_memoryEditor.OptShowExtraInfo = static_cast<int>(showExtraInfo);
            }
        });

        EventManager::subscribe<QuerySelection>(this, [this](auto &region) {
            u64 address = std::min(this->m_memoryEditor.DataPreviewAddr, this->m_memoryEditor.DataPreviewAddrEnd);
            size_t size = std::abs(s64(this->m_memoryEditor.DataPreviewAddrEnd) - s64(this->m_memoryEditor.DataPreviewAddr)) + 1;

            region = Region { address, size };
        });
    }

    void ViewHexEditor::registerShortcuts() {
        ShortcutManager::addGlobalShortcut(CTRL + Keys::S, [] {
            save();
        });

        ShortcutManager::addGlobalShortcut(CTRL + SHIFT + Keys::S, [] {
            saveAs();
        });


        ShortcutManager::addShortcut(this, CTRL + Keys::Z, [] {
            if (ImHexApi::Provider::isValid())
                ImHexApi::Provider::get()->undo();
        });

        ShortcutManager::addShortcut(this, CTRL + Keys::Y, [] {
            if (ImHexApi::Provider::isValid())
                ImHexApi::Provider::get()->redo();
        });


        ShortcutManager::addShortcut(this, CTRL + Keys::F, [] {
            ImGui::OpenPopupInWindow(View::toWindowName("hex.builtin.view.hexeditor.name").c_str(), "hex.builtin.view.hexeditor.menu.file.search"_lang);
        });

        ShortcutManager::addShortcut(this, CTRL + Keys::G, [] {
            ImGui::OpenPopupInWindow(View::toWindowName("hex.builtin.view.hexeditor.name").c_str(), "hex.builtin.view.hexeditor.menu.file.goto"_lang);
        });

        ShortcutManager::addShortcut(this, CTRL + Keys::O, [] {
            hex::openFileBrowser("hex.builtin.view.hexeditor.open_file"_lang, DialogMode::Open, { }, [](const auto &path) {
                EventManager::post<RequestOpenFile>(path);
            });
        });


        ShortcutManager::addShortcut(this, CTRL + Keys::C, [this] {
            this->copyBytes();
        });

        ShortcutManager::addShortcut(this, CTRL + SHIFT + Keys::C, [this] {
            this->copyString();
        });

        ShortcutManager::addShortcut(this, CTRL + Keys::V, [this] {
            this->pasteBytes();
        });

        ShortcutManager::addShortcut(this, CTRL + Keys::A, [] {
            auto provider = ImHexApi::Provider::get();
            EventManager::post<RequestSelectionChange>(Region { provider->getBaseAddress(), provider->getActualSize() });
        });

    }

}