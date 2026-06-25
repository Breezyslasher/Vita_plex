/**
 * VitaPlex - SyncLounge client (connectivity spike)
 *
 * Minimal client for a SyncLounge watch-party server
 * (https://github.com/synclounge/synclounge). SyncLounge's server is a plain
 * Socket.IO v4 application, so this speaks the Socket.IO v4 / Engine.IO v4
 * protocol over the HTTP **long-polling** transport only — no WebSocket. That
 * lets it reuse the app's existing libcurl HttpClient unchanged and keeps the
 * whole exchange to at most one HTTP request in flight at a time, which the
 * Vita's tight concurrent-connection budget needs.
 *
 * Scope of this first cut is deliberately a spike: prove the transport end to
 * end on-device before any player-sync logic is built. Concretely it
 *   1. completes the Engine.IO handshake (GET ?EIO=4&transport=polling),
 *   2. opens the default Socket.IO namespace (POST "40"),
 *   3. emits `join` with the room id + a display name,
 *   4. answers the Engine.IO transport heartbeat (server "2" ping -> client
 *      "3" pong) AND SyncLounge's application-level keepalive (server emits
 *      `slPing` with a secret -> client must echo it back via `slPong` or the
 *      server drops the socket),
 *   5. surfaces every room / sync event the server pushes.
 *
 * Everything runs on a single detached worker thread; the public surface is
 * just start()/stop() plus a log callback that the worker invokes for every
 * protocol step. The caller owns marshalling those lines onto the UI thread.
 */

#pragma once

#include <string>
#include <functional>
#include <atomic>
#include <memory>

namespace vitaplex {

class SyncLoungeClient {
public:
    // Invoked (on the worker thread) for every handshake step and inbound
    // event. The caller must marshal to the UI thread itself if it touches
    // views — see SettingsTab::onSyncLoungeTest for the brls::sync wrapper.
    using LogFn = std::function<void(const std::string&)>;

    // Invoked (on the worker thread) for every inbound Socket.IO event, with
    // the event name and the raw `["name",payload...]` array text. The
    // transport-level keepalives (slPing) are handled internally and are NOT
    // delivered here. Consumers (e.g. SyncLoungeSession) parse the payload to
    // drive higher-level state; keep handlers cheap and thread-safe.
    using EventFn = std::function<void(const std::string& name,
                                       const std::string& payload)>;

    struct Config {
        std::string server;                    // base URL, e.g. "https://server.synclounge.tv" or "http://host:8088"
        std::string socketPath = "/socket.io/"; // Socket.IO mount path (server default)
        std::string room;                      // room id / code to join
        std::string username = "VitaPlex";     // desired display name
        int syncFlexibility  = 3000;           // ms slack the server uses when nudging us into sync
        int spikeSeconds     = 120;            // safety bound: auto-stop the poll loop after this long
    };

    SyncLoungeClient() = default;
    ~SyncLoungeClient();

    SyncLoungeClient(const SyncLoungeClient&)            = delete;
    SyncLoungeClient& operator=(const SyncLoungeClient&) = delete;

    // Register a callback for parsed inbound events. Call BEFORE start(); the
    // worker captures it at launch. Optional — the spike harness leaves it
    // unset and just reads the log.
    void setEventCallback(EventFn cb) { m_eventCb = std::move(cb); }

    // Kick off handshake -> connect -> join -> poll on a background thread and
    // return immediately. `log` receives every protocol step. No-op if a run
    // is already in flight.
    void start(const Config& config, LogFn log);

    // Ask the worker to stop. The poll loop exits once the in-flight GET
    // returns (<= poll timeout). Idempotent; also called by the destructor.
    void stop();

    bool running() const;

    // Emit a SyncLounge chat message (`sendMessage`). The server rebroadcasts
    // it as `newMessage` to every other member of the room, so it's the
    // simplest way to prove outbound events land. Sent on the separate POST
    // channel (its own HttpClient) so it doesn't disturb the held-open poll;
    // returns false if we haven't connected yet. Safe to call from the UI
    // thread — the actual POST runs on a short background task.
    bool sendChatMessage(const std::string& text);

    // Emit an arbitrary Socket.IO event with a single, already-serialized JSON
    // argument, e.g. emitEvent("playerStateUpdate", "{\"state\":\"playing\",...}").
    // Used by SyncLoungeSession to broadcast our player state when we're host.
    // Returns false if we haven't connected yet.
    bool emitEvent(const std::string& name, const std::string& jsonArg);

    // Emit two Socket.IO events in one Engine.IO polling POST. This preserves
    // event order and avoids overlapping POSTs on the polling transport.
    bool emitEventPair(const std::string& firstName, const std::string& firstJsonArg,
                       const std::string& secondName, const std::string& secondJsonArg);

private:
    // Emit one raw Engine.IO/Socket.IO frame (e.g. `42["name",arg]`) on the
    // POST channel. Returns false if the session isn't connected yet.
    bool emitFrame(const std::string& frame);

    // Connection state shared between the poll worker (which discovers the
    // sid) and the send path (which needs it). Defined in the .cpp; held by
    // shared_ptr so a detached worker keeps it alive past this object.
    struct Session;
    std::shared_ptr<Session> m_session;

    // The handshake -> connect -> join -> poll loop. Static (takes the shared
    // Session by value) so the detached worker touches no instance state and
    // is safe even if the client is destroyed mid-run.
    static void runWorker(std::shared_ptr<Session> session, Config cfg,
                          LogFn log, EventFn eventCb);

    EventFn m_eventCb;  // set before start(), captured by the worker
};

} // namespace vitaplex
