/**
 * VitaPlex - Plex Client for PlayStation Vita
 * 
 * HTTP Client implementation using libcurl
 */

#include "utils/http_client.h"
#include "app.h"  // For debugLog

#include <curl/curl.h>
#include <psp2/kernel/clib.h>
#include <cstring>
#include <cstdio>

namespace vitaplex {

// Curl write callback data
struct WriteCallbackData {
    std::string* buffer;
    int64_t totalSize;
};

bool HttpClient::globalInit() {
    CURLcode res = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (res != CURLE_OK) {
        debugLog("curl_global_init failed: %s\n", curl_easy_strerror(res));
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

int HttpClient::progressCallback(void* clientp, int64_t dltotal, int64_t dlnow,
                                 int64_t ultotal, int64_t ulnow) {
    ProgressCallback* cb = (ProgressCallback*)clientp;
    if (cb && *cb) {
        (*cb)(dlnow, dltotal);
    }
    return 0;
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
    
    // Set timeout
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, req.timeout > 0 ? req.timeout : m_timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10);
    
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
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, req.body.length());
        }
    } else if (req.method == "PUT") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        if (!req.body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, req.body.length());
        }
    } else if (req.method == "DELETE") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    }
    
    // Perform request
    debugLog("HTTP %s %s\n", req.method.c_str(), req.url.c_str());
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
        
        debugLog("HTTP response: %d (%d bytes)\n", response.statusCode, (int)response.body.length());
    } else {
        response.error = curl_easy_strerror(res);
        debugLog("HTTP error: %s\n", response.error.c_str());
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

HttpResponse HttpClient::downloadToMemory(const std::string& url, ProgressCallback progress) {
    HttpRequest req;
    req.url = url;
    req.method = "GET";
    
    // TODO: Implement progress callback
    
    return request(req);
}

bool HttpClient::downloadToFile(const std::string& url, const std::string& filepath,
                                ProgressCallback progress) {
    // TODO: Implement file download
    return false;
}

std::string HttpClient::urlEncode(const std::string& str) {
    std::string encoded;
    static const char* hex = "0123456789ABCDEF";
    
    for (unsigned char c : str) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
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

} // namespace vitaplex
