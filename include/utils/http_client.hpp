/**
 * VitaPlex - HTTP Client
 * Using libcurl for network requests
 */

#pragma once

#include <string>
#include <map>
#include <functional>
#include <cstdint>

namespace vitaplex {

// Redact sensitive query parameters (auth tokens) before logging a URL.
// Any call site that logs a URL which may carry "X-Plex-Token=..." must go
// through this, or the user's long-lived token ends up in on-device logs
// that get shared for debugging.
std::string redactTokensInUrl(const std::string& url);

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

    // Stop receiving as soon as the response body contains one complete,
    // brace-balanced top-level JSON object, then return what we have.
    //
    // Needed for endpoints that return their JSON payload up front but keep
    // the HTTP response open afterwards (e.g. the Live TV tune, which is a
    // long-lived "rolling subscription": the Media/session metadata arrives
    // immediately but the server holds the chunked stream open for the life
    // of the recording, so a normal read blocks until the request times out).
    bool stopAtJsonClose = false;
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

    // Simple methods with custom headers
    bool get(const std::string& url, std::string& response,
             const std::map<std::string, std::string>& headers);
    bool post(const std::string& url, const std::string& body, std::string& response,
              const std::map<std::string, std::string>& headers);
    bool put(const std::string& url, const std::string& body, std::string& response);
    bool del(const std::string& url, std::string& response);

    // Download file with progress callbacks
    // writeCallback: receives data chunks, return false to cancel
    // sizeCallback: called with total file size when known
    using WriteCallback = std::function<bool(const char* data, size_t size)>;
    using SizeCallback = std::function<void(int64_t totalSize)>;
    // startCallback (optional): invoked exactly once, right before the first
    // body byte is delivered (or after headers for an empty-body response),
    // with the FINAL HTTP status code and the full file size — the Content-Range
    // total for a 206 resume, the Content-Length for a 200, or -1 when unknown
    // (chunked). Lets a resuming caller open its output file in the correct mode
    // (append for 206, truncate for 200) before any data lands.
    using DownloadStartCallback = std::function<void(int statusCode, int64_t fullSize)>;
    // resumeOffset > 0 asks the server to continue from that byte (HTTP Range /
    // CURLOPT_RESUME_FROM). The server answers 206 (honoured) or 200 (full file)
    // — inspect statusCode in startCallback to decide append vs truncate.
    bool downloadFile(const std::string& url, WriteCallback writeCallback, SizeCallback sizeCallback = nullptr,
                      const std::map<std::string, std::string>& headers = {},
                      int64_t resumeOffset = 0,
                      DownloadStartCallback startCallback = nullptr);

    // URL encoding
    static std::string urlEncode(const std::string& str);
    static std::string urlDecode(const std::string& str);

    // Shared instance for API calls - reuses the same curl handle
    // to avoid curl_easy_init/cleanup overhead on every request.
    // NOT thread-safe - only use from the main/API thread.
    static HttpClient& shared();

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
