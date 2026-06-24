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

    // Kick off handshake -> connect -> join -> poll on a background thread and
    // return immediately. `log` receives every protocol step. No-op if a run
    // is already in flight.
    void start(const Config& config, LogFn log);

    // Ask the worker to stop. The poll loop exits once the in-flight GET
    // returns (<= poll timeout). Idempotent; also called by the destructor.
    void stop();

    bool running() const { return m_running && m_running->load(); }

private:
    // Shared so the detached worker keeps the flag alive after this object is
    // destroyed, and so stop()/the worker observe the same atomic.
    std::shared_ptr<std::atomic<bool>> m_running;
};

} // namespace vitaplex
