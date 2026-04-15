#pragma once

/**
 * VitaPlex - Local HTTPS-to-HTTP Proxy for PS4
 *
 * PS4's ffmpeg build lacks TLS support, so MPV cannot open HTTPS URLs directly.
 * This proxy listens on localhost and forwards requests to HTTPS upstream
 * using libcurl (which has working TLS via mbedtls on PS4).
 *
 * URL scheme: http://127.0.0.1:PORT/<original_https_url>
 * e.g. http://127.0.0.1:9876/https://plex:32400/video/.../start.m3u8?token=xyz
 *
 * For HLS (.m3u8) responses, the proxy rewrites https:// segment URLs
 * to route through the proxy as well.
 *
 * Only compiled/used on PS4 (#ifdef __PS4__).
 */

#ifdef __PS4__

#include <string>
#include <atomic>
#include <thread>

namespace vitaplex {

class HttpsProxy {
public:
    static HttpsProxy& getInstance();

    // Start the proxy on a random local port. Returns true on success.
    bool start();

    // Stop the proxy and close the listen socket.
    void stop();

    // Get the local port the proxy is listening on (0 if not running).
    int getPort() const { return m_port; }

    // Is the proxy running?
    bool isRunning() const { return m_running.load(); }

    // Rewrite an HTTPS URL to go through the local proxy.
    // "https://host/path" → "http://127.0.0.1:PORT/https://host/path"
    // Returns the original URL unchanged if it's not HTTPS.
    std::string rewriteUrl(const std::string& url) const;

private:
    HttpsProxy() = default;
    ~HttpsProxy();
    HttpsProxy(const HttpsProxy&) = delete;
    HttpsProxy& operator=(const HttpsProxy&) = delete;

    void acceptLoop();
    void handleClient(int clientFd);

    int m_port = 0;
    int m_serverFd = -1;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
};

} // namespace vitaplex

#endif // __PS4__
