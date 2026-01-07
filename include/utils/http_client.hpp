/**
 * VitaPlex - HTTP Client
 * Using libcurl for network requests
 */

#pragma once

#include <string>
#include <map>
#include <functional>

namespace vitaplex {

// HTTP response
struct HttpResponse {
    int statusCode = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    std::string error;
    bool success = false;
};

// HTTP request configuration
struct HttpRequest {
    std::string url;
    std::string method = "GET";
    std::string body;
    std::map<std::string, std::string> headers;
    int timeout = 30;
    bool followRedirects = true;
};

/**
 * HTTP Client using libcurl
 */
class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    // Initialize/cleanup (call once globally)
    static bool globalInit();
    static void globalCleanup();

    // Simple requests
    HttpResponse get(const std::string& url);
    HttpResponse post(const std::string& url, const std::string& body,
                      const std::string& contentType = "application/json");
    HttpResponse put(const std::string& url, const std::string& body);
    HttpResponse del(const std::string& url);

    // Full request
    HttpResponse request(const HttpRequest& req);

    // Headers
    void setDefaultHeader(const std::string& key, const std::string& value);
    void removeDefaultHeader(const std::string& key);
    void clearDefaultHeaders();

    // Configuration
    void setTimeout(int seconds) { m_timeout = seconds; }
    void setFollowRedirects(bool follow) { m_followRedirects = follow; }
    void setUserAgent(const std::string& ua) { m_userAgent = ua; }

    // Simple get that returns body directly
    bool get(const std::string& url, std::string& response);

    // Download file with progress callbacks
    // writeCallback: receives data chunks, return false to cancel
    // sizeCallback: called with total file size when known
    using WriteCallback = std::function<bool(const char* data, size_t size)>;
    using SizeCallback = std::function<void(int64_t totalSize)>;
    bool downloadFile(const std::string& url, WriteCallback writeCallback, SizeCallback sizeCallback = nullptr);

    // URL encoding
    static std::string urlEncode(const std::string& str);
    static std::string urlDecode(const std::string& str);

private:
    void* m_curl = nullptr;
    int m_timeout = 30;
    bool m_followRedirects = true;
    std::string m_userAgent;
    std::map<std::string, std::string> m_defaultHeaders;

    static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp);
    static size_t headerCallback(void* contents, size_t size, size_t nmemb, void* userp);
};

} // namespace vitaplex
