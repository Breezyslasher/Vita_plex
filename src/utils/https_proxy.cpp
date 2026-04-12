/**
 * VitaPlex - Local HTTPS-to-HTTP Proxy for PS4
 *
 * See include/utils/https_proxy.h for design overview.
 */

#ifdef __PS4__

#include "utils/https_proxy.h"
#include <borealis.hpp>
#include <curl/curl.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>

namespace vitaplex {

// ─── Curl write callback: streams data to the client socket ──────────────

struct ProxyWriteCtx {
    int clientFd;
    bool headersSent;
    long httpCode;
    std::string contentType;
    // For m3u8: buffer the entire body so we can rewrite URLs before sending
    bool isM3u8;
    std::string bodyBuffer;
    int proxyPort;
};

static size_t curlWriteToSocket(void* data, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    ProxyWriteCtx* ctx = (ProxyWriteCtx*)userp;
    if (!ctx || ctx->clientFd < 0) return 0;

    if (ctx->isM3u8) {
        // Buffer m3u8 content for URL rewriting
        ctx->bodyBuffer.append((char*)data, total);
        return total;
    }

    // Stream directly to client
    if (!ctx->headersSent) {
        ctx->headersSent = true;
        char hdr[256];
        snprintf(hdr, sizeof(hdr),
                 "HTTP/1.0 200 OK\r\n"
                 "Content-Type: %s\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 ctx->contentType.empty() ? "application/octet-stream" : ctx->contentType.c_str());
        send(ctx->clientFd, hdr, strlen(hdr), 0);
    }

    size_t sent = 0;
    while (sent < total) {
        ssize_t n = send(ctx->clientFd, (char*)data + sent, total - sent, 0);
        if (n <= 0) return 0;  // Client disconnected
        sent += n;
    }
    return total;
}

// ─── Curl header callback: capture Content-Type ──────────────────────────

static size_t curlHeaderCb(void* data, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    ProxyWriteCtx* ctx = (ProxyWriteCtx*)userp;
    std::string line((char*)data, total);

    // Parse Content-Type header
    if (line.size() > 14) {
        std::string lower = line.substr(0, 13);
        for (auto& c : lower) c = tolower(c);
        if (lower == "content-type:") {
            ctx->contentType = line.substr(14);
            // Trim whitespace and \r\n
            while (!ctx->contentType.empty() &&
                   (ctx->contentType.back() == '\r' || ctx->contentType.back() == '\n' ||
                    ctx->contentType.back() == ' '))
                ctx->contentType.pop_back();

            // Check if this is an HLS playlist
            std::string ct = ctx->contentType;
            for (auto& c : ct) c = tolower(c);
            ctx->isM3u8 = (ct.find("mpegurl") != std::string::npos ||
                           ct.find("m3u") != std::string::npos);
        }
    }
    return total;
}

// ─── Rewrite HTTPS URLs in m3u8 playlist content ─────────────────────────

static std::string rewriteM3u8(const std::string& body, int proxyPort) {
    std::string result;
    result.reserve(body.size() * 2);

    char proxyPrefix[64];
    snprintf(proxyPrefix, sizeof(proxyPrefix), "http://127.0.0.1:%d/", proxyPort);

    size_t pos = 0;
    while (pos < body.size()) {
        // Find next https://
        size_t found = body.find("https://", pos);
        if (found == std::string::npos) {
            result.append(body, pos, body.size() - pos);
            break;
        }
        // Append everything before the match
        result.append(body, pos, found - pos);
        // Insert proxy prefix before https://
        result.append(proxyPrefix);
        // Keep the https:// and continue
        pos = found;
        // Find end of URL (whitespace, newline, quote, or EOF)
        size_t urlEnd = found;
        while (urlEnd < body.size() && body[urlEnd] != '\n' && body[urlEnd] != '\r' &&
               body[urlEnd] != '"' && body[urlEnd] != ' ' && body[urlEnd] != '\t')
            urlEnd++;
        result.append(body, found, urlEnd - found);
        pos = urlEnd;
    }
    return result;
}

// ─── Parse HTTP request line from client ─────────────────────────────────

// Reads until \r\n\r\n (end of HTTP headers). Returns the full header block.
static std::string readHttpHeaders(int fd) {
    std::string buf;
    char c;
    while (buf.size() < 16384) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) break;
        buf.push_back(c);
        if (buf.size() >= 4 && buf.substr(buf.size() - 4) == "\r\n\r\n")
            break;
    }
    return buf;
}

// Extract the request path from "GET /path HTTP/1.x\r\n..."
static std::string extractRequestPath(const std::string& headers) {
    // Find the first line
    size_t lineEnd = headers.find("\r\n");
    if (lineEnd == std::string::npos) return "";

    std::string requestLine = headers.substr(0, lineEnd);
    // "GET /https://host/path HTTP/1.1"
    size_t pathStart = requestLine.find(' ');
    if (pathStart == std::string::npos) return "";
    pathStart++;  // Skip the space

    size_t pathEnd = requestLine.find(' ', pathStart);
    if (pathEnd == std::string::npos) pathEnd = requestLine.size();

    std::string path = requestLine.substr(pathStart, pathEnd - pathStart);
    // Remove leading /
    if (!path.empty() && path[0] == '/') path = path.substr(1);
    return path;
}

// Extract specific headers from the request to forward (X-Plex-* headers)
static std::vector<std::string> extractForwardHeaders(const std::string& headers) {
    std::vector<std::string> result;
    size_t pos = headers.find("\r\n");
    if (pos == std::string::npos) return result;
    pos += 2;  // Skip first line (request line)

    while (pos < headers.size()) {
        size_t lineEnd = headers.find("\r\n", pos);
        if (lineEnd == std::string::npos || lineEnd == pos) break;

        std::string line = headers.substr(pos, lineEnd - pos);
        // Forward X-Plex-* and User-Agent headers
        std::string lower = line;
        for (auto& c : lower) c = tolower(c);
        if (lower.find("x-plex-") == 0 || lower.find("user-agent:") == 0) {
            result.push_back(line);
        }
        pos = lineEnd + 2;
    }
    return result;
}

// ─── Send error response to client ───────────────────────────────────────

static void sendError(int fd, int code, const char* msg) {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "HTTP/1.0 %d %s\r\n"
             "Content-Type: text/plain\r\n"
             "Connection: close\r\n"
             "\r\n"
             "%s\n", code, msg, msg);
    send(fd, buf, strlen(buf), 0);
}

// ─── HttpsProxy implementation ───────────────────────────────────────────

HttpsProxy& HttpsProxy::getInstance() {
    static HttpsProxy instance;
    return instance;
}

HttpsProxy::~HttpsProxy() {
    stop();
}

bool HttpsProxy::start() {
    if (m_running.load()) return true;

    m_serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverFd < 0) {
        brls::Logger::error("HttpsProxy: socket() failed: {}", strerror(errno));
        return false;
    }

    int opt = 1;
    setsockopt(m_serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // Let the OS pick a free port

    if (bind(m_serverFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        brls::Logger::error("HttpsProxy: bind() failed: {}", strerror(errno));
        close(m_serverFd);
        m_serverFd = -1;
        return false;
    }

    // Get the assigned port
    socklen_t addrLen = sizeof(addr);
    getsockname(m_serverFd, (struct sockaddr*)&addr, &addrLen);
    m_port = ntohs(addr.sin_port);

    if (listen(m_serverFd, 8) < 0) {
        brls::Logger::error("HttpsProxy: listen() failed: {}", strerror(errno));
        close(m_serverFd);
        m_serverFd = -1;
        return false;
    }

    m_running.store(true);
    m_thread = std::thread(&HttpsProxy::acceptLoop, this);

    brls::Logger::info("HttpsProxy: Started on 127.0.0.1:{}", m_port);
    return true;
}

void HttpsProxy::stop() {
    if (!m_running.load()) return;

    m_running.store(false);

    // Close the server socket to unblock accept()
    if (m_serverFd >= 0) {
        close(m_serverFd);
        m_serverFd = -1;
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }

    brls::Logger::info("HttpsProxy: Stopped");
}

void HttpsProxy::acceptLoop() {
    while (m_running.load()) {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = accept(m_serverFd, (struct sockaddr*)&clientAddr, &clientLen);
        if (clientFd < 0) {
            if (m_running.load()) {
                brls::Logger::debug("HttpsProxy: accept() failed: {}", strerror(errno));
            }
            continue;
        }

        // Handle each connection in a detached thread to allow concurrent
        // segment fetches during HLS playback
        std::thread([this, clientFd]() {
            handleClient(clientFd);
            close(clientFd);
        }).detach();
    }
}

void HttpsProxy::handleClient(int clientFd) {
    // Read HTTP request headers
    std::string headers = readHttpHeaders(clientFd);
    if (headers.empty()) return;

    // Extract the target URL from the request path
    std::string targetUrl = extractRequestPath(headers);
    if (targetUrl.empty() || targetUrl.find("://") == std::string::npos) {
        sendError(clientFd, 400, "Bad Request: no target URL in path");
        return;
    }

    brls::Logger::debug("HttpsProxy: Fetching {}", targetUrl.substr(0, 100));

    // Extract headers to forward
    auto fwdHeaders = extractForwardHeaders(headers);

    // Use libcurl to fetch the target URL (with HTTPS)
    CURL* curl = curl_easy_init();
    if (!curl) {
        sendError(clientFd, 502, "Bad Gateway: curl_easy_init failed");
        return;
    }

    ProxyWriteCtx ctx;
    ctx.clientFd = clientFd;
    ctx.headersSent = false;
    ctx.httpCode = 0;
    ctx.isM3u8 = false;
    ctx.proxyPort = m_port;

    // Check if URL ends with .m3u8 (before query params)
    {
        std::string pathOnly = targetUrl;
        size_t qmark = pathOnly.find('?');
        if (qmark != std::string::npos) pathOnly = pathOnly.substr(0, qmark);
        std::string lower = pathOnly;
        for (auto& c : lower) c = tolower(c);
        if (lower.size() >= 5 && lower.substr(lower.size() - 5) == ".m3u8") {
            ctx.isM3u8 = true;
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, targetUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteToSocket);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curlHeaderCb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "VitaPlex/1.0.0");
    // Don't send signal on timeout (not safe in threads)
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    // Build forward headers
    struct curl_slist* headerList = nullptr;
    for (const auto& h : fwdHeaders) {
        headerList = curl_slist_append(headerList, h.c_str());
    }
    if (headerList) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    }

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        brls::Logger::error("HttpsProxy: curl failed for {}: {}",
                           targetUrl.substr(0, 80), curl_easy_strerror(res));
        if (!ctx.headersSent) {
            sendError(clientFd, 502, curl_easy_strerror(res));
        }
    } else if (ctx.isM3u8 && !ctx.bodyBuffer.empty()) {
        // Rewrite m3u8 content and send
        std::string rewritten = rewriteM3u8(ctx.bodyBuffer, m_port);

        char hdr[512];
        snprintf(hdr, sizeof(hdr),
                 "HTTP/1.0 200 OK\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 ctx.contentType.empty() ? "application/vnd.apple.mpegurl" : ctx.contentType.c_str(),
                 rewritten.size());
        send(clientFd, hdr, strlen(hdr), 0);

        size_t sent = 0;
        while (sent < rewritten.size()) {
            ssize_t n = send(clientFd, rewritten.data() + sent, rewritten.size() - sent, 0);
            if (n <= 0) break;
            sent += n;
        }
    }

    if (headerList) curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
}

std::string HttpsProxy::rewriteUrl(const std::string& url) const {
    if (!m_running.load() || m_port == 0) return url;

    // Only rewrite https:// URLs
    if (url.size() < 8) return url;
    std::string prefix = url.substr(0, 8);
    for (auto& c : prefix) c = tolower(c);
    if (prefix.find("https://") != 0) return url;

    char buf[64];
    snprintf(buf, sizeof(buf), "http://127.0.0.1:%d/", m_port);
    return std::string(buf) + url;
}

} // namespace vitaplex

#endif // __PS4__
