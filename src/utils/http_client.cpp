/**
 * VitaPlex - HTTP Client implementation using libcurl
 */

#include "utils/http_client.hpp"
#include "app/application.hpp"

#include <borealis.hpp>
#include <curl/curl.h>
#include <cstring>
#include <cstdio>
#include <cctype>

namespace vitaplex {

// Curl write callback data
struct WriteCallbackData {
    std::string* buffer;
    int64_t totalSize;
};

bool HttpClient::globalInit() {
    CURLcode res = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (res != CURLE_OK) {
        brls::Logger::error("curl_global_init failed: {}", curl_easy_strerror(res));
        return false;
    }
    return true;
}

void HttpClient::globalCleanup() {
    curl_global_cleanup();
}

HttpClient::HttpClient() {
    m_curl = curl_easy_init();
    m_userAgent = PLEX_CLIENT_NAME "/" PLEX_CLIENT_VERSION " (" PLEX_PLATFORM ")";
}

HttpClient::~HttpClient() {
    if (m_curl) {
        curl_easy_cleanup((CURL*)m_curl);
        m_curl = nullptr;
    }
}

size_t HttpClient::writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    WriteCallbackData* data = (WriteCallbackData*)userp;

    if (data && data->buffer) {
        data->buffer->append((char*)contents, totalSize);
    }

    return totalSize;
}

size_t HttpClient::headerCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::map<std::string, std::string>* headers = (std::map<std::string, std::string>*)userp;

    if (headers) {
        std::string header((char*)contents, totalSize);
        size_t colonPos = header.find(':');
        if (colonPos != std::string::npos) {
            std::string key = header.substr(0, colonPos);
            std::string value = header.substr(colonPos + 1);

            // Trim whitespace
            while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) {
                value = value.substr(1);
            }
            while (!value.empty() && (value.back() == '\r' || value.back() == '\n' ||
                   value.back() == ' ' || value.back() == '\t')) {
                value.pop_back();
            }

            (*headers)[key] = value;
        }
    }

    return totalSize;
}

HttpResponse HttpClient::get(const std::string& url) {
    HttpRequest req;
    req.url = url;
    req.method = "GET";
    return request(req);
}

HttpResponse HttpClient::post(const std::string& url, const std::string& body,
                              const std::string& contentType) {
    HttpRequest req;
    req.url = url;
    req.method = "POST";
    req.body = body;
    req.headers["Content-Type"] = contentType;
    return request(req);
}

HttpResponse HttpClient::put(const std::string& url, const std::string& body) {
    HttpRequest req;
    req.url = url;
    req.method = "PUT";
    req.body = body;
    return request(req);
}

HttpResponse HttpClient::del(const std::string& url) {
    HttpRequest req;
    req.url = url;
    req.method = "DELETE";
    return request(req);
}

HttpResponse HttpClient::request(const HttpRequest& req) {
    HttpResponse response;

    if (!m_curl) {
        response.error = "CURL not initialized";
        return response;
    }

    CURL* curl = (CURL*)m_curl;

    // Reset curl handle
    curl_easy_reset(curl);

    // Set URL
    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());

    // Set timeout - use up to 60 second connect timeout for slow connections
    int timeout = req.timeout > 0 ? req.timeout : m_timeout;
    int connectTimeout = timeout > 60 ? 60 : (timeout > 30 ? 30 : 15);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connectTimeout);

    // Enable DNS caching for faster reconnects
    curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 300);  // 5 minutes

    // Follow redirects
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, req.followRedirects ? 1L : 0L);

    // SSL options (Vita specific)
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

    // User agent
    curl_easy_setopt(curl, CURLOPT_USERAGENT, m_userAgent.c_str());

    // Response buffer
    WriteCallbackData writeData;
    writeData.buffer = &response.body;
    writeData.totalSize = 0;

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writeData);

    // Headers callback
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

    // Build headers list
    struct curl_slist* headerList = nullptr;

    // Add default headers
    for (const auto& h : m_defaultHeaders) {
        std::string header = h.first + ": " + h.second;
        headerList = curl_slist_append(headerList, header.c_str());
    }

    // Add request-specific headers
    for (const auto& h : req.headers) {
        std::string header = h.first + ": " + h.second;
        headerList = curl_slist_append(headerList, header.c_str());
    }

    if (headerList) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    }

    // Set HTTP method and body
    if (req.method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (!req.body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req.body.length());
        }
    } else if (req.method == "PUT") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        if (!req.body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req.body.length());
        }
    } else if (req.method == "DELETE") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    }

    // Perform request
    brls::Logger::debug("HTTP {} {}", req.method, req.url);
    CURLcode res = curl_easy_perform(curl);

    // Cleanup headers
    if (headerList) {
        curl_slist_free_all(headerList);
    }

    // Check result
    if (res == CURLE_OK) {
        long httpCode;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        response.statusCode = (int)httpCode;
        response.success = (httpCode >= 200 && httpCode < 300);

        brls::Logger::debug("HTTP response: {} ({} bytes)", response.statusCode, response.body.length());
    } else {
        response.error = curl_easy_strerror(res);
        brls::Logger::error("HTTP error: {}", response.error);
    }

    return response;
}

void HttpClient::setDefaultHeader(const std::string& key, const std::string& value) {
    m_defaultHeaders[key] = value;
}

void HttpClient::removeDefaultHeader(const std::string& key) {
    m_defaultHeaders.erase(key);
}

void HttpClient::clearDefaultHeaders() {
    m_defaultHeaders.clear();
}

std::string HttpClient::urlEncode(const std::string& str) {
    std::string encoded;
    static const char* hex = "0123456789ABCDEF";

    for (unsigned char c : str) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += (char)c;
        } else if (c == ' ') {
            encoded += '+';
        } else {
            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 0x0F];
        }
    }

    return encoded;
}

std::string HttpClient::urlDecode(const std::string& str) {
    std::string decoded;

    for (size_t i = 0; i < str.length(); i++) {
        if (str[i] == '%' && i + 2 < str.length()) {
            int val = 0;
            char high = str[i + 1];
            char low = str[i + 2];

            if (high >= '0' && high <= '9') val = (high - '0') << 4;
            else if (high >= 'A' && high <= 'F') val = (high - 'A' + 10) << 4;
            else if (high >= 'a' && high <= 'f') val = (high - 'a' + 10) << 4;

            if (low >= '0' && low <= '9') val |= (low - '0');
            else if (low >= 'A' && low <= 'F') val |= (low - 'A' + 10);
            else if (low >= 'a' && low <= 'f') val |= (low - 'a' + 10);

            decoded += (char)val;
            i += 2;
        } else if (str[i] == '+') {
            decoded += ' ';
        } else {
            decoded += str[i];
        }
    }

    return decoded;
}

bool HttpClient::get(const std::string& url, std::string& response) {
    HttpResponse res = get(url);
    if (res.success) {
        response = res.body;
        return true;
    }
    return false;
}

// Download callback data structure
struct DownloadCallbackData {
    HttpClient::WriteCallback writeCallback;
    HttpClient::SizeCallback sizeCallback;
    bool sizeReported;
    bool cancelled;
};

static size_t downloadWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    DownloadCallbackData* data = static_cast<DownloadCallbackData*>(userp);
    size_t totalSize = size * nmemb;

    if (data && data->writeCallback) {
        // Call user's write callback
        if (!data->writeCallback(static_cast<const char*>(contents), totalSize)) {
            data->cancelled = true;
            return 0; // Return 0 to signal curl to abort
        }
    }

    return totalSize;
}

static size_t downloadHeaderCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    DownloadCallbackData* data = static_cast<DownloadCallbackData*>(userp);
    size_t totalSize = size * nmemb;

    if (data && data->sizeCallback && !data->sizeReported) {
        std::string header(static_cast<char*>(contents), totalSize);

        // Look for Content-Length header
        if (header.find("Content-Length:") == 0 || header.find("content-length:") == 0) {
            size_t colonPos = header.find(':');
            if (colonPos != std::string::npos) {
                std::string value = header.substr(colonPos + 1);
                // Trim whitespace
                while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) {
                    value = value.substr(1);
                }
                int64_t contentLength = std::stoll(value);
                data->sizeCallback(contentLength);
                data->sizeReported = true;
            }
        }
    }

    return totalSize;
}

bool HttpClient::downloadFile(const std::string& url, WriteCallback writeCallback, SizeCallback sizeCallback) {
    if (!m_curl) {
        brls::Logger::error("CURL not initialized for download");
        return false;
    }

    CURL* curl = (CURL*)m_curl;

    // Reset curl handle
    curl_easy_reset(curl);

    // Set URL
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    // Set longer timeout for downloads (10 minutes)
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);

    // Follow redirects
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    // SSL options (Vita specific)
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

    // User agent
    curl_easy_setopt(curl, CURLOPT_USERAGENT, m_userAgent.c_str());

    // Setup callback data
    DownloadCallbackData callbackData;
    callbackData.writeCallback = writeCallback;
    callbackData.sizeCallback = sizeCallback;
    callbackData.sizeReported = false;
    callbackData.cancelled = false;

    // Set callbacks
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, downloadWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &callbackData);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, downloadHeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &callbackData);

    // Low speed limit - abort if < 1KB/s for 30 seconds
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);

    brls::Logger::info("HttpClient: Starting download from {}", url);

    // Perform download
    CURLcode res = curl_easy_perform(curl);

    if (callbackData.cancelled) {
        brls::Logger::info("HttpClient: Download cancelled by user");
        return false;
    }

    if (res == CURLE_OK) {
        long httpCode;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        if (httpCode >= 200 && httpCode < 300) {
            brls::Logger::info("HttpClient: Download completed successfully");
            return true;
        } else {
            brls::Logger::error("HttpClient: Download failed with HTTP {}", httpCode);
            return false;
        }
    } else {
        brls::Logger::error("HttpClient: Download failed: {}", curl_easy_strerror(res));
        return false;
    }
}

} // namespace vitaplex
