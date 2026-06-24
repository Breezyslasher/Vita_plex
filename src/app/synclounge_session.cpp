/**
 * VitaPlex - SyncLounge session implementation.
 */

#include "app/synclounge_session.hpp"

#include <borealis.hpp>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <string>

namespace vitaplex {

namespace {

// Value of a `"key":"string"` pair (first match, scanning left to right).
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

// Value of a `"key":<number>` pair (int or float). Returns 0 if absent.
double jsonDouble(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) return 0.0;
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return 0.0;
    size_t p = colon + 1;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) p++;
    size_t start = p;
    while (p < json.size()) {
        char c = json[p];
        if (std::isdigit((unsigned char)c) || c == '.' || c == '-' ||
            c == '+' || c == 'e' || c == 'E') { p++; }
        else break;
    }
    if (p == start) return 0.0;
    try { return std::stod(json.substr(start, p - start)); }
    catch (...) { return 0.0; }
}

// The n-th (1-based) double-quoted token, escapes unwrapped. For an event
// array `["newHost","<id>"]`, token 2 is the host id.
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

}  // namespace

SyncLoungeSession& SyncLoungeSession::instance() {
    static SyncLoungeSession s;
    return s;
}

void SyncLoungeSession::connect(const std::string& server, const std::string& room,
                                const std::string& username, LogFn log) {
    // Tear down any prior client first (outside the lock — stop() is cheap but
    // we don't want to hold the mutex across it).
    std::shared_ptr<SyncLoungeClient> old;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        old      = m_client;
        m_client = nullptr;
        m_server = server;
        m_room   = room;
        m_remote = RemoteState{};
    }
    if (old) old->stop();

    auto client = std::make_shared<SyncLoungeClient>();
    client->setEventCallback([this](const std::string& name, const std::string& payload) {
        onEvent(name, payload);
    });

    SyncLoungeClient::Config cfg;
    cfg.server       = server;
    cfg.room         = room;
    cfg.username     = username.empty() ? std::string("VitaPlex") : username;
    cfg.spikeSeconds = 0;  // persistent — run until disconnect()

    client->start(cfg, std::move(log));

    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_client = client;
    }
}

void SyncLoungeSession::disconnect() {
    std::shared_ptr<SyncLoungeClient> old;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        old      = m_client;
        m_client = nullptr;
        m_remote = RemoteState{};
    }
    if (old) old->stop();
}

bool SyncLoungeSession::isConnected() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_client && m_client->running();
}

std::string SyncLoungeSession::room() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_room;
}

SyncLoungeSession::RemoteState SyncLoungeSession::remoteState() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_remote;
}

void SyncLoungeSession::onEvent(const std::string& name, const std::string& payload) {
    // Both events carry the host's transport state as a flat object:
    //   {updatedAt, state, time, duration, playbackRate, [media], id}
    // The top-level fields are serialized before the (optional) media object,
    // so a left-to-right key scan picks them up even though media may carry
    // its own "duration".
    if (name == "playerStateUpdate" || name == "mediaUpdate") {
        RemoteState rs;
        rs.valid        = true;
        rs.state        = jsonStr(payload, "state");
        rs.timeMs       = jsonDouble(payload, "time");
        rs.durationMs   = jsonDouble(payload, "duration");
        double pr       = jsonDouble(payload, "playbackRate");
        rs.playbackRate = pr > 0.0 ? pr : 1.0;
        rs.at           = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_remote = rs;
        }
        brls::Logger::debug("SyncLounge: host {} state={} time={}ms",
                            name, rs.state, (long)rs.timeMs);
    } else if (name == "joinResult") {
        // payload: ["joinResult",{...,"hostId":"H","user":{"id":"SELF",...},...}]
        // The first "id" key belongs to the user object (our socket id).
        const std::string self = jsonStr(payload, "id");
        const std::string host = jsonStr(payload, "hostId");
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (!self.empty()) m_selfId = self;
            if (!host.empty()) m_hostId = host;
        }
        brls::Logger::info("SyncLounge: joined self={} host={} isHost={}",
                           self, host, (!self.empty() && self == host));
    } else if (name == "newHost") {
        // payload: ["newHost","<hostId>"]
        const std::string host = nthQuoted(payload, 2);
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (!host.empty()) m_hostId = host;
        }
        brls::Logger::info("SyncLounge: newHost={}", host);
    }
}

bool SyncLoungeSession::isHost() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return !m_selfId.empty() && m_selfId == m_hostId;
}

void SyncLoungeSession::reportLocalState(const std::string& state, double timeMs,
                                         double durationMs, double playbackRate) {
    std::shared_ptr<SyncLoungeClient> client;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_client) return;
        // Throttle: emit immediately on a state change, otherwise at most once
        // every few seconds, so steady playback doesn't flood the POST channel.
        const auto now = std::chrono::steady_clock::now();
        const bool changed = (state != m_lastSentState);
        const bool due     = (now - m_lastSentAt) >= std::chrono::seconds(3);
        if (!changed && !due) return;
        m_lastSentState = state;
        m_lastSentAt    = now;
        client          = m_client;
    }

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "{\"state\":\"%s\",\"time\":%.0f,\"duration\":%.0f,\"playbackRate\":%.3f}",
                  state.c_str(), timeMs, durationMs, playbackRate);
    client->emitEvent("playerStateUpdate", buf);
}

void SyncLoungeSession::reportUserAction(const std::string& state, double timeMs,
                                         double durationMs, double playbackRate) {
    std::shared_ptr<SyncLoungeClient> client;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_client) return;
        client = m_client;
        // Count this as our latest broadcast so the periodic playerStateUpdate
        // throttle doesn't immediately re-announce the same thing.
        m_lastSentState = state;
        m_lastSentAt    = std::chrono::steady_clock::now();
    }

    // media:null for now — cross-server content matching is a later step, and
    // the server's host-claim path (userInitiated && !isUserHost &&
    // isAutoHostEnabled) doesn't depend on the media payload.
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "{\"state\":\"%s\",\"time\":%.0f,\"duration\":%.0f,"
                  "\"playbackRate\":%.3f,\"media\":null,\"userInitiated\":true}",
                  state.c_str(), timeMs, durationMs, playbackRate);
    client->emitEvent("mediaUpdate", buf);
}

}  // namespace vitaplex
