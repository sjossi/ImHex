#pragma once

#include <hex.hpp>

#include <cstring>
#include <future>
#include <optional>
#include <string>
#include <filesystem>
#include <atomic>

#include <nlohmann/json_fwd.hpp>
#include <curl/system.h>

#include <hex/helpers/paths.hpp>

using CURL = void;
struct curl_slist;

namespace hex {

    template<typename T>
    struct Response {
        s32 code;
        T body;
    };

    template<>
    struct Response<void> {
        s32 code;
    };

    class Net {
    public:
        Net();
        ~Net();

        std::future<Response<std::string>> getString(const std::string &url, u32 timeout = 2000);
        std::future<Response<nlohmann::json>> getJson(const std::string &url, u32 timeout = 2000);

        std::future<Response<std::string>> uploadFile(const std::string &url, const fs::path &filePath, u32 timeout = 2000);
        std::future<Response<void>> downloadFile(const std::string &url, const fs::path &filePath, u32 timeout = 2000);

        [[nodiscard]]
        std::string encode(const std::string &input);

        [[nodiscard]]
        float getProgress() const { return this->m_progress; }

        void cancel() { this->m_shouldCancel = true; }

    private:
        void setCommonSettings(std::string &response, const std::string &url, u32 timeout = 2000, const std::map<std::string, std::string> &extraHeaders = { }, const std::string &body = { });
        std::optional<s32> execute();

        friend int progressCallback(void *contents, curl_off_t dlTotal, curl_off_t dlNow, curl_off_t ulTotal, curl_off_t ulNow);
    private:
        CURL *m_ctx;
        curl_slist *m_headers = nullptr;

        std::mutex m_transmissionActive;
        float m_progress = 0.0F;
        bool m_shouldCancel = false;
    };

}