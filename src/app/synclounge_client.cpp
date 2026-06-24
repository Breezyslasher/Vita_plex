/**
 * VitaPlex - SyncLounge client (connectivity spike) implementation.
 *
 * Speaks Engine.IO v4 / Socket.IO v4 over the HTTP long-polling transport.
 * Wire format reminders (EIO=4):
 *   - A polling HTTP body carries one or more Engine.IO packets separated by
 *     the record-separator byte 0x1e.
 *   - Engine.IO packet = <type-digit><payload>:
 *       0 open    -> payload is JSON: {"sid":...,"pingInterval":...,"pingTimeout":...}
 *       1 close
 *       2 ping    -> we must reply with a "3" pong
 *       3 pong
 *       4 message -> payload is a Socket.IO packet
 *       6 noop
 *   - Socket.IO packet (inside an Engine.IO "4") = <type-digit><payload>:
 *       0 CONNECT      -> "40{\"sid\":...}" namespace-connect ack
 *       1 DISCONNECT
 *       2 EVENT        -> "42[\"name\",arg,...]"
 *       4 CONNECT_ERROR
 *   - We only ever use the default namespace, so there is no "/nsp," prefix.
 *
 * SyncLounge layers its own keepalive on top: the server emits `slPing` with a
 * freshly generated secret string; the client must echo that exact secret back
 * via `slPong` or the server disconnects it. We answer both that and the
 * transport-level 2/3 ping inline in the poll loop.
 *
 * Two HTTP channels, exactly like a browser XHR-polling client:
 *   - the worker thread holds one long-poll GET open and POSTs its own
 *     replies (pong / slPong / join) serially on a dedicated HttpClient;
 *   - outbound app events (sendChatMessage, future player updates) go out on a
 *     SEPARATE POST using a fresh HttpClient on a short background task, so a
 *     send never has to wait for the in-flight poll to return. The server
 *     flushes the held GET when the POST arrives — standard polling behaviour.
 */

#include "app/synclounge_client.hpp"
#include "utils/http_client.hpp"
#include "utils/async.hpp"

#include <borealis.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

namespace vitaplex {

// Connection state shared between the poll worker and the send path.
struct SyncLoungeClient::Session {
    std::atomic<bool> running{true};
    std::mutex        mtx;             // guards endpointPrefix + sid
    std::string       endpointPrefix;  // "<base><path>", e.g. "https://host/socket.io/"
    std::string       sid;             // empty until the handshake completes
    std::atomic<long> sendTbust{1000000};  // send-channel cache-buster (distinct range)
};

namespace {

// ── tiny, flat JSON scrapers ──────────────────────────────────────────────
// Good enough for the handful of flat fields we read out of the open packet
// and event frames; not a general JSON parser.

// Value of a `"key":"string"` pair. Returns "" if absent.
std::string jsonStr(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) return "";
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return "";
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return json.substr(q1 + 1, q2 - q1 - 1);
}

// Value of a `"key":<integer>` pair. Returns -1 if absent.
long jsonNum(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) return -1;
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return -1;
    size_t p = colon + 1;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) p++;
    long v = 0; bool any = false;
    while (p < json.size() && json[p] >= '0' && json[p] <= '9') {
        v = v * 10 + (json[p] - '0'); p++; any = true;
    }
    return any ? v : -1;
}

// The n-th (1-based) double-quoted token in a string, with backslash escapes
// unwrapped. For an event payload `["name",arg,...]`, token 1 is the event
// name; for `["slPing","secret"]`, token 2 is the secret.
std::string nthQuoted(const std::string& s, int n) {
    int count = 0;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '"') {
            size_t j = i + 1;
            std::string out;
            while (j < s.size() && s[j] != '"') {
                if (s[j] == '\\' && j + 1 < s.size()) { out += s[j + 1]; j += 2; }
                else { out += s[j]; j++; }
            }
            if (++count == n) return out;
            i = j + 1;
        } else {
            i++;
        }
    }
    return "";
}

// Escape a user-supplied string for embedding in a JSON string literal.
std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:   o += c;      break;
        }
    }
    return o;
}

}  // namespace

// The whole handshake -> connect -> join -> poll sequence. Runs on the worker
// thread; everything it needs is in `session` (shared) + the by-value config
// and log callback, so it is safe even if the SyncLoungeClient is destroyed
// mid-run — `session->running` is the shared kill switch.
void SyncLoungeClient::runWorker(std::shared_ptr<Session> session,
                                 Config cfg,
                                 LogFn  log) {
    auto alive = [&]() { return session->running.load(); };
    auto out   = [&](const std::string& s) {
        brls::Logger::info("SyncLounge: {}", s);
        if (log) log(s);
    };
    auto kill  = [&]() { session->running.store(false); };

    // Normalize the base URL + socket path, then publish the prefix the send
    // channel will reuse.
    std::string base = cfg.server;
    while (!base.empty() && base.back() == '/') base.pop_back();
    std::string path = cfg.socketPath.empty() ? std::string("/socket.io/") : cfg.socketPath;
    if (path.front() != '/') path = "/" + path;
    if (path.back()  != '/') path += "/";
    {
        std::lock_guard<std::mutex> lk(session->mtx);
        session->endpointPrefix = base + path;
    }

    long tbust = 1;  // worker-channel cache-buster, mirrors the JS client's &t= param
    auto endpoint = [&](const std::string& sid) {
        std::string u = base + path + "?EIO=4&transport=polling&t=" + std::to_string(tbust++);
        if (!sid.empty()) u += "&sid=" + sid;
        return u;
    };

    HttpClient http;  // dedicated handle: only ever used from this thread
    http.setFollowRedirects(true);

    // ── 1. Engine.IO handshake ────────────────────────────────────────────
    out("[handshake] GET " + base + path + "?EIO=4&transport=polling");
    std::string sid;
    {
        HttpRequest req;
        req.url     = endpoint("");
        req.method  = "GET";
        req.timeout = 15;
        HttpResponse res = http.request(req);
        if (!res.success || res.statusCode != 200) {
            out("[handshake] FAILED status=" + std::to_string(res.statusCode) +
                (res.error.empty() ? "" : (" err=" + res.error)));
            kill();
            return;
        }
        if (res.body.empty() || res.body[0] != '0') {
            out("[handshake] unexpected open packet: " + res.body.substr(0, 120));
            kill();
            return;
        }
        const std::string json = res.body.substr(1);
        sid = jsonStr(json, "sid");
        if (sid.empty()) {
            out("[handshake] no sid in: " + res.body.substr(0, 120));
            kill();
            return;
        }
        out("[handshake] OK sid=" + sid +
            " pingInterval=" + std::to_string(jsonNum(json, "pingInterval")) +
            " pingTimeout="  + std::to_string(jsonNum(json, "pingTimeout")));
    }

    // POST a single raw Engine.IO packet on the worker channel.
    auto sendPacket = [&](const std::string& packet, const std::string& tag) -> bool {
        HttpResponse res = http.post(endpoint(sid), packet, "text/plain;charset=UTF-8");
        if (!res.success) {
            out("[" + tag + "] POST failed status=" + std::to_string(res.statusCode) +
                (res.error.empty() ? "" : (" err=" + res.error)));
            return false;
        }
        return true;
    };

    // Handle one Engine.IO packet pulled out of a poll body.
    auto handlePacket = [&](const std::string& pkt) {
        if (pkt.empty()) return;
        switch (pkt[0]) {
            case '0':  // open (only seen on handshake; harmless if it recurs)
                out("[eio] open " + pkt.substr(0, 80));
                break;
            case '1':  // close
                out("[eio] server closed transport");
                kill();
                break;
            case '2':  // ping -> pong
                out("[eio] ping -> pong");
                sendPacket("3", "pong");
                break;
            case '3':  // pong (we don't initiate pings, but log if seen)
                out("[eio] pong");
                break;
            case '4': {  // Socket.IO message
                if (pkt.size() < 2) { out("[sio] empty message"); break; }
                const char st = pkt[1];
                const std::string rest = pkt.substr(2);
                if (st == '0') {
                    out("[sio] namespace connected " + rest);
                } else if (st == '1') {
                    out("[sio] namespace disconnected");
                    kill();
                } else if (st == '2') {  // EVENT
                    const std::string name = nthQuoted(rest, 1);
                    if (name == "slPing") {
                        const std::string secret = nthQuoted(rest, 2);
                        out("[sl] slPing secret=" + secret + " -> slPong");
                        sendPacket("42[\"slPong\",\"" + jsonEscape(secret) + "\"]", "slPong");
                    } else {
                        out("[event] " + name + "  " + rest.substr(0, 220));
                    }
                } else if (st == '4') {  // CONNECT_ERROR
                    out("[sio] connect error " + rest);
                    kill();
                } else {
                    out("[sio] type " + std::string(1, st) + "  " + rest.substr(0, 120));
                }
                break;
            }
            case '6':  // noop
                break;
            default:
                out("[eio] type " + std::string(1, pkt[0]) + "  " + pkt.substr(0, 80));
                break;
        }
    };

    // One long-poll GET; split the body on 0x1e and dispatch each packet.
    auto pollOnce = [&]() -> bool {
        HttpRequest req;
        req.url     = endpoint(sid);
        req.method  = "GET";
        req.timeout = 35;  // > pingInterval so a healthy held poll isn't cut short
        HttpResponse res = http.request(req);
        if (!alive()) return false;
        if (!res.success) {
            out("[poll] GET failed status=" + std::to_string(res.statusCode) +
                (res.error.empty() ? "" : (" err=" + res.error)));
            return false;
        }
        if (res.statusCode == 400) {  // session invalidated by the server
            out("[poll] 400 session closed: " + res.body.substr(0, 120));
            kill();
            return false;
        }
        size_t start = 0;
        const std::string& b = res.body;
        while (start <= b.size()) {
            size_t sep = b.find('\x1e', start);
            handlePacket(sep == std::string::npos ? b.substr(start)
                                                  : b.substr(start, sep - start));
            if (sep == std::string::npos) break;
            start = sep + 1;
        }
        return true;
    };

    // ── 2. Open the default Socket.IO namespace ───────────────────────────
    out("[connect] POST 40 (open default namespace)");
    if (!sendPacket("40", "connect")) { kill(); return; }
    // Drain the connect ack (and any keepalive the server fires immediately).
    if (alive()) pollOnce();

    // ── 3. Join the room ──────────────────────────────────────────────────
    const std::string join =
        std::string("42[\"join\",{")
        + "\"roomId\":\""               + jsonEscape(cfg.room)     + "\","
        + "\"desiredUsername\":\""      + jsonEscape(cfg.username) + "\","
        + "\"desiredPartyPausingEnabled\":false,"
        + "\"desiredAutoHostEnabled\":false,"
        + "\"thumb\":\"\","
        + "\"syncFlexibility\":"        + std::to_string(cfg.syncFlexibility) + ","
        + "\"playerProduct\":\"VitaPlex\","
        + "\"state\":\"stopped\","
        + "\"time\":0,"
        + "\"duration\":0,"
        + "\"playbackRate\":0,"
        + "\"media\":null"
        + "}]";
    out("[join] emit join room=\"" + cfg.room + "\" as \"" + cfg.username + "\"");
    if (!sendPacket(join, "join")) { kill(); return; }

    // Publish the sid only now, so the outbound send channel can't fire an
    // event before we've actually joined the room (the server would drop it).
    {
        std::lock_guard<std::mutex> lk(session->mtx);
        session->sid = sid;
    }

    // ── 4. Poll loop (bounded for the spike) ──────────────────────────────
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(cfg.spikeSeconds > 0 ? cfg.spikeSeconds : 120);
    int consecutiveFails = 0;
    while (alive()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            out("[worker] spike time limit reached, stopping");
            break;
        }
        if (pollOnce()) {
            consecutiveFails = 0;
        } else if (alive()) {
            if (++consecutiveFails >= 5) {
                out("[worker] giving up after 5 consecutive poll failures");
                break;
            }
            // Back off so a refused/erroring endpoint doesn't hot-loop.
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    kill();
    out("[worker] stopped");
}

SyncLoungeClient::~SyncLoungeClient() {
    stop();
}

void SyncLoungeClient::start(const Config& config, LogFn log) {
    if (running()) return;
    auto session = std::make_shared<Session>();
    m_session = session;

    Config cfg = config;
    LogFn  cb  = std::move(log);
    // Larger stack: the mbedtls TLS handshake under HTTPS has a deep call
    // chain that overflows the default console thread stacks.
    asyncRunLargeStack([session, cfg, cb]() {
        SyncLoungeClient::runWorker(session, cfg, cb);
    });
}

void SyncLoungeClient::stop() {
    if (m_session) m_session->running.store(false);
}

bool SyncLoungeClient::running() const {
    return m_session && m_session->running.load();
}

bool SyncLoungeClient::sendChatMessage(const std::string& text) {
    return emitFrame("42[\"sendMessage\",\"" + jsonEscape(text) + "\"]");
}

bool SyncLoungeClient::emitFrame(const std::string& frame) {
    auto session = m_session;
    if (!session) return false;

    std::string url;
    {
        std::lock_guard<std::mutex> lk(session->mtx);
        if (session->sid.empty()) return false;  // not connected yet
        url = session->endpointPrefix + "?EIO=4&transport=polling&t=" +
              std::to_string(session->sendTbust++) + "&sid=" + session->sid;
    }

    // Fire-and-forget on a fresh handle so the held-open poll isn't disturbed.
    std::string body = frame;
    asyncRun([url, body]() {
        HttpClient sender;
        sender.setFollowRedirects(true);
        sender.post(url, body, "text/plain;charset=UTF-8");
    });
    return true;
}

}  // namespace vitaplex
