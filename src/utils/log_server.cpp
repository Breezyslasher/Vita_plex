/**
 * VitaPlex - the log, served over the local network. See include/utils/log_server.hpp.
 */

#include "utils/log_server.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
// Android and iPhone / iPad / Apple TV: the devices a log is hard to get off.
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
#define VITAPLEX_LOG_SERVER 1
#endif

#ifdef VITAPLEX_LOG_SERVER

#include "platform/platform.hpp"

#include <borealis.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>

namespace vitaplex {
namespace logserver {

namespace {

std::mutex g_mutex;
std::atomic<bool> g_run{false};
std::atomic<unsigned> g_gen{0};
std::string g_token;
std::string g_url;

// The address other devices reach this one at: the local end of a UDP
// "connection" to a public address (nothing is sent).
std::string localAddress() {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return "";
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &target.sin_addr);
    std::string out;
    if (connect(s, (sockaddr*)&target, sizeof(target)) == 0) {
        sockaddr_in local{};
        socklen_t len = sizeof(local);
        char buf[INET_ADDRSTRLEN] = {0};
        if (getsockname(s, (sockaddr*)&local, &len) == 0 &&
            inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf)) && local.sin_addr.s_addr != 0)
            out = buf;
    }
    close(s);
    return out;
}

std::string randomToken() {
    std::random_device rd;
    static const char hex[] = "0123456789abcdef";
    std::string t;
    for (int i = 0; i < 12; i++) t.push_back(hex[rd() & 0xF]);
    return t;
}

bool sendAll(int fd, const char* p, size_t n) {
    while (n > 0) {
        const ssize_t w = send(fd, p, n, 0);
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

void respond(int fd, const char* status, const char* type, const std::string& body,
             const char* extraHeaders = "") {
    char head[512];
    std::snprintf(head, sizeof(head),
                  "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                  "Cache-Control: no-store\r\nConnection: close\r\n%s\r\n",
                  status, type, body.size(), extraHeaders);
    if (sendAll(fd, head, std::strlen(head))) sendAll(fd, body.data(), body.size());
}

void sendFile(int fd, const std::string& path, const char* downloadName) {
    FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "rb");
    if (!f) {
        respond(fd, "404 Not Found", "text/plain; charset=utf-8", "No such log on this device.\n");
        return;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    char head[512];
    std::snprintf(head, sizeof(head),
                  "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
                  "Content-Length: %ld\r\nContent-Disposition: inline; filename=\"%s\"\r\n"
                  "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                  size < 0 ? 0L : size, downloadName);
    if (sendAll(fd, head, std::strlen(head))) {
        char buf[16384];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            if (!sendAll(fd, buf, n)) break;
    }
    std::fclose(f);
}

void handle(int fd) {
    // The request line is all that matters; a short wait for it.
    timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char req[2048];
    size_t got = 0;
    while (got < sizeof(req) - 1) {
        const ssize_t r = recv(fd, req + got, sizeof(req) - 1 - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
        req[got] = 0;
        if (std::strstr(req, "\r\n\r\n") || std::strstr(req, "\n\n")) break;
    }
    req[got] = 0;

    char method[8] = {0}, path[512] = {0};
    if (std::sscanf(req, "%7s %511s", method, path) != 2 || std::strcmp(method, "GET") != 0) {
        respond(fd, "400 Bad Request", "text/plain", "Bad request\n");
        return;
    }
    const std::string p = path;
    const std::string base = "/" + g_token + "/";
    if (p == base || p == "/" + g_token) {
        const std::string page =
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>VitaPlex logs</title>"
            "<style>body{font-family:sans-serif;background:#1c1c1e;color:#eee;margin:24px}"
            "a{display:block;margin:14px 0;padding:14px 18px;background:#2c2c2e;color:#e5a00d;"
            "border-radius:10px;text-decoration:none;font-size:18px}</style></head><body>"
            "<h2>VitaPlex logs</h2>"
            "<a href=\"" + base + "vitaplex.log\">This run's log</a>"
            "<a href=\"" + base + "vitaplex.prev.log\">The run before this one</a>"
            "<p>Open one, then save or share it from this browser.</p></body></html>";
        respond(fd, "200 OK", "text/html; charset=utf-8", page);
    } else if (p == base + "vitaplex.log") {
        sendFile(fd, platform::getLogPath(), "vitaplex.log");
    } else if (p == base + "vitaplex.prev.log") {
        sendFile(fd, platform::previousLogPath(), "vitaplex.prev.log");
    } else {
        respond(fd, "404 Not Found", "text/plain", "Not found\n");
    }
}

void serve(int listenFd, std::chrono::steady_clock::time_point until, unsigned gen) {
    // Ends on stop(), when the time runs out, or once a newer start() took over.
    while (g_run.load() && g_gen.load() == gen && std::chrono::steady_clock::now() < until) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listenFd, &set);
        timeval tv{1, 0};
        const int r = select(listenFd + 1, &set, nullptr, nullptr, &tv);
        if (r <= 0) continue;
        const int c = accept(listenFd, nullptr, nullptr);
        if (c < 0) continue;
        handle(c);
        shutdown(c, SHUT_RDWR);
        close(c);
    }
    close(listenFd);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_gen.load() == gen) {
            g_run.store(false);
            g_url.clear();
        }
    }
    brls::Logger::info("Log server: stopped");
}

}  // namespace

bool available() { return true; }

bool running() { return g_run.load(); }

std::string start(int minutes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_run.load() && !g_url.empty()) return g_url;

    const std::string ip = localAddress();
    if (ip.empty()) {
        brls::Logger::warning("Log server: no network address");
        return "";
    }

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    int port = 0;
    for (int candidate : {8765, 8766, 8767, 0}) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons((uint16_t)candidate);
        if (bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0) {
            socklen_t len = sizeof(addr);
            getsockname(fd, (sockaddr*)&addr, &len);
            port = ntohs(addr.sin_port);
            break;
        }
    }
    if (port == 0 || listen(fd, 4) != 0) {
        close(fd);
        brls::Logger::warning("Log server: could not listen");
        return "";
    }

    const unsigned gen = ++g_gen;
    g_token = randomToken();
    g_url = "http://" + ip + ":" + std::to_string(port) + "/" + g_token + "/";
    g_run.store(true);
    const auto until = std::chrono::steady_clock::now() + std::chrono::minutes(minutes);
    // Detached: it ends by itself (stop(), or the time running out), and a
    // joinable std::thread left at exit would abort the app.
    std::thread(serve, fd, until, gen).detach();
    brls::Logger::info("Log server: serving on {}:{} for {} min", ip, port, minutes);
    return g_url;
}

void stop() {
    // The thread notices within a second and closes the socket.
    std::lock_guard<std::mutex> lock(g_mutex);
    g_run.store(false);
    g_url.clear();
}

}  // namespace logserver
}  // namespace vitaplex

#else  // not POSIX-networked here

namespace vitaplex {
namespace logserver {
bool available() { return false; }
std::string start(int) { return ""; }
void stop() {}
bool running() { return false; }
}  // namespace logserver
}  // namespace vitaplex

#endif
