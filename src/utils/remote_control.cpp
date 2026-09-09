/*
 * Sockets and player wiring for remote control. The protocol itself is in
 * remote_control_protocol.cpp and knows nothing about either.
 *
 * Two sockets:
 *   UDP :32412  GDM. Answers a controller's M-SEARCH and announces itself
 *               unprompted, so this app appears in a player list.
 *   TCP :32500  The /player endpoints a controller then talks to.
 *
 * Both are off unless the setting is on, and neither exists on a platform
 * whose sockets cannot do this — see isSupported().
 */

#include "utils/remote_control.hpp"

#include "app/application.hpp"
#include "app/music_queue.hpp"
#include "app/plex_client.hpp"
#include "activity/player_activity.hpp"
#include "player/mpv_player.hpp"
#include "platform/platform.hpp"
#include "utils/async.hpp"

#include <borealis.hpp>
#include <borealis/core/timer.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>

// Consoles are excluded rather than attempted: their socket layers do not
// offer the multicast join and reusable bind this needs, and a half-working
// discovery is worse than an honest "not on this platform".
#if defined(__vita__) || defined(__PS4__) || defined(__SWITCH__)
#define VITAPLEX_RC_SOCKETS 0
#else
#define VITAPLEX_RC_SOCKETS 1
#endif

#if VITAPLEX_RC_SOCKETS
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define RC_INVALID INVALID_SOCKET
#define RC_CLOSE closesocket
using sockopt_t = char;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define RC_INVALID (-1)
#define RC_CLOSE ::close
using sockopt_t = int;
#endif
#endif  // VITAPLEX_RC_SOCKETS

namespace vitaplex {

namespace {

constexpr int kHttpPort = 32500;   // where a controller talks to a player
constexpr int kGdmPort  = 32412;   // where a player is asked "are you there"
constexpr int kGdmHelloPort = 32413;   // where a player says so unprompted
constexpr const char* kGdmGroup = "239.0.0.250";

// Everything the timeline needs, read on the UI thread and handed to the
// socket thread. Reading player state off-thread is what this avoids.
remote::Snapshot snapshotNow() {
    remote::Snapshot s;
    auto& mpv = MpvPlayer::getInstance();
    auto& queue = MusicQueue::getInstance();

    const bool anything = PlayerActivity::isActive();
    if (!anything) return s;   // state stays "stopped"

    s.type = queue.isMusicQueue() ? "music" : "video";
    switch (mpv.getState()) {
        case MpvPlayerState::PLAYING: s.state = "playing"; break;
        case MpvPlayerState::PAUSED:  s.state = "paused";  break;
        case MpvPlayerState::LOADING:
        case MpvPlayerState::BUFFERING: s.state = "buffering"; break;
        default:                      s.state = "stopped"; break;
    }
    s.timeMs = (int64_t)(mpv.getPosition() * 1000.0);
    s.durationMs = (int64_t)(mpv.getDuration() * 1000.0);
    s.playQueueID = queue.getPlayQueueID();
    if (s.playQueueID > 0)
        s.containerKey = "/playQueues/" + std::to_string(s.playQueueID) + "?window=100&own=1";
    if (const QueueItem* t = queue.getCurrentTrack()) {
        s.ratingKey = t->ratingKey;
        if (!t->ratingKey.empty()) s.key = "/library/metadata/" + t->ratingKey;
    }

    PlexClient& client = PlexClient::getInstance();
    s.machineIdentifier = client.getMachineIdentifier();
    s.volume = mpv.getVolume();
    return s;
}

// Everything a command has to do to the player. Runs on the UI thread.
void applyCommand(const remote::Command& cmd) {
    auto& mpv = MpvPlayer::getInstance();
    using K = remote::Command::Kind;

    switch (cmd.kind) {
        case K::PLAY:       mpv.play();  break;
        case K::PAUSE:      mpv.pause(); break;
        case K::PLAY_PAUSE:
            if (mpv.isPlaying()) mpv.pause(); else mpv.play();
            break;
        case K::STOP:
            // Closing the player is what "stop" means here; there is no
            // stopped-but-open state for a controller to see.
            if (PlayerActivity::isActive()) brls::Application::popActivity();
            break;
        case K::NEXT:          MusicQueue::getInstance().playNext(); break;
        case K::PREVIOUS:      MusicQueue::getInstance().playPrevious(); break;
        case K::SEEK_TO:       mpv.seekTo((double)cmd.offsetMs / 1000.0); break;
        case K::STEP_FORWARD:  mpv.seekRelative(30.0);  break;
        case K::STEP_BACK:     mpv.seekRelative(-15.0); break;
        case K::SET_VOLUME:    mpv.setVolume(cmd.volume); break;

        case K::PLAY_MEDIA: {
            // The queue lives on the server and the controller has already
            // built it, so this fetches that rather than making its own —
            // which is what keeps both ends looking at one object.
            const int pqID = cmd.playQueueID;
            const std::string key = cmd.key;
            const int64_t offset = cmd.offsetMs;
            asyncRun([pqID, key, offset]() {
                if (pqID > 0) {
                    PlexClient::PlayQueueContainer pq;
                    if (PlexClient::getInstance().getPlayQueue(pqID, pq) && !pq.items.empty()) {
                        brls::sync([pq]() {
                            MusicQueue::getInstance().setFromPlayQueue(pq, pq.playQueueShuffled);
                            // A player already open is showing the old queue,
                            // so it is replaced rather than stacked on.
                            if (PlayerActivity::isActive())
                                brls::Application::popActivity(brls::TransitionAnimation::NONE);
                            brls::Application::pushActivity(PlayerActivity::createResumeQueue());
                        });
                        return;
                    }
                    brls::sync([]() {
                        brls::Application::notify("A remote app sent a queue that could not be read");
                    });
                    return;
                }
                // No queue, just an item: open it directly.
                std::string ratingKey = key;
                const size_t slash = ratingKey.rfind('/');
                if (slash != std::string::npos) ratingKey = ratingKey.substr(slash + 1);
                if (ratingKey.empty()) return;
                brls::sync([ratingKey, offset]() {
                    (void)offset;
                    brls::Application::pushActivity(new PlayerActivity(ratingKey));
                });
            });
            break;
        }
        case K::NONE:
        default: break;
    }
}

}  // namespace

// ── Identity ─────────────────────────────────────────────────────────────

std::string RemoteControlServer::deviceName() {
    const auto& vc = platform::getVideoConstraints();
    std::string n = Application::getInstance().getSettings().remoteControlName;
    if (!n.empty()) return n;
    return std::string(PLEX_CLIENT_NAME) + " (" + vc.plexDevice + ")";
}

std::string RemoteControlServer::clientIdentifier() {
    // PLEX_CLIENT_ID is a build constant, so every install would claim the
    // same identity and a server could not tell two of them apart. The first
    // run makes one and it is kept from then on.
    AppSettings& s = Application::getInstance().getSettings();
    if (s.clientUuid.empty()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s-%08lx%08lx", PLEX_CLIENT_ID,
                      (unsigned long)std::chrono::steady_clock::now().time_since_epoch().count(),
                      (unsigned long)std::time(nullptr));
        s.clientUuid = buf;
        Application::getInstance().saveSettings();
    }
    return s.clientUuid;
}

bool RemoteControlServer::isSupported() {
#if VITAPLEX_RC_SOCKETS
    return true;
#else
    return false;
#endif
}

// ── Server ───────────────────────────────────────────────────────────────

#if VITAPLEX_RC_SOCKETS

struct RemoteControlServer::Impl {
    std::atomic<bool> running{false};
    socket_t httpFd = RC_INVALID;
    socket_t gdmFd  = RC_INVALID;
    std::thread httpThread, gdmThread;
    std::mutex protoMutex;
    remote::Protocol proto{remote::Identity{}};

    // Published by the UI thread, read by the socket threads.
    std::mutex snapMutex;
    remote::Snapshot lastSnap;
    brls::RepeatingTimer snapTimer;

    void publishSnapshot() {
        remote::Snapshot s = snapshotNow();
        std::lock_guard<std::mutex> lk(snapMutex);
        lastSnap = s;
    }

    void serveHttp();
    void serveGdm();
    void closeSockets() {
        if (httpFd != RC_INVALID) { RC_CLOSE(httpFd); httpFd = RC_INVALID; }
        if (gdmFd  != RC_INVALID) { RC_CLOSE(gdmFd);  gdmFd  = RC_INVALID; }
    }
};

namespace {

// Read until the headers are complete or the peer gives up. Bodies are not
// read: nothing in the player protocol sends one.
bool readRequest(socket_t fd, std::string& out) {
    char buf[2048];
    out.clear();
    for (int i = 0; i < 16; i++) {
        const int n = (int)recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        out.append(buf, (size_t)n);
        if (out.find("\r\n\r\n") != std::string::npos) return true;
        if (out.size() > 64 * 1024) return false;   // not a player request
    }
    return false;
}

void sendAll(socket_t fd, const std::string& s) {
    size_t sent = 0;
    while (sent < s.size()) {
        const int n = (int)send(fd, s.data() + sent, (int)(s.size() - sent), 0);
        if (n <= 0) return;
        sent += (size_t)n;
    }
}

const char* statusText(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        default:  return "OK";
    }
}

}  // namespace

void RemoteControlServer::Impl::serveHttp() {
    while (running.load()) {
        sockaddr_in peer{};
#ifdef _WIN32
        int plen = sizeof(peer);
#else
        socklen_t plen = sizeof(peer);
#endif
        const socket_t c = accept(httpFd, (sockaddr*)&peer, &plen);
        if (c == RC_INVALID) {
            if (!running.load()) break;
            continue;
        }

        std::string raw;
        if (readRequest(c, raw)) {
            remote::Request req;
            if (remote::parseRequest(raw, req)) {
                // The last snapshot the UI thread published, rather than
                // asking it for one now. Blocking a socket thread on the UI
                // thread would stall every poll behind whatever the player is
                // doing, and deadlock outright if the UI thread is itself
                // waiting on something. A second of staleness is what Plex's
                // own timeline reporting carries anyway.
                remote::Snapshot snap;
                {
                    std::lock_guard<std::mutex> lk(snapMutex);
                    snap = lastSnap;
                }

                remote::Command cmd;
                remote::Response res;
                {
                    std::lock_guard<std::mutex> lk(protoMutex);
                    res = proto.handle(req, snap, &cmd);
                }
                if (cmd.kind != remote::Command::Kind::NONE) {
                    brls::Logger::info("RemoteControl: {} from {}", req.path,
                                       inet_ntoa(peer.sin_addr));
                    brls::sync([this, cmd]() { applyCommand(cmd); publishSnapshot(); });
                }

                std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " +
                                  statusText(res.status) + "\r\n";
                out += "Content-Type: " + res.contentType + "\r\n";
                out += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
                for (const auto& h : res.headers) out += h.first + ": " + h.second + "\r\n";
                out += "Connection: close\r\n\r\n";
                out += res.body;
                sendAll(c, out);
            }
        }
        RC_CLOSE(c);
    }
}

void RemoteControlServer::Impl::serveGdm() {
    auto lastHello = std::chrono::steady_clock::now() - std::chrono::seconds(60);
    while (running.load()) {
        // Announce periodically so a controller that started listening after
        // this app did still finds it without asking.
        const auto now = std::chrono::steady_clock::now();
        if (now - lastHello > std::chrono::seconds(30)) {
            lastHello = now;
            std::string hello;
            {
                std::lock_guard<std::mutex> lk(protoMutex);
                hello = proto.gdmHello();
            }
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(kGdmHelloPort);
            to.sin_addr.s_addr = inet_addr(kGdmGroup);
            sendto(gdmFd, hello.data(), (int)hello.size(), 0, (sockaddr*)&to, sizeof(to));
        }

        char buf[1024];
        sockaddr_in from{};
#ifdef _WIN32
        int flen = sizeof(from);
#else
        socklen_t flen = sizeof(from);
#endif
        const int n = (int)recvfrom(gdmFd, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &flen);
        if (n <= 0) {
            // The socket has a receive timeout so the hello above still fires
            // on a quiet network.
            continue;
        }
        buf[n] = '\0';
        if (std::strncmp(buf, "M-SEARCH", 8) != 0) continue;

        std::string reply;
        {
            std::lock_guard<std::mutex> lk(protoMutex);
            reply = proto.gdmReply();
        }
        // The reply must leave from :32412, which it does because that is the
        // socket it arrived on. Replying from an ephemeral port is the classic
        // way to be invisible to a controller that checks.
        sendto(gdmFd, reply.data(), (int)reply.size(), 0, (sockaddr*)&from, flen);
    }
}

void RemoteControlServer::start() {
    if (m_impl && m_impl->running.load()) return;
    if (!isSupported()) return;

#ifdef _WIN32
    static bool wsaUp = false;
    if (!wsaUp) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            brls::Logger::error("RemoteControl: WSAStartup failed");
            return;
        }
        wsaUp = true;
    }
#endif

    m_impl.reset(new Impl());

    remote::Identity id;
    id.name = deviceName();
    id.identifier = clientIdentifier();
    id.product = PLEX_CLIENT_NAME;
    id.version = VITA_PLEX_DISPLAY_VERSION;
    id.platform = platform::getVideoConstraints().plexPlatform;
    id.deviceClass = platform::isPhoneScreen() ? "phone" : "stb";
    id.httpPort = kHttpPort;
    m_impl->proto.setIdentity(id);

    // HTTP listener.
    m_impl->httpFd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_impl->httpFd == RC_INVALID) {
        brls::Logger::error("RemoteControl: could not make the HTTP socket");
        m_impl.reset();
        return;
    }
    sockopt_t one = 1;
    setsockopt(m_impl->httpFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(kHttpPort);
    if (bind(m_impl->httpFd, (sockaddr*)&addr, sizeof(addr)) < 0 ||
        listen(m_impl->httpFd, 8) < 0) {
        brls::Logger::error("RemoteControl: port {} is not available", kHttpPort);
        m_impl->closeSockets();
        m_impl.reset();
        return;
    }

    // GDM. Bound to the discovery port so replies leave from it, joined to the
    // group so a multicast M-SEARCH is heard as well as a broadcast one.
    m_impl->gdmFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_impl->gdmFd != RC_INVALID) {
        setsockopt(m_impl->gdmFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockopt_t bcast = 1;
        setsockopt(m_impl->gdmFd, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
        sockaddr_in g{};
        g.sin_family = AF_INET;
        g.sin_addr.s_addr = INADDR_ANY;
        g.sin_port = htons(kGdmPort);
        if (bind(m_impl->gdmFd, (sockaddr*)&g, sizeof(g)) < 0) {
            brls::Logger::warning("RemoteControl: GDM port {} busy; discovery off", kGdmPort);
            RC_CLOSE(m_impl->gdmFd);
            m_impl->gdmFd = RC_INVALID;
        } else {
            ip_mreq mreq{};
            mreq.imr_multiaddr.s_addr = inet_addr(kGdmGroup);
            mreq.imr_interface.s_addr = INADDR_ANY;
            if (setsockopt(m_impl->gdmFd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                           (const sockopt_t*)&mreq, sizeof(mreq)) < 0) {
                // Not fatal: a broadcast M-SEARCH still arrives. iOS refuses
                // this without Apple's multicast entitlement.
                brls::Logger::warning("RemoteControl: no multicast join; broadcast only");
            }
#ifdef _WIN32
            DWORD tv = 1000;
#else
            timeval tv{};
            tv.tv_sec = 1;
#endif
            setsockopt(m_impl->gdmFd, SOL_SOCKET, SO_RCVTIMEO, (const sockopt_t*)&tv, sizeof(tv));
        }
    }

    m_impl->running.store(true);
    // One publisher on the UI thread; a poll never has to wake it.
    m_impl->publishSnapshot();
    m_impl->snapTimer.setCallback([this]() { if (m_impl) m_impl->publishSnapshot(); });
    m_impl->snapTimer.start(1000);
    m_impl->httpThread = std::thread([this] { m_impl->serveHttp(); });
    if (m_impl->gdmFd != RC_INVALID)
        m_impl->gdmThread = std::thread([this] { m_impl->serveGdm(); });

    brls::Logger::info("RemoteControl: listening on {} as \"{}\" ({})",
                       kHttpPort, id.name, id.identifier);
}

void RemoteControlServer::stop() {
    if (!m_impl) return;
    m_impl->running.store(false);
    m_impl->snapTimer.stop();
    // Closing the sockets is what unblocks accept() and recvfrom().
    m_impl->closeSockets();
    if (m_impl->httpThread.joinable()) m_impl->httpThread.join();
    if (m_impl->gdmThread.joinable())  m_impl->gdmThread.join();
    m_impl.reset();
    brls::Logger::info("RemoteControl: stopped");
}

bool RemoteControlServer::isRunning() const {
    return m_impl && m_impl->running.load();
}

#else  // no sockets on this platform

struct RemoteControlServer::Impl {};
void RemoteControlServer::start() {}
void RemoteControlServer::stop() {}
bool RemoteControlServer::isRunning() const { return false; }

#endif  // VITAPLEX_RC_SOCKETS

RemoteControlServer& RemoteControlServer::getInstance() {
    static RemoteControlServer instance;
    return instance;
}

RemoteControlServer::~RemoteControlServer() = default;

void RemoteControlServer::applySetting() {
    const bool want = isSupported() &&
                      Application::getInstance().getSettings().remoteControlEnabled;
    if (want && !isRunning())  start();
    if (!want && isRunning())  stop();
}

}  // namespace vitaplex
