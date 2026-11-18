#pragma once
#include "TranslationTypes.h"

namespace ChromaTranslate {

    class TranslationRouter {
    public:
        explicit TranslationRouter(const TranslatorConfig& config);
        ~TranslationRouter();

        void TranslateBlocks(std::vector<TextBlock>& blocks);

        void UpdateConfig(const TranslatorConfig& config) {
            if (m_config.sourceLang != config.sourceLang || m_config.targetLang != config.targetLang) {
                m_cache.clear();
            }
            m_config = config;
        }

    private:
        // Tier 1: Google Translate gtx endpoint (&dj=1 JSON object)
        bool TranslateSingleGoogleGtx(const std::wstring& sourceText, std::wstring& outTranslated);

        // Tier 2: Google Chrome Dictionary Extension endpoint (clients5.google.com)
        bool TranslateSingleGoogleClients5(const std::wstring& sourceText, std::wstring& outTranslated);

        // Tier 3A: Microsoft Edge Built-In Keyless Translator API
        bool TranslateSingleEdgeFallback(const std::wstring& sourceText, std::wstring& outTranslated);

        // Tier 3B: MyMemory Public Keyless REST API
        bool TranslateSingleMyMemoryFallback(const std::wstring& sourceText, std::wstring& outTranslated);

        bool RefreshEdgeAuthToken();
        std::string ExecuteHttpsRequest(
            const wchar_t* host,
            const wchar_t* verb,
            const std::wstring& path,
            const std::wstring& extraHeaders = L"",
            const std::string& body = "");

        static std::wstring ExtractJsonFieldValues(const std::string& json, const std::string& keyName);
        static std::wstring ExtractFirstJsonArrayString(const std::string& json);
        static std::string WideToUtf8(const std::wstring& wstr);
        static std::wstring Utf8ToWide(const std::string& str);
        static std::wstring UrlEncodeUtf8(const std::wstring& wstr);
        static std::wstring EscapeJsonString(const std::wstring& wstr);

    private:
        TranslatorConfig m_config;
        HINTERNET m_hSession = nullptr;

        std::unordered_map<std::wstring, std::wstring> m_cache;
        static constexpr size_t MAX_CACHE_ENTRIES = 4096;

        std::wstring m_edgeAuthToken;
        uint64_t m_edgeTokenExpiryMs = 0;
    };

} // namespace ChromaTranslate