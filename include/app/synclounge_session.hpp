/**
 * VitaPlex - SyncLounge session (app-level, persistent)
 *
 * Owns one SyncLoungeClient for the lifetime of the app and keeps the latest
 * host playback state so an active player can follow the watch party.
 *
 * Receive-only for now: the Vita never claims host — it just mirrors the
 * party's play / pause / seek onto the local player. Sending our own state
 * (host / auto-host) and cross-server content matching are later steps.
 *
 * Singleton because both the player loop and the settings screen need to reach
 * the same connection. The SyncLoungeClient delivers events on its worker
 * thread, so the stored state is mutex-guarded; the player reads it from the
 * UI thread.
 */

#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <chrono>
#include <functional>

#include "app/synclounge_client.hpp"

namespace vitaplex {

class SyncLoungeSession {
public:
    static SyncLoungeSession& instance();

    using LogFn = std::function<void(const std::string&)>;

    // Connect (or reconnect) to a room. `log` (optional) receives raw protocol
    // lines for a debug view and is invoked on the worker thread — marshal to
    // the UI thread inside it if it touches views. Replaces any prior client.
    void connect(const std::string& server, const std::string& room,
                 const std::string& username, LogFn log = nullptr);
    void disconnect();

    bool        isConnected() const;
    std::string room() const;

    // ── Party pause ───────────────────────────────────────────────────────
    // When the room has party-pausing enabled, ANY member can pause/resume the
    // whole party. We mirror inbound partyPause onto the local player and, on a
    // user pause/play, broadcast it.
    bool isPartyPauseEnabled() const;
    // Latest party pause/resume action. seq bumps on each inbound partyPause so
    // the player applies it exactly once.
    struct PartyPause { int seq = 0; bool isPause = false; };
    PartyPause partyPauseState() const;
    // Broadcast a pause/resume to the party. No-op (and never sent) unless
    // party-pausing is enabled — the server disconnects a sender otherwise.
    void sendPartyPause(bool isPause);
    // Enable/disable party-pausing for the room. Server-gated to the host;
    // sending as a non-host disconnects us, so this is a no-op unless isHost().
    void setPartyPauseEnabled(bool enabled);

    // True once we've joined and the room's host is us (our socket id equals
    // the host id). Tracked from joinResult + newHost events.
    bool isHost() const;

    // ── Room auto-host ────────────────────────────────────────────────────
    // Server-side, room-wide switch: when enabled, the server promotes ANY
    // non-host member who starts new media (userInitiated) to host. Mirrors the
    // party-pause control — host-only to change (the server disconnects a
    // non-host sender) and the new value is broadcast to the whole room.
    //
    // This is the room switch the server checks in its makeHost rule; the
    // per-device "Auto Host" preference (setAutoHost) decides whether THIS
    // client actually sends userInitiated when allowed. Both must be on for a
    // non-host to take over by playing.
    bool isRoomAutoHostEnabled() const;
    void setRoomAutoHostEnabled(bool enabled);

    // Broadcast our local player state to the room (only meaningful when we're
    // host — the server relays playerStateUpdate to the other members). Called
    // each second from the player loop; throttled internally to emit on state
    // change and at most every few seconds otherwise. time/duration are ms.
    void reportLocalState(const std::string& state, double timeMs,
                          double durationMs, double playbackRate);

    // Tell the session what we're locally playing so our outbound mediaUpdates
    // carry a real `media` object (title/type/ratingKey/our machineIdentifier,
    // plus show/season for episodes) instead of null — otherwise the
    // server/party shows us as "Nothing" (or "undefined - <episode>").
    void setLocalMedia(const std::string& title, const std::string& type,
                       const std::string& ratingKey,
                       const std::string& grandparentTitle = "",
                       const std::string& parentTitle = "");
    void clearLocalMedia();

    // Announce our current media + transport state to the room as a
    // mediaUpdate. claimHost maps to the server's userInitiated flag: pass true
    // ONLY when the user starts a new video (under auto-host that takes host so
    // the party follows us). Pass false for pause/play/seek and for content we
    // auto-loaded to FOLLOW the host — those must never steal host.
    void announceLocalMedia(const std::string& state, double timeMs,
                            double durationMs, bool claimHost);

    // Local "auto host" opt-in (default off). When off, announceLocalMedia never
    // sets userInitiated even for a user-started new video, so a non-host client
    // never takes over the party. When on, starting new media here claims host
    // (under the room's auto-host). Persisted in settings; pushed here at startup,
    // on connect, and when the user toggles it.
    void setAutoHost(bool enabled);
    bool autoHost() const;

    // Latest host state, mirrored from playerStateUpdate / mediaUpdate. Time
    // and duration are milliseconds (Plex / SyncLounge convention).
    struct RemoteState {
        bool        valid        = false;
        std::string state;             // "playing" / "paused" / "buffering" / ...
        double      timeMs       = 0.0;
        double      durationMs   = 0.0;
        double      playbackRate = 1.0;
        // When this state was received (steady clock). Used to judge freshness
        // and extrapolate a playing host forward at playback-start.
        std::chrono::steady_clock::time_point at{};
    };
    RemoteState remoteState() const;

    // What the host is playing, parsed from the mediaUpdate `media` object.
    // Cross-server: ratingKey/key are the HOST's and useless to us, so we
    // match on title/year/show/season/episode against our own library.
    struct HostMedia {
        bool        valid = false;
        std::string type;             // "movie" / "episode" / ...
        std::string title;
        int         year = 0;
        std::string grandparentTitle; // show (episodes)
        std::string parentTitle;      // season name, e.g. "Season 1" (episodes)
        int         parentIndex = 0;  // season number (host usually omits)
        int         index = 0;        // episode number (host usually omits)
        std::string machineIdentifier;// host server id (== ours => same server)
        std::string hostRatingKey;    // host server ratingKey (exact when same server)
        std::string raw;              // raw media JSON (debug)
    };
    HostMedia hostMedia() const;

    // Result of resolving the host's media against our local library. ratingKey
    // is empty when no confident match was found. `exact` marks a high-
    // confidence match (same Plex server, or an exact title+type match) that's
    // safe to auto-switch the player to. `forTitle` records which host title
    // this is for, so a stale match isn't applied to new media.
    struct MatchResult {
        bool        resolved = false;
        bool        exact    = false;
        std::string ratingKey;
        std::string title;
        std::string forTitle;
    };
    MatchResult match() const;

    // Invoked (on the UI thread) when the host starts NEW content that resolves
    // to a confident local match, so a coordinator can offer to join. Fires
    // once per distinct host item; the handler decides whether to actually
    // prompt (e.g. skip when already in a player). Set once at startup.
    using MatchPromptFn = std::function<void(const std::string& ratingKey,
                                             const std::string& title)>;
    void setMatchPromptCallback(MatchPromptFn cb) { m_promptCb = std::move(cb); }

private:
    SyncLoungeSession() = default;
    SyncLoungeSession(const SyncLoungeSession&)            = delete;
    SyncLoungeSession& operator=(const SyncLoungeSession&) = delete;

    // Invoked by the client on its worker thread for each inbound event.
    void onEvent(const std::string& name, const std::string& payload);

    // Parse a host `media` object (from mediaUpdate or joinResult.users[host]),
    // store it, and kick a background resolve. No-op for an empty/null object.
    void processHostMedia(const std::string& mediaObj);

    // Build + emit a mediaUpdate carrying our current local media (or null).
    void emitMediaUpdate(const std::string& state, double timeMs,
                         double durationMs, double playbackRate, bool userInitiated);

    // Kick a background search of our library for the host's media, storing
    // the result in m_match. Debounced by m_resolveKey so the same media isn't
    // re-resolved repeatedly. Runs on its own thread (PlexClient::search is
    // background-safe — it uses its own HttpClient).
    void resolveMatchAsync(HostMedia hm);

    mutable std::mutex                m_mtx;
    std::shared_ptr<SyncLoungeClient> m_client;
    std::string                       m_server;
    std::string                       m_room;
    RemoteState                       m_remote;
    HostMedia                         m_hostMedia;
    MatchResult                       m_match;
    std::string                       m_resolveKey;   // identity of the media last resolved

    // Host tracking + outbound throttle (all guarded by m_mtx).
    std::string                           m_selfId;       // our socket id (from joinResult)
    std::string                           m_hostId;       // current room host id
    bool                                  m_autoHost = false;  // local claim-host opt-in (default off)
    std::string                           m_lastSentState;
    std::chrono::steady_clock::time_point m_lastSentAt{};

    // What we're locally playing, for outbound mediaUpdates.
    std::string                           m_localTitle;
    std::string                           m_localType;
    std::string                           m_localRatingKey;
    std::string                           m_localMachineId;
    std::string                           m_localGrandparentTitle;  // show (episodes)
    std::string                           m_localParentTitle;       // season (episodes)

    // Auto-join prompt: callback + last host item offered (prompt once per item).
    MatchPromptFn                         m_promptCb;
    std::string                           m_lastPromptKey;

    // Party pause state (guarded by m_mtx).
    bool                                  m_partyPauseEnabled = false;
    // Room-wide auto-host (server-side; guarded by m_mtx). Mirrored from
    // joinResult.isAutoHostEnabled and the setAutoHostEnabled broadcast.
    bool                                  m_roomAutoHostEnabled = false;
    int                                   m_partyPauseSeq = 0;
    bool                                  m_partyPaused = false;
};

} // namespace vitaplex
