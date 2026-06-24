/**
 * VitaPlex - SyncLounge session implementation.
 */

#include "app/synclounge_session.hpp"

#include <borealis.hpp>

#include <cctype>
#include <chrono>
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
    }
}

}  // namespace vitaplex
