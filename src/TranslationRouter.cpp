#include "TranslationRouter.h"
#include <iomanip>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

namespace ChromaTranslate {

    TranslationRouter::TranslationRouter(const TranslatorConfig& config)
        : m_config(config)
    {
        m_hSession = WinHttpOpen(
            L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);

        if (!m_hSession) {
            m_hSession = WinHttpOpen(
                L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                0);
        }

        if (m_hSession) {
            DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
            WinHttpSetOption(m_hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        }
    }

    TranslationRouter::~TranslationRouter() {
        if (m_hSession) WinHttpCloseHandle(m_hSession);
    }

    void TranslationRouter::TranslateBlocks(std::vector<TextBlock>& blocks) {
        if (blocks.empty() || !m_hSession) return;

        if (m_cache.size() > MAX_CACHE_ENTRIES) {
            m_cache.clear();
        }

        for (auto& block : blocks) {
            if (block.originalText.empty()) continue;

            auto it = m_cache.find(block.originalText);
            if (it != m_cache.end()) {
                block.translatedText = it->second;
                continue;
            }

            std::wstring translated;
            bool ok = TranslateSingleGoogleGtx(block.originalText, translated) ||
                      TranslateSingleGoogleClients5(block.originalText, translated) ||
                      TranslateSingleEdgeFallback(block.originalText, translated) ||
                      TranslateSingleMyMemoryFallback(block.originalText, translated);

            if (ok && !translated.empty()) {
                block.translatedText = translated;
                m_cache[block.originalText] = translated;
            } else {
                block.translatedText = L"[NET-ERR] " + block.originalText;
            }
        }
    }

    std::string TranslationRouter::ExecuteHttpsRequest(
        const wchar_t* host,
        const wchar_t* verb,
        const std::wstring& path,
        const std::wstring& extraHeaders,
        const std::string& body)
    {
        if (!m_hSession) return {};

        HINTERNET hConnect = WinHttpConnect(m_hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) return {};

        HINTERNET hRequest = WinHttpOpenRequest(
            hConnect, verb, path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

        std::string responseBody;
        if (hRequest) {
            LPCWSTR hdrPtr = extraHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extraHeaders.c_str();
            DWORD hdrLen = extraHeaders.empty() ? 0 : static_cast<DWORD>(extraHeaders.length());
            LPVOID bodyPtr = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
            DWORD bodyLen = static_cast<DWORD>(body.size());

            if (WinHttpSendRequest(hRequest, hdrPtr, hdrLen, bodyPtr, bodyLen, bodyLen, 0) &&
                WinHttpReceiveResponse(hRequest, nullptr))
            {
                DWORD statusCode = 0, size = sizeof(statusCode);
                WinHttpQueryHeaders(
                    hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size, WINHTTP_NO_HEADER_INDEX);

                if (statusCode == 200) {
                    DWORD avail = 0;
                    while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
                        std::string chunk(avail, '\0');
                        DWORD read = 0;
                        WinHttpReadData(hRequest, chunk.data(), avail, &read);
                        responseBody.append(chunk, 0, read);
                    }
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
        return responseBody;
    }

    bool TranslationRouter::TranslateSingleGoogleGtx(
        const std::wstring& sourceText,
        std::wstring& outTranslated)
    {
        std::wstring sl = (m_config.sourceLang == m_config.targetLang) ? L"auto" : m_config.sourceLang;
        std::wstring path = L"/translate_a/single?client=gtx&sl=" + sl +
                            L"&tl=" + m_config.targetLang +
                            L"&dt=t&dj=1&ie=UTF-8&oe=UTF-8&q=" + UrlEncodeUtf8(sourceText);

        std::string resp = ExecuteHttpsRequest(L"translate.googleapis.com", L"GET", path);
        if (resp.empty()) return false;

        outTranslated = ExtractJsonFieldValues(resp, "trans");
        return !outTranslated.empty();
    }

    bool TranslationRouter::TranslateSingleGoogleClients5(
        const std::wstring& sourceText,
        std::wstring& outTranslated)
    {
        std::wstring sl = (m_config.sourceLang == m_config.targetLang) ? L"auto" : m_config.sourceLang;
        std::wstring path = L"/translate_a/t?client=dict-chrome-ex&sl=" + sl +
                            L"&tl=" + m_config.targetLang +
                            L"&ie=UTF-8&oe=UTF-8&q=" + UrlEncodeUtf8(sourceText);

        std::string resp = ExecuteHttpsRequest(L"clients5.google.com", L"GET", path);
        if (resp.empty()) return false;

        outTranslated = ExtractJsonFieldValues(resp, "trans");
        if (outTranslated.empty()) {
            outTranslated = ExtractFirstJsonArrayString(resp);
        }
        return !outTranslated.empty();
    }

    bool TranslationRouter::RefreshEdgeAuthToken() {
        uint64_t now = GetTickCount64();
        if (!m_edgeAuthToken.empty() && now < m_edgeTokenExpiryMs) return true;

        std::string token = ExecuteHttpsRequest(L"edge.microsoft.com", L"GET", L"/translate/auth");
        if (token.empty()) return false;

        m_edgeAuthToken = Utf8ToWide(token);
        m_edgeTokenExpiryMs = now + (8 * 60 * 1000);
        return true;
    }

    bool TranslationRouter::TranslateSingleEdgeFallback(
        const std::wstring& sourceText,
        std::wstring& outTranslated)
    {
        if (!RefreshEdgeAuthToken()) return false;

        std::wstring wPayload = L"[{\"Text\":\"" + EscapeJsonString(sourceText) + L"\"}]";
        std::string utf8Payload = WideToUtf8(wPayload);

        std::wstring path = L"/translate?api-version=3.0&to=" + m_config.targetLang;
        if (m_config.sourceLang != L"auto" && m_config.sourceLang != m_config.targetLang) {
            path += L"&from=" + m_config.sourceLang;
        }

        std::wstring headers = L"Content-Type: application/json; charset=UTF-8\r\nAuthorization: Bearer " +
                               m_edgeAuthToken + L"\r\n";

        std::string resp = ExecuteHttpsRequest(
            L"api.cognitive.microsofttranslator.com", L"POST", path, headers, utf8Payload);
        if (resp.empty()) return false;

        outTranslated = ExtractJsonFieldValues(resp, "text");
        return !outTranslated.empty();
    }

    bool TranslationRouter::TranslateSingleMyMemoryFallback(
        const std::wstring& sourceText,
        std::wstring& outTranslated)
    {
        std::wstring src = (m_config.sourceLang == L"auto" || m_config.sourceLang == m_config.targetLang)
            ? L"ja"
            : m_config.sourceLang;

        std::wstring path = L"/get?q=" + UrlEncodeUtf8(sourceText) +
                            L"&langpair=" + src + L"|" + m_config.targetLang;

        std::string resp = ExecuteHttpsRequest(L"api.mymemory.translated.net", L"GET", path);
        if (resp.empty()) return false;

        outTranslated = ExtractJsonFieldValues(resp, "translatedText");
        return !outTranslated.empty();
    }

    std::wstring TranslationRouter::ExtractFirstJsonArrayString(const std::string& json) {
        size_t firstQuote = json.find('"');
        if (firstQuote == std::string::npos) return {};

        size_t pos = firstQuote + 1;
        std::string rawUtf8;
        while (pos < json.size()) {
            char c = json[pos++];
            if (c == '"') break;
            if (c == '\\' && pos < json.size()) {
                char esc = json[pos++];
                if (esc == 'n' || esc == 't') rawUtf8 += ' ';
                else if (esc != 'r') rawUtf8 += esc;
            } else {
                rawUtf8 += c;
            }
        }
        return Utf8ToWide(rawUtf8);
    }

    std::wstring TranslationRouter::ExtractJsonFieldValues(
        const std::string& json,
        const std::string& keyName)
    {
        std::string pattern = "\"" + keyName + "\"";
        std::wstring combined;
        size_t searchPos = 0;

        while ((searchPos = json.find(pattern, searchPos)) != std::string::npos) {
            size_t pos = searchPos + pattern.size();
            while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':')) {
                ++pos;
            }
            if (pos >= json.size() || json[pos] != '"') {
                searchPos = pos;
                continue;
            }
            ++pos;

            std::string rawUtf8;
            while (pos < json.size()) {
                char c = json[pos++];
                if (c == '"') break;
                if (c == '\\' && pos < json.size()) {
                    char esc = json[pos++];
                    switch (esc) {
                    case '"':  rawUtf8 += '"'; break;
                    case '\\': rawUtf8 += '\\'; break;
                    case '/':  rawUtf8 += '/'; break;
                    case 'b':  rawUtf8 += '\b'; break;
                    case 'f':  rawUtf8 += '\f'; break;
                    case 'n':  rawUtf8 += ' '; break;
                    case 'r':  break;
                    case 't':  rawUtf8 += ' '; break;
                    case 'u':
                        if (pos + 4 <= json.size()) {
                            try {
                                unsigned int cp = std::stoul(json.substr(pos, 4), nullptr, 16);
                                pos += 4;
                                wchar_t wc = static_cast<wchar_t>(cp);
                                rawUtf8 += WideToUtf8(std::wstring(1, wc));
                            } catch (...) {}
                        }
                        break;
                    default:
                        rawUtf8 += esc;
                        break;
                    }
                } else {
                    rawUtf8 += c;
                }
            }

            combined += Utf8ToWide(rawUtf8);
            searchPos = pos;
        }
        return combined;
    }

    std::string TranslationRouter::WideToUtf8(const std::wstring& wstr) {
        if (wstr.empty()) return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
        std::string out(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), out.data(), len, nullptr, nullptr);
        return out;
    }

    std::wstring TranslationRouter::Utf8ToWide(const std::string& str) {
        if (str.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
        std::wstring out(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), out.data(), len);
        return out;
    }

    std::wstring TranslationRouter::UrlEncodeUtf8(const std::wstring& wstr) {
        std::string utf8 = WideToUtf8(wstr);
        std::wostringstream escaped;
        escaped.fill(L'0');
        escaped << std::hex << std::uppercase;

        for (unsigned char c : utf8) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
                escaped << static_cast<wchar_t>(c);
            } else {
                escaped << L'%' << std::setw(2) << static_cast<unsigned int>(c);
            }
        }
        return escaped.str();
    }

    std::wstring TranslationRouter::EscapeJsonString(const std::wstring& wstr) {
        std::wstring out;
        out.reserve(wstr.size() + 16);
        for (wchar_t c : wstr) {
            if (c == L'"') out += L"\\\"";
            else if (c == L'\\') out += L"\\\\";
            else if (c == L'\n') out += L"\\n";
            else if (c != L'\r') out += c;
        }
        return out;
    }

} // namespace ChromaTranslate