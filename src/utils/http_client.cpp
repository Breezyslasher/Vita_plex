/**
 * VitaPlex - HTTP Client implementation using libcurl
 */

#include "utils/http_client.hpp"
#include "app/application.hpp"
#include "platform/platform.hpp"

#include <borealis.hpp>
#include <curl/curl.h>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <mutex>

namespace vitaplex {

// Redact sensitive query parameters (auth tokens) before logging a URL.
// Plex tokens are carried as "X-Plex-Token=..." in query strings, and many
// call sites log req.url at debug level. Without redaction, a user's long-
// lived auth token ends up in on-device logs and crash reports.
// Declared in http_client.hpp so every logging call site can share it.
std::string redactTokensInUrl(const std::string& url) {
    static const char* const kSensitiveKeys[] = {
        "X-Plex-Token=", "x-plex-token=", "token=",
    };
    std::string out = url;
    for (const char* key : kSensitiveKeys) {
        size_t pos = 0;
        while ((pos = out.find(key, pos)) != std::string::npos) {
            size_t valStart = pos + strlen(key);
            size_t valEnd = out.find_first_of("&#", valStart);
            if (valEnd == std::string::npos) valEnd = out.size();
            out.replace(valStart, valEnd - valStart, "[redacted]");
            pos = valStart + sizeof("[redacted]") - 1;
        }
    }
    return out;
}

// Process-wide curl share: DNS results, TLS session IDs and the connection
// cache are shared across every HttpClient instance. Subsystems each own a
// client (guide fetch, channel list, DVR checks, image loader, downloads),
// and without sharing, every one of them pays a fresh TCP + TLS handshake —
// ~300-600ms per call on Vita against a plex.direct host. With the share,
// later calls reuse a warm connection (or at least an abbreviated TLS
// resumption). The share is created once and intentionally never freed:
// it must outlive every easy handle, including ones alive at exit.
static std::mutex s_curlShareLocks[CURL_LOCK_DATA_LAST];

static void curlShareLock(CURL*, curl_lock_data data, curl_lock_access, void*) {
    s_curlShareLocks[data].lock();
}

static void curlShareUnlock(CURL*, curl_lock_data data, void*) {
    s_curlShareLocks[data].unlock();
}

static CURLSH* curlShare() {
    static CURLSH* share = []() -> CURLSH* {
        CURLSH* sh = curl_share_init();
        if (!sh) return nullptr;
        curl_share_setopt(sh, CURLSHOPT_LOCKFUNC, curlShareLock);
        curl_share_setopt(sh, CURLSHOPT_UNLOCKFUNC, curlShareUnlock);
        curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
        curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
        return sh;
    }();
    return share;
}

// Perf options shared by every request: connection/session reuse via the
// process-wide share, and transparent response compression. The EPG grid
// body is ~0.5MB of JSON that gzips ~10:1; ACCEPT_ENCODING "" advertises
// whatever encodings this libcurl build supports and decompresses in the
// write callback, so callers see plain bytes either way (and a zlib-less
// build simply sends no header — identity, same as before).
static void applyCurlPerfDefaults(CURL* curl) {
    if (CURLSH* sh = curlShare())
        curl_easy_setopt(curl, CURLOPT_SHARE, sh);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
}

// Apply baseline security-sensitive curl options shared by every request.
// Keeping this centralized makes it harder to accidentally ship a handle
// with verification off or with file:// / gopher:// left enabled.
static void applyCurlSecurityDefaults(CURL* curl) {
    // Restrict the protocols libcurl will dial to HTTP(S) only. Without this,
    // a malicious redirect (or a user-supplied URL) could coerce curl into
    // file://, dict://, gopher://, smb://, etc. — useful primitives for
    // SSRF / local-file disclosure. Redirects are restricted the same way.
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS,
                     (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                     (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));

    // Floor the TLS version at 1.2. Plex.tv has required TLS 1.2+ for years,
    // so this only blocks a downgrade attack, never a legitimate server.
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

#if defined(__vita__) || defined(__PS4__) || defined(__SWITCH__)
    // Console SSL backends (sceSsl on Vita/PS4, mbedtls on Switch) ship
    // without a usable CA bundle for libcurl, so peer verification cannot
    // succeed. Plex traffic on these devices is still confined to the LAN
    // or a user-chosen relay. If you later bundle a cacert.pem, swap these
    // to 1L/2L and point CURLOPT_CAINFO at the bundled file.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
#else
    // Desktop (Linux/macOS/Windows) and Android: verify both the cert
    // chain and the hostname — otherwise all traffic to plex.tv (login,
    // PIN exchange, token refresh) is MITM-able by anyone on the network
    // path.
    //
    // Android in particular ships libcurl built against the NDK with no
    // configured CA store, so an unqualified VERIFYPEER=1 fails every
    // HTTPS handshake (which is why "failed to request pin" was the
    // first symptom on Android). We ship Mozilla's bundle as
    // resources/cacert.pem and point CAINFO at it. The path resolves
    // via the platform's RESOURCE_PREFIX, which the Android asset
    // extractor places under the writable cwd so fopen() can read it.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, RESOURCE_PREFIX "cacert.pem");
#endif

    // Never deliver SIGALRM-based timeouts. curl's default timeout path
    // uses signals which are unsafe in a multi-threaded process (the app
    // performs requests from background threads).
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
}

// Curl write callback data
struct WriteCallbackData {
    std::string* buffer;
    int64_t totalSize;

    // Early-stop-at-top-level-JSON-close support (see HttpRequest::stopAtJsonClose).
    bool stopAtJsonClose = false;  // enable the brace-tracking below
    bool stopped = false;          // set true once a full object was received
    int  jsonDepth = 0;            // current brace nesting depth
    bool jsonSawOpen = false;      // have we seen the first '{' yet
    bool jsonInString = false;     // inside a JSON string literal
    bool jsonEscape = false;       // previous char was a backslash in a string
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

HttpClient& HttpClient::shared() {
    static HttpClient instance;
    return instance;
}

HttpClient::HttpClient() {
    m_curl = curl_easy_init();
    // User agent string combines the compile-time client id with the
    // platform-layer's runtime X-Plex-Platform value (e.g. "VitaPlex/2.0.0
    // (PlayStation Vita)"). Previously the platform part came from a
    // PLEX_PLATFORM ifdef macro; now it's a runtime lookup.
    m_userAgent = std::string(PLEX_CLIENT_NAME "/" PLEX_CLIENT_VERSION " (") +
                  platform::getVideoConstraints().plexPlatform + ")";
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

    if (!data || !data->buffer) {
        return totalSize;
    }

    if (!data->stopAtJsonClose) {
        data->buffer->append((char*)contents, totalSize);
        return totalSize;
    }

    // Append byte-by-byte while tracking JSON structure so we can stop the
    // transfer the instant the first complete top-level object is in hand.
    const char* bytes = (const char*)contents;
    for (size_t i = 0; i < totalSize; i++) {
        char c = bytes[i];
        data->buffer->push_back(c);

        if (data->jsonInString) {
            if (data->jsonEscape) {
                data->jsonEscape = false;
            } else if (c == '\\') {
                data->jsonEscape = true;
            } else if (c == '"') {
                data->jsonInString = false;
            }
            continue;
        }

        if (c == '"') {
            data->jsonInString = true;
        } else if (c == '{') {
            data->jsonDepth++;
            data->jsonSawOpen = true;
        } else if (c == '}') {
            data->jsonDepth--;
            if (data->jsonSawOpen && data->jsonDepth <= 0) {
                // Full top-level object received - abort the rest of the
                // (potentially never-ending) stream. Returning a short count
                // makes curl_easy_perform finish with CURLE_WRITE_ERROR,
                // which request() treats as success because `stopped` is set.
                data->stopped = true;
                return i + 1;
            }
        }
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

    // Pin HTTP/1.1.  curl_easy's default is CURL_HTTP_VERSION_2TLS, and HTTP/2
    // multiplexed responses can leave the easy interface blocked in
    // curl_easy_perform() waiting for END_STREAM on chunked/streamed bodies -
    // exactly what happens with the Live TV tune POST, which delivers its
    // success body (a ~77KB "live" MediaContainer) over a kept-alive
    // connection.  The transcode/download path in this same client already
    // pins HTTP/1.1 for the same reason, so we match that here.

    // Follow redirects
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, req.followRedirects ? 1L : 0L);
    // Cap redirect chains so a misbehaving server can't loop us forever.
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);

    // TLS verification, protocol allowlist, and signal safety.
    applyCurlSecurityDefaults(curl);
    applyCurlPerfDefaults(curl);

    // Force HTTP/1.1 (see comment above CURLOPT_DNS_CACHE_TIMEOUT).
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

    // User agent
    curl_easy_setopt(curl, CURLOPT_USERAGENT, m_userAgent.c_str());

    // Response buffer
    WriteCallbackData writeData;
    writeData.buffer = &response.body;
    writeData.totalSize = 0;
    writeData.stopAtJsonClose = req.stopAtJsonClose;

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
    } else if (req.method == "HEAD") {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    }

    // Perform request. Redact tokens from the logged URL — req.url commonly
    // contains ?X-Plex-Token=<long-lived-secret>.
    brls::Logger::debug("HTTP {} {}", req.method, redactTokensInUrl(req.url));
    CURLcode res = curl_easy_perform(curl);

    // Cleanup headers
    if (headerList) {
        curl_slist_free_all(headerList);
    }

    // Check result. A deliberate early stop (stopAtJsonClose) surfaces as
    // CURLE_WRITE_ERROR even though we have the full payload we wanted, so
    // treat that as a normal completion.
    if (res == CURLE_OK || (res == CURLE_WRITE_ERROR && writeData.stopped)) {
        long httpCode;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        response.statusCode = (int)httpCode;
        response.success = (httpCode >= 200 && httpCode < 300);

        brls::Logger::debug("HTTP response: {} ({} bytes{})", response.statusCode,
                            response.body.length(), writeData.stopped ? ", stopped at JSON close" : "");
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

bool HttpClient::get(const std::string& url, std::string& response,
                     const std::map<std::string, std::string>& headers) {
    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers = headers;
    HttpResponse res = request(req);
    if (res.success) {
        response = res.body;
        return true;
    }
    return false;
}

bool HttpClient::post(const std::string& url, const std::string& body, std::string& response,
                      const std::map<std::string, std::string>& headers) {
    HttpRequest req;
    req.url = url;
    req.method = "POST";
    req.body = body;
    req.headers = headers;
    HttpResponse res = request(req);
    if (res.success) {
        response = res.body;
        return true;
    }
    return false;
}

bool HttpClient::put(const std::string& url, const std::string& body, std::string& response) {
    HttpResponse res = put(url, body);
    if (res.success) {
        response = res.body;
        return true;
    }
    return false;
}

bool HttpClient::del(const std::string& url, std::string& response) {
    HttpResponse res = del(url);
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
    HttpClient::DownloadStartCallback startCallback;
    bool sizeReported;
    bool cancelled;
    int  lastStatus = 0;        // most recent HTTP status line seen (final after redirects)
    int64_t fullSize = -1;      // full file size: Content-Range total, else Content-Length
    bool startFired = false;    // startCallback invoked once
};

// Fire the one-shot startCallback with the final status + full size. Called both
// from the header callback (covers empty-body responses like 416) and as a guard
// from the write callback (covers servers that omit a clean end-of-headers line).
static void fireDownloadStart(DownloadCallbackData* data) {
    if (data && !data->startFired) {
        data->startFired = true;
        if (data->startCallback) data->startCallback(data->lastStatus, data->fullSize);
    }
}

static size_t downloadWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    DownloadCallbackData* data = static_cast<DownloadCallbackData*>(userp);
    size_t totalSize = size * nmemb;

    if (data) {
        // First body byte: headers are all in, so the status + size are final.
        fireDownloadStart(data);
        if (data->writeCallback) {
            // Call user's write callback
            if (!data->writeCallback(static_cast<const char*>(contents), totalSize)) {
                data->cancelled = true;
                return 0; // Return 0 to signal curl to abort
            }
        }
    }

    return totalSize;
}

static size_t downloadHeaderCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    DownloadCallbackData* data = static_cast<DownloadCallbackData*>(userp);
    size_t totalSize = size * nmemb;
    if (!data) return totalSize;

    std::string header(static_cast<char*>(contents), totalSize);

    // Status line ("HTTP/1.1 206 Partial Content"). With redirects curl feeds us
    // each response's headers; keep the latest — the body only follows the final.
    if (header.size() >= 5 && (header.compare(0, 5, "HTTP/") == 0)) {
        size_t sp = header.find(' ');
        if (sp != std::string::npos) {
            data->lastStatus = (int)strtol(header.c_str() + sp + 1, nullptr, 10);
        }
        return totalSize;
    }

    // Content-Range: bytes <start>-<end>/<total> — the total is the FULL size.
    if (header.find("Content-Range:") == 0 || header.find("content-range:") == 0) {
        size_t slash = header.rfind('/');
        if (slash != std::string::npos) {
            int64_t total = strtoll(header.c_str() + slash + 1, nullptr, 10);
            if (total > 0) data->fullSize = total;
        }
        return totalSize;
    }

    if (header.find("Content-Length:") == 0 || header.find("content-length:") == 0) {
        size_t colonPos = header.find(':');
        if (colonPos != std::string::npos) {
            std::string value = header.substr(colonPos + 1);
            while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) value = value.substr(1);
            char* endPtr = nullptr;
            int64_t contentLength = strtoll(value.c_str(), &endPtr, 10);
            if (endPtr != value.c_str() && contentLength > 0) {
                // For a 200, Content-Length IS the full size; for a 206 it's the
                // remaining bytes, so only use it when Content-Range didn't set it.
                if (data->lastStatus != 206 && data->fullSize < 0) data->fullSize = contentLength;
                if (data->sizeCallback && !data->sizeReported) {
                    data->sizeCallback(contentLength);
                    data->sizeReported = true;
                }
            }
        }
        return totalSize;
    }

    // Blank line = end of a header block. If this was the final response (not a
    // 3xx redirect), the status + size are settled even if no body follows
    // (e.g. 416 Range Not Satisfiable) — fire startCallback now.
    if (header == "\r\n" || header == "\n") {
        if (data->lastStatus < 300 || data->lastStatus >= 400) {
            fireDownloadStart(data);
        }
    }

    return totalSize;
}

bool HttpClient::downloadFile(const std::string& url, WriteCallback writeCallback, SizeCallback sizeCallback,
                              const std::map<std::string, std::string>& headers,
                              int64_t resumeOffset,
                              DownloadStartCallback startCallback) {
    if (!m_curl) {
        brls::Logger::error("CURL not initialized for download");
        return false;
    }

    CURL* curl = (CURL*)m_curl;

    // Reset curl handle
    curl_easy_reset(curl);

    // Set URL
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    // No overall timeout for downloads - large transcoded files can take a long time.
    // The low-speed limit below handles stalled connections instead.
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);

    // Follow redirects
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);

    // TLS verification, protocol allowlist, and NOSIGNAL (critical for
    // thread-safe operation — downloads run on a background thread).
    applyCurlSecurityDefaults(curl);
    applyCurlPerfDefaults(curl);

    // User agent
    curl_easy_setopt(curl, CURLOPT_USERAGENT, m_userAgent.c_str());

    // Set a smaller receive buffer to avoid overwhelming the Vita's network stack
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 16384L);

    // Enable TCP keepalive to prevent idle connection drops during transcoding
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 30L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 15L);

    // Prefer HTTP/1.1 for better compatibility with Vita's network stack
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

    // Resume from a byte offset (HTTP Range). The server replies 206 (honoured)
    // or 200 (ignored, full file) — the caller's startCallback inspects the
    // status to open its file append-vs-truncate before any body is written.
    if (resumeOffset > 0) {
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)resumeOffset);
    }

    // Setup callback data
    DownloadCallbackData callbackData;
    callbackData.writeCallback = writeCallback;
    callbackData.sizeCallback = sizeCallback;
    callbackData.startCallback = startCallback;
    callbackData.sizeReported = false;
    callbackData.cancelled = false;

    // Set callbacks
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, downloadWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &callbackData);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, downloadHeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &callbackData);

    // Low speed limit - abort if < 100 bytes/s for 120 seconds
    // Transcoding can have very slow starts while the server processes the media,
    // especially for video where the server needs to decode and re-encode
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 100L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);

    // Set custom headers (e.g. Plex identification headers required by transcode API)
    struct curl_slist* headerList = nullptr;
    for (const auto& h : headers) {
        std::string header = h.first + ": " + h.second;
        headerList = curl_slist_append(headerList, header.c_str());
    }
    if (headerList) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    }

    brls::Logger::info("HttpClient: Starting download from {}", redactTokensInUrl(url));

    // Perform download
    CURLcode res = curl_easy_perform(curl);

    if (headerList) {
        curl_slist_free_all(headerList);
    }

    if (callbackData.cancelled) {
        brls::Logger::info("HttpClient: Download cancelled by user");
        return false;
    }

    if (res == CURLE_OK || res == CURLE_PARTIAL_FILE) {
        long httpCode;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        if (httpCode >= 200 && httpCode < 300) {
            // For streaming transcodes, CURLE_PARTIAL_FILE is expected because the
            // server uses chunked transfer encoding and may not send Content-Length.
            // Accept as success if we actually received data.
            if (res == CURLE_PARTIAL_FILE) {
                double downloaded = 0;
                curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD, &downloaded);
                if (downloaded > 0) {
                    brls::Logger::info("HttpClient: Download completed (partial file, {:.0f} bytes received)", downloaded);
                    return true;
                }
                brls::Logger::error("HttpClient: Partial file with no data received");
                return false;
            }
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
