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

    // True once we've joined and the room's host is us (our socket id equals
    // the host id). Tracked from joinResult + newHost events.
    bool isHost() const;

    // Broadcast our local player state to the room (only meaningful when we're
    // host — the server relays playerStateUpdate to the other members). Called
    // each second from the player loop; throttled internally to emit on state
    // change and at most every few seconds otherwise. time/duration are ms.
    void reportLocalState(const std::string& state, double timeMs,
                          double durationMs, double playbackRate);

    // Announce a manual play/pause/seek the user performed on our player, as a
    // `mediaUpdate` with userInitiated=true. Under the room's auto-host mode
    // this also claims host (the server makes the initiator host and emits
    // newHost), so interacting with the Vita's player takes control of the
    // party. Not throttled — user actions are infrequent. time/duration ms.
    void reportUserAction(const std::string& state, double timeMs,
                          double durationMs, double playbackRate);

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

private:
    SyncLoungeSession() = default;
    SyncLoungeSession(const SyncLoungeSession&)            = delete;
    SyncLoungeSession& operator=(const SyncLoungeSession&) = delete;

    // Invoked by the client on its worker thread for each inbound event.
    void onEvent(const std::string& name, const std::string& payload);

    mutable std::mutex                m_mtx;
    std::shared_ptr<SyncLoungeClient> m_client;
    std::string                       m_server;
    std::string                       m_room;
    RemoteState                       m_remote;

    // Host tracking + outbound throttle (all guarded by m_mtx).
    std::string                           m_selfId;       // our socket id (from joinResult)
    std::string                           m_hostId;       // current room host id
    std::string                           m_lastSentState;
    std::chrono::steady_clock::time_point m_lastSentAt{};
};

} // namespace vitaplex
