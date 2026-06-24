/**
 * VitaPlex - SyncLounge session implementation.
 */

#include "app/synclounge_session.hpp"
#include "app/plex_client.hpp"
#include "utils/async.hpp"

#include <borealis.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

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

// The balanced `{...}` object value of `"key":{...}` (or "" if the value is
// null / not an object). Brace-counting only — fine for the flat media object,
// whose string values don't contain braces.
std::string extractJsonObject(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) return "";
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return "";
    size_t p = colon + 1;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) p++;
    if (p >= json.size() || json[p] != '{') return "";  // null or non-object
    int depth = 0;
    const size_t start = p;
    for (; p < json.size(); p++) {
        if (json[p] == '{') depth++;
        else if (json[p] == '}') { if (--depth == 0) return json.substr(start, p - start + 1); }
    }
    return "";
}

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

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool icontains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return false;
    return lower(hay).find(lower(needle)) != std::string::npos;
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
        old          = m_client;
        m_client     = nullptr;
        m_server     = server;
        m_room       = room;
        m_remote     = RemoteState{};
        m_hostMedia  = HostMedia{};
        m_match      = MatchResult{};
        m_resolveKey.clear();
        m_selfId.clear();
        m_hostId.clear();
        m_lastSentState.clear();
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
        old          = m_client;
        m_client     = nullptr;
        m_remote     = RemoteState{};
        m_hostMedia  = HostMedia{};
        m_match      = MatchResult{};
        m_resolveKey.clear();
        m_selfId.clear();
        m_hostId.clear();
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

        // mediaUpdate may also carry the host's `media` object. But the client
        // sends mediaUpdate with media:null on plain state changes (pause/play)
        // too — those are state-only (handled above) and must NOT wipe the last
        // known media/match, or an in-flight resolve gets dropped by the
        // staleness check. Only (re)parse a real media object.
        if (name == "mediaUpdate") {
            const std::string mediaObj = extractJsonObject(payload, "media");
            if (!mediaObj.empty()) {
                HostMedia hm;
                hm.valid            = true;
                hm.type             = jsonStr(mediaObj, "type");
                hm.title            = jsonStr(mediaObj, "title");
                hm.year             = (int)jsonDouble(mediaObj, "year");
                hm.grandparentTitle = jsonStr(mediaObj, "grandparentTitle");
                hm.parentIndex      = (int)jsonDouble(mediaObj, "parentIndex");
                hm.index            = (int)jsonDouble(mediaObj, "index");
                hm.machineIdentifier= jsonStr(mediaObj, "machineIdentifier");
                hm.hostRatingKey    = jsonStr(mediaObj, "ratingKey");
                hm.raw              = mediaObj.substr(0, 500);

                bool kick = false;
                {
                    std::lock_guard<std::mutex> lk(m_mtx);
                    m_hostMedia = hm;
                    // Debounce: only re-resolve when the media identity changes.
                    const std::string key = hm.type + "|" + hm.grandparentTitle +
                                            "|" + hm.title + "|" + std::to_string(hm.year);
                    if (key != m_resolveKey) {
                        m_resolveKey = key;
                        kick = true;
                    }
                }
                brls::Logger::info(
                    "SyncLounge: host media type={} title=\"{}\" year={} show=\"{}\" s{}e{} raw={}",
                    hm.type, hm.title, hm.year, hm.grandparentTitle,
                    hm.parentIndex, hm.index, hm.raw);
                if (kick) resolveMatchAsync(hm);
            }
        }
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

SyncLoungeSession::HostMedia SyncLoungeSession::hostMedia() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_hostMedia;
}

SyncLoungeSession::MatchResult SyncLoungeSession::match() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_match;
}

void SyncLoungeSession::resolveMatchAsync(HostMedia hm) {
    // `this` is a singleton — always valid. PlexClient::search is
    // background-safe (uses its own HttpClient), so search off the worker
    // thread rather than blocking it.
    asyncRun([this, hm]() {
        MatchResult best;
        best.resolved = true;
        best.forTitle = hm.title;

        // Fast path: same Plex server (machineIdentifier matches ours) -> the
        // host's ratingKey is exactly our item, no search needed.
        if (!hm.machineIdentifier.empty() && !hm.hostRatingKey.empty() &&
            hm.machineIdentifier == PlexClient::getInstance().getMachineIdentifier()) {
            best.exact     = true;
            best.ratingKey = hm.hostRatingKey;
            best.title     = hm.title;
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                if (m_hostMedia.title == hm.title && m_hostMedia.type == hm.type)
                    m_match = best;
            }
            brls::Logger::info("SyncLounge match: host \"{}\" ({}) -> same-server ratingKey={} (exact)",
                               hm.title, hm.type, best.ratingKey);
            return;
        }

        // Cross-server: search our library and score candidates. Wrong type is
        // disqualifying; otherwise prefer an exact title, then year (movies) /
        // show (episodes). Episodes search the show title (episode titles are
        // often generic).
        std::vector<MediaItem> results;
        const std::string query = (hm.type == "episode" && !hm.grandparentTitle.empty())
                                       ? hm.grandparentTitle
                                       : hm.title;
        if (!query.empty()) PlexClient::getInstance().search(query, results);

        int  bestScore = 0;
        bool bestExactTitle = false;
        for (const auto& it : results) {
            if (!hm.type.empty() && !it.type.empty() && it.type != hm.type) continue;
            int  score = 1;
            bool exactTitle = iequals(it.title, hm.title);
            if (exactTitle) score += 4;
            else if (icontains(it.title, hm.title) || icontains(hm.title, it.title)) score += 1;
            else if (hm.type != "episode") continue;  // a movie needs some title overlap
            if (hm.type == "movie" && hm.year > 0 && it.year == hm.year) score += 3;
            if (hm.type == "episode" && !hm.grandparentTitle.empty() &&
                iequals(it.grandparentTitle, hm.grandparentTitle)) score += 3;
            if (score > bestScore) {
                bestScore      = score;
                bestExactTitle = exactTitle;
                best.ratingKey = it.ratingKey;
                best.title     = it.title;
            }
        }
        if (bestScore < 2) { best.ratingKey.clear(); best.title.clear(); }
        // Auto-switch only on a confident match: an exact title of the right
        // type. Looser (substring) matches are stored but flagged inexact.
        best.exact = bestExactTitle && !best.ratingKey.empty();

        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_hostMedia.title == hm.title && m_hostMedia.type == hm.type)
                m_match = best;
        }
        if (best.ratingKey.empty())
            brls::Logger::info("SyncLounge match: host \"{}\" ({}) -> no local match among {} results",
                               hm.title, hm.type, results.size());
        else
            brls::Logger::info("SyncLounge match: host \"{}\" ({}) -> local ratingKey={} \"{}\" (exact={})",
                               hm.title, hm.type, best.ratingKey, best.title, best.exact);
    });
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
    // A manual play/pause/seek: announce with userInitiated=true (claims host
    // under auto-host) and our real media.
    emitMediaUpdate(state, timeMs, durationMs, playbackRate, /*userInitiated=*/true);
}

void SyncLoungeSession::announceLocalMedia(const std::string& state, double timeMs,
                                           double durationMs) {
    // Tell the room what we're playing without claiming host.
    emitMediaUpdate(state, timeMs, durationMs, 1.0, /*userInitiated=*/false);
}

void SyncLoungeSession::emitMediaUpdate(const std::string& state, double timeMs,
                                        double durationMs, double playbackRate,
                                        bool userInitiated) {
    std::shared_ptr<SyncLoungeClient> client;
    std::string media = "null";
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_client) return;
        client = m_client;
        if (!m_localRatingKey.empty()) {
            media = "{\"title\":\"" + jsonEscape(m_localTitle) +
                    "\",\"type\":\"" + jsonEscape(m_localType) +
                    "\",\"ratingKey\":\"" + jsonEscape(m_localRatingKey) +
                    "\",\"machineIdentifier\":\"" + jsonEscape(m_localMachineId) + "\"}";
        }
        if (userInitiated) {
            // Count this as our latest broadcast so the periodic
            // playerStateUpdate throttle doesn't immediately duplicate it.
            m_lastSentState = state;
            m_lastSentAt    = std::chrono::steady_clock::now();
        }
    }

    // time/duration are whole milliseconds; format as integers to avoid the
    // server choking on a bad/NaN double (NaN:NaN). Callers must pass finite
    // values — invalid readings are filtered upstream.
    char nums[128];
    std::snprintf(nums, sizeof(nums),
                  "\"time\":%lld,\"duration\":%lld,\"playbackRate\":%.3f",
                  (long long)timeMs, (long long)durationMs, playbackRate);
    std::string frame = "{\"state\":\"" + jsonEscape(state) + "\"," + nums +
                        ",\"media\":" + media +
                        ",\"userInitiated\":" + (userInitiated ? "true" : "false") + "}";
    client->emitEvent("mediaUpdate", frame);
}

void SyncLoungeSession::setLocalMedia(const std::string& title, const std::string& type,
                                      const std::string& ratingKey) {
    const std::string machineId = PlexClient::getInstance().getMachineIdentifier();
    std::lock_guard<std::mutex> lk(m_mtx);
    m_localTitle     = title;
    m_localType      = type;
    m_localRatingKey = ratingKey;
    m_localMachineId = machineId;
}

void SyncLoungeSession::clearLocalMedia() {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_localTitle.clear();
    m_localType.clear();
    m_localRatingKey.clear();
    m_localMachineId.clear();
}

}  // namespace vitaplex
