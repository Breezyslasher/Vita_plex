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

// Value of a `"key":true/false` pair. False if absent.
bool jsonBool(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) return false;
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return false;
    size_t p = colon + 1;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) p++;
    return p < json.size() && json[p] == 't';
}

// A bare-bool event array — `["name",true]` / `["name",false]`. Our bare-bool
// event names contain no "true", so a plain scan is unambiguous.
bool barePayloadTrue(const std::string& payload) {
    return payload.find("true") != std::string::npos;
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

// Last run of digits in a string as an int (e.g. "Season 12" -> 12). -1 if none.
int parseLastNumber(const std::string& s) {
    int end = -1;
    for (int i = (int)s.size() - 1; i >= 0; i--)
        if (std::isdigit((unsigned char)s[i])) { end = i; break; }
    if (end < 0) return -1;
    int start = end;
    while (start > 0 && std::isdigit((unsigned char)s[start - 1])) start--;
    try { return std::stoi(s.substr(start, end - start + 1)); }
    catch (...) { return -1; }
}

// Resolve a movie (or other flat title) against our library by title/year.
SyncLoungeSession::MatchResult resolveTitleMatch(const SyncLoungeSession::HostMedia& hm) {
    SyncLoungeSession::MatchResult best;
    best.resolved = true;
    best.forTitle = hm.title;

    std::vector<MediaItem> results;
    if (!hm.title.empty()) PlexClient::getInstance().search(hm.title, results);

    int  bestScore = 0;
    bool bestExact = false;
    for (const auto& it : results) {
        if (!hm.type.empty() && !it.type.empty() && it.type != hm.type) continue;
        bool exactTitle = iequals(it.title, hm.title);
        int  score      = 1;
        if (exactTitle) score += 4;
        else if (icontains(it.title, hm.title) || icontains(hm.title, it.title)) score += 1;
        else continue;  // needs some title overlap
        if (hm.year > 0 && it.year == hm.year) score += 3;
        if (score > bestScore) {
            bestScore      = score;
            bestExact      = exactTitle;
            best.ratingKey = it.ratingKey;
            best.title     = it.title;
        }
    }
    if (bestScore < 2) { best.ratingKey.clear(); best.title.clear(); }
    best.exact = bestExact && !best.ratingKey.empty();
    return best;
}

// Resolve an episode cross-server by navigating show -> season -> episode. The
// host's media only gives names (show=grandparentTitle, season=parentTitle as a
// string, episode=title) — no numbers — so we match on those.
SyncLoungeSession::MatchResult resolveEpisodeMatch(const SyncLoungeSession::HostMedia& hm) {
    SyncLoungeSession::MatchResult best;
    best.resolved = true;
    best.forTitle = hm.title;
    PlexClient& client = PlexClient::getInstance();

    // 1. Find the show.
    std::vector<MediaItem> shows;
    if (!hm.grandparentTitle.empty()) client.search(hm.grandparentTitle, shows);
    std::string showKey;
    for (const auto& it : shows)
        if (it.type == "show" && iequals(it.title, hm.grandparentTitle)) { showKey = it.ratingKey; break; }
    if (showKey.empty())
        for (const auto& it : shows)
            if (it.type == "show" &&
                (icontains(it.title, hm.grandparentTitle) || icontains(hm.grandparentTitle, it.title))) {
                showKey = it.ratingKey; break;
            }
    if (showKey.empty()) return best;

    // 2. Find the season — by title, else by parsed number, else single-season.
    std::vector<MediaItem> seasons;
    client.fetchChildren(showKey, seasons);
    std::string seasonKey;
    for (const auto& s : seasons)
        if (s.mediaType == MediaType::SEASON && iequals(s.title, hm.parentTitle)) { seasonKey = s.ratingKey; break; }
    if (seasonKey.empty()) {
        const int want = parseLastNumber(hm.parentTitle);
        if (want > 0)
            for (const auto& s : seasons)
                if (s.mediaType == MediaType::SEASON && s.index == want) { seasonKey = s.ratingKey; break; }
    }
    if (seasonKey.empty()) {
        int count = 0; std::string only;
        for (const auto& s : seasons) if (s.mediaType == MediaType::SEASON) { count++; only = s.ratingKey; }
        if (count == 1) seasonKey = only;  // single-season shows skip the grouping
    }
    if (seasonKey.empty()) return best;

    // 3. Find the episode by title.
    std::vector<MediaItem> episodes;
    client.fetchChildren(seasonKey, episodes);
    for (const auto& e : episodes)
        if (e.mediaType == MediaType::EPISODE && iequals(e.title, hm.title)) {
            best.ratingKey = e.ratingKey; best.title = e.title; best.exact = true; return best;
        }
    for (const auto& e : episodes)
        if (e.mediaType == MediaType::EPISODE &&
            (icontains(e.title, hm.title) || icontains(hm.title, e.title))) {
            best.ratingKey = e.ratingKey; best.title = e.title; best.exact = false; return best;
        }
    return best;
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
        m_lastPromptKey.clear();
        m_partyPauseEnabled   = false;
        m_roomAutoHostEnabled = false;
        m_partyPaused         = false;
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
        m_lastPromptKey.clear();
        m_partyPauseEnabled   = false;
        m_roomAutoHostEnabled = false;
        m_partyPaused         = false;
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
        // Only the room HOST drives us — the server relays every member's
        // updates, so ignore anyone who isn't the current host (matters once
        // there are 3+ people). `id` is the sender's socket id.
        const std::string sender = jsonStr(payload, "id");
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (!m_hostId.empty() && !sender.empty() && sender != m_hostId) return;
        }

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

        // mediaUpdate may also carry the host's `media` object (the client
        // sends media:null on plain pause/play — those are state-only, handled
        // above, and processHostMedia ignores them so the last match stands).
        if (name == "mediaUpdate")
            processHostMedia(extractJsonObject(payload, "media"));
    } else if (name == "joinResult") {
        // payload: ["joinResult",{...,"hostId":"H","user":{"id":"SELF",...},...}]
        // The first "id" key belongs to the user object (our socket id).
        const std::string self = jsonStr(payload, "id");
        const std::string host = jsonStr(payload, "hostId");
        const bool partyPausing = jsonBool(payload, "isPartyPausingEnabled");
        const bool roomAutoHost = jsonBool(payload, "isAutoHostEnabled");
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (!self.empty()) m_selfId = self;
            if (!host.empty()) m_hostId = host;
            m_partyPauseEnabled   = partyPausing;
            m_roomAutoHostEnabled = roomAutoHost;
        }
        brls::Logger::info("SyncLounge: joined self={} host={} isHost={} partyPause={} roomAutoHost={}",
                           self, host, (!self.empty() && self == host), partyPausing, roomAutoHost);

        // Seed the host's CURRENT media + position from joinResult.users[host]
        // so connecting mid-session can offer to join (and open at the right
        // spot) what's already playing — not just future content changes.
        if (!host.empty()) {
            const std::string usersObj  = extractJsonObject(payload, "users");
            const std::string hostEntry = extractJsonObject(usersObj, host);
            if (!hostEntry.empty()) {
                const std::string st = jsonStr(hostEntry, "state");
                if (!st.empty()) {
                    RemoteState rs;
                    rs.valid        = true;
                    rs.state        = st;
                    rs.timeMs       = jsonDouble(hostEntry, "time");
                    rs.durationMs   = jsonDouble(hostEntry, "duration");
                    double pr       = jsonDouble(hostEntry, "playbackRate");
                    rs.playbackRate = pr > 0.0 ? pr : 1.0;
                    rs.at           = std::chrono::steady_clock::now();
                    std::lock_guard<std::mutex> lk(m_mtx);
                    m_remote = rs;
                }
                processHostMedia(extractJsonObject(hostEntry, "media"));
            }
        }
    } else if (name == "newHost") {
        // payload: ["newHost","<hostId>"]
        const std::string host = nthQuoted(payload, 2);
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (!host.empty()) m_hostId = host;
        }
        brls::Logger::info("SyncLounge: newHost={}", host);
    } else if (name == "userLeft") {
        // payload: ["userLeft",{"id":"<socketId>"}]
        // If the HOST left, stop following their now-frozen state — otherwise the
        // follower keeps seeking back to wherever the host was when they left.
        // Drop the host id and invalidate the remote state; the server promotes a
        // new host via newHost (possibly us), and we resume following that fresh
        // state. A non-host leaving doesn't affect us.
        const std::string who = jsonStr(payload, "id");
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (!who.empty() && who == m_hostId) {
                m_hostId.clear();
                m_remote.valid = false;
            }
        }
        brls::Logger::info("SyncLounge: userLeft {}", who);
    } else if (name == "setPartyPausingEnabled") {
        // payload: ["setPartyPausingEnabled",true|false]
        const bool enabled = barePayloadTrue(payload);
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_partyPauseEnabled = enabled;
        }
        brls::Logger::info("SyncLounge: partyPause {}", enabled ? "enabled" : "disabled");
    } else if (name == "setAutoHostEnabled") {
        // payload: ["setAutoHostEnabled",true|false] — room-wide, host-controlled.
        // When on, the server promotes any non-host who starts new media.
        const bool enabled = barePayloadTrue(payload);
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_roomAutoHostEnabled = enabled;
        }
        brls::Logger::info("SyncLounge: roomAutoHost {}", enabled ? "enabled" : "disabled");
    } else if (name == "partyPause") {
        // payload: ["partyPause",{"senderId":"...","isPause":true|false}]
        const bool isPause = jsonBool(payload, "isPause");
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_partyPaused = isPause;
            m_partyPauseSeq++;
        }
        brls::Logger::info("SyncLounge: partyPause -> {}", isPause ? "pause" : "play");
    }
}

void SyncLoungeSession::processHostMedia(const std::string& mediaObj) {
    if (mediaObj.empty()) return;  // media:null (state-only) — keep the last match

    HostMedia hm;
    hm.valid            = true;
    hm.type             = jsonStr(mediaObj, "type");
    hm.title            = jsonStr(mediaObj, "title");
    hm.year             = (int)jsonDouble(mediaObj, "year");
    hm.grandparentTitle = jsonStr(mediaObj, "grandparentTitle");
    hm.parentTitle      = jsonStr(mediaObj, "parentTitle");
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
        const std::string key = hm.type + "|" + hm.grandparentTitle + "|" +
                                hm.parentTitle + "|" + hm.title + "|" +
                                std::to_string(hm.year);
        if (key != m_resolveKey) {
            m_resolveKey = key;
            kick = true;
        }
    }
    brls::Logger::info(
        "SyncLounge: host media type={} title=\"{}\" show=\"{}\" season=\"{}\" raw={}",
        hm.type, hm.title, hm.grandparentTitle, hm.parentTitle, hm.raw);
    if (kick) resolveMatchAsync(hm);
}

bool SyncLoungeSession::isHost() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return !m_selfId.empty() && m_selfId == m_hostId;
}

bool SyncLoungeSession::isPartyPauseEnabled() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_partyPauseEnabled;
}

SyncLoungeSession::PartyPause SyncLoungeSession::partyPauseState() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return { m_partyPauseSeq, m_partyPaused };
}

void SyncLoungeSession::sendPartyPause(bool isPause) {
    std::shared_ptr<SyncLoungeClient> client;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        // The server disconnects a sender if party-pausing isn't enabled.
        if (!m_client || !m_partyPauseEnabled) return;
        client = m_client;
    }
    client->emitEvent("partyPause", isPause ? "true" : "false");
}

void SyncLoungeSession::setPartyPauseEnabled(bool enabled) {
    std::shared_ptr<SyncLoungeClient> client;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        // Server-gated to the host; sending as a non-host disconnects us.
        if (!m_client || m_selfId.empty() || m_selfId != m_hostId) return;
        client = m_client;
    }
    client->emitEvent("setPartyPausingEnabled", enabled ? "true" : "false");
}

bool SyncLoungeSession::isRoomAutoHostEnabled() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_roomAutoHostEnabled;
}

void SyncLoungeSession::setRoomAutoHostEnabled(bool enabled) {
    std::shared_ptr<SyncLoungeClient> client;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        // Server-gated to the host; sending as a non-host disconnects us.
        if (!m_client || m_selfId.empty() || m_selfId != m_hostId) return;
        client = m_client;
    }
    client->emitEvent("setAutoHostEnabled", enabled ? "true" : "false");
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
    // `this` is a singleton — always valid. PlexClient calls are background-safe
    // (each uses its own HttpClient), so resolve off the worker thread.
    asyncRun([this, hm]() {
        MatchResult best;
        best.resolved = true;
        best.forTitle = hm.title;

        if (!hm.machineIdentifier.empty() && !hm.hostRatingKey.empty() &&
            hm.machineIdentifier == PlexClient::getInstance().getMachineIdentifier()) {
            // Same Plex server -> the host's ratingKey is exactly our item.
            best.exact     = true;
            best.ratingKey = hm.hostRatingKey;
            best.title     = hm.title;
        } else if (hm.type == "episode") {
            // Cross-server episode: navigate show -> season -> episode by name.
            best = resolveEpisodeMatch(hm);
        } else {
            // Cross-server movie / other: title (+ year) search.
            best = resolveTitleMatch(hm);
        }

        MatchPromptFn promptCb;
        std::string   promptTitle;
        std::string   promptKey;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            // Drop a result that arrived after the host moved to other media.
            if (m_hostMedia.title == hm.title && m_hostMedia.type == hm.type)
                m_match = best;
            // Offer to join once per distinct, confidently-matched host item.
            if (best.exact && !best.ratingKey.empty() && best.ratingKey != m_lastPromptKey) {
                m_lastPromptKey = best.ratingKey;
                promptCb        = m_promptCb;
                promptKey       = best.ratingKey;
                promptTitle     = (hm.type == "episode" && !hm.grandparentTitle.empty())
                                      ? (hm.grandparentTitle + " - " + hm.title)
                                      : hm.title;
            }
        }
        if (promptCb)
            brls::sync([promptCb, promptKey, promptTitle]() { promptCb(promptKey, promptTitle); });

        if (best.ratingKey.empty())
            brls::Logger::info("SyncLounge match: host \"{}\" ({}) show=\"{}\" -> no local match",
                               hm.title, hm.type, hm.grandparentTitle);
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

void SyncLoungeSession::announceLocalMedia(const std::string& state, double timeMs,
                                           double durationMs, bool claimHost) {
    // claimHost == the server's userInitiated flag. Only true on a
    // user-initiated NEW video; pause/play/seek and follow-loads pass false so
    // they never steal host. The server only acts on it when the room's
    // auto-host (Room Auto Host) is enabled — that's the gate.
    emitMediaUpdate(state, timeMs, durationMs, 1.0, /*userInitiated=*/claimHost);
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
                    "\",\"type\":\"" + jsonEscape(m_localType) + "\"";
            // Episodes: include show + season so the server renders
            // "Show - Episode" instead of "undefined - Episode".
            if (!m_localGrandparentTitle.empty())
                media += ",\"grandparentTitle\":\"" + jsonEscape(m_localGrandparentTitle) + "\"";
            if (!m_localParentTitle.empty())
                media += ",\"parentTitle\":\"" + jsonEscape(m_localParentTitle) + "\"";
            media += ",\"ratingKey\":\"" + jsonEscape(m_localRatingKey) +
                     "\",\"machineIdentifier\":\"" + jsonEscape(m_localMachineId) + "\"}";
        }
        // Count this as our latest broadcast so the periodic playerStateUpdate
        // throttle doesn't immediately duplicate it.
        m_lastSentState = state;
        m_lastSentAt    = std::chrono::steady_clock::now();
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
                                      const std::string& ratingKey,
                                      const std::string& grandparentTitle,
                                      const std::string& parentTitle) {
    const std::string machineId = PlexClient::getInstance().getMachineIdentifier();
    std::lock_guard<std::mutex> lk(m_mtx);
    m_localTitle            = title;
    m_localType             = type;
    m_localRatingKey        = ratingKey;
    m_localMachineId        = machineId;
    m_localGrandparentTitle = grandparentTitle;
    m_localParentTitle      = parentTitle;
}

void SyncLoungeSession::clearLocalMedia() {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_localTitle.clear();
    m_localType.clear();
    m_localRatingKey.clear();
    m_localMachineId.clear();
    m_localGrandparentTitle.clear();
    m_localParentTitle.clear();
}

}  // namespace vitaplex
