#pragma once

#include <string>
#include <map>
#include <vector>
#include <functional>

// Define Plex client info for HTTP User-Agent
#ifndef PLEX_CLIENT_NAME
#define PLEX_CLIENT_NAME "VitaPlex"
#endif
#ifndef PLEX_CLIENT_VERSION
#define PLEX_CLIENT_VERSION "1.0.0"
#endif
#ifndef PLEX_PLATFORM
#define PLEX_PLATFORM "PlayStation Vita"
#endif

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
    int timeout = 30;  // seconds
    bool followRedirects = true;
};

/**
 * HTTP Client using libcurl
 */
class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    
    // Initialize/cleanup (call once)
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
    
    // Download with progress
    using ProgressCallback = std::function<void(int64_t downloaded, int64_t total)>;
    HttpResponse downloadToMemory(const std::string& url, ProgressCallback progress = nullptr);
    bool downloadToFile(const std::string& url, const std::string& filepath, 
                       ProgressCallback progress = nullptr);
    
    // URL encoding
    static std::string urlEncode(const std::string& str);
    static std::string urlDecode(const std::string& str);
    
private:
    void* m_curl = nullptr;
    int m_timeout = 30;
    bool m_followRedirects = true;
    std::string m_userAgent;
    std::map<std::string, std::string> m_defaultHeaders;
    
    void setupRequest(void* curl, const HttpRequest& req);
    static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp);
    static size_t headerCallback(void* contents, size_t size, size_t nmemb, void* userp);
    static int progressCallback(void* clientp, int64_t dltotal, int64_t dlnow, 
                                int64_t ultotal, int64_t ulnow);
};

} // namespace vitaplex
