/*
 * VitaPlex - Being controlled by another Plex app.
 *
 * The other half of "Play on...". That sends this app's queue to another
 * player; this lets a Plex controller — the phone app, Plexamp, Plex Web —
 * drive playback here.
 *
 * Two pieces have to exist for that. A controller finds players over GDM,
 * Plex's multicast discovery, so the app has to answer an M-SEARCH; and it
 * then talks HTTP to the player directly, so the app has to serve the
 * /player endpoints. See DESIGN_NOTES for what is and is not documented.
 *
 * Everything in `remote` is pure: a request and a state snapshot in, a
 * response and at most one command out. No sockets, no globals, no player.
 * That is what lets the protocol be tested on a host and driven with curl
 * without a Plex server anywhere near it. RemoteControlServer below owns the
 * sockets and the wiring to the real player.
 */

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vitaplex {
namespace remote {

// What this app is, as a controller sees it.
struct Identity {
    std::string name;         // shown in the controller's player list
    std::string identifier;   // stable per install; the target of every command
    std::string product = "VitaPlex";
    std::string version = "1.0";
    std::string platform = "Unknown";
    // GDM's Device-Class. Plex uses "pc", "phone", "tablet", "stb"; a console
    // or handheld reads closest to a set-top box.
    std::string deviceClass = "stb";
    int httpPort = 32500;     // where the /player endpoints are served
};

// The player, as a timeline reports it. Filled by the caller from whatever it
// actually has; a stopped player needs nothing but `state`.
struct Snapshot {
    std::string type = "music";      // music | video | photo
    std::string state = "stopped";   // playing | paused | stopped | buffering
    int64_t timeMs = 0;
    int64_t durationMs = 0;
    std::string ratingKey;
    std::string key;              // /library/metadata/...
    std::string containerKey;     // /playQueues/...
    std::string machineIdentifier;   // the *server's*, so a controller can follow
    std::string address;
    int port = 32400;
    std::string protocol = "http";
    int playQueueID = 0;
    int volume = 100;
    bool shuffle = false;
    bool repeat = false;
    bool canSeek = true;
};

// What a request asked the player to do. NONE for anything that only reads.
struct Command {
    enum class Kind {
        NONE, PLAY, PAUSE, PLAY_PAUSE, STOP, NEXT, PREVIOUS,
        SEEK_TO, STEP_FORWARD, STEP_BACK, SET_VOLUME, PLAY_MEDIA,
    };
    Kind kind = Kind::NONE;
    int64_t offsetMs = 0;
    int volume = -1;
    // PLAY_MEDIA: where to fetch what it wants played.
    std::string key, containerKey, machineIdentifier, address, protocol, token, type;
    int port = 32400;
    int playQueueID = 0;
};

struct Request {
    std::string method = "GET";
    std::string path;
    std::map<std::string, std::string> query;
    std::map<std::string, std::string> headers;   // lower-cased keys
};

struct Response {
    int status = 200;
    std::string contentType = "text/xml; charset=utf-8";
    std::string body;
    std::map<std::string, std::string> headers;
};

// Parse "GET /player/playback/play?a=1&b=2 HTTP/1.1" and its headers.
// Returns false when the bytes are not a complete request yet.
bool parseRequest(const std::string& raw, Request& out);

// Percent-decoding for query values; "+" is a space, as in a query string.
std::string urlDecode(const std::string& in);

// XML attribute-value escaping. A track called He Said "Hi" would otherwise
// close the attribute early and produce a document nothing can read.
std::string xmlEscape(const std::string& in);

class Protocol {
  public:
    explicit Protocol(Identity id) : m_id(std::move(id)) {}

    const Identity& identity() const { return m_id; }
    void setIdentity(Identity id) { m_id = std::move(id); }

    // The whole player surface. `now` is the current state, used for
    // timelines; `outCmd` receives whatever the request asked for, if
    // anything. Never throws and never blocks.
    Response handle(const Request& req, const Snapshot& now, Command* outCmd);

    // GDM payloads. The reply answers an M-SEARCH; the hello announces this
    // player unprompted so a controller already listening picks it up.
    std::string gdmReply() const;
    std::string gdmHello() const;

    std::string resourcesXml() const;
    std::string timelineXml(const Snapshot& now, int commandID) const;

    // Highest commandID seen. A controller raises it on every command so a
    // player can discard one that arrives out of order.
    int lastCommandID() const { return m_lastCommandID; }

  private:
    Identity m_id;
    int m_lastCommandID = 0;

    std::string gdmBody() const;
};

}  // namespace remote

/*
 * The live server: GDM socket, HTTP socket, and the bridge to the player.
 *
 * start() is a no-op unless the setting is on and the platform has the
 * sockets for it. Safe to call repeatedly.
 */
class RemoteControlServer {
  public:
    static RemoteControlServer& getInstance();

    // Reads the setting itself, so callers can just say "apply the setting".
    void applySetting();

    void start();
    void stop();
    bool isRunning() const;

    // Whether this build can serve at all. False on the ports whose sockets
    // cannot do this, so settings can say so rather than offer a dead switch.
    static bool isSupported();

    // The name a controller shows for this player.
    static std::string deviceName();
    // Stable per install. Two VitaPlex installs on one network must not claim
    // the same identifier or a server cannot tell them apart.
    static std::string clientIdentifier();

  private:
    RemoteControlServer() = default;
    ~RemoteControlServer();
    RemoteControlServer(const RemoteControlServer&) = delete;
    RemoteControlServer& operator=(const RemoteControlServer&) = delete;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace vitaplex
