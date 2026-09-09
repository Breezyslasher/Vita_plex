/*
 * The Plex player protocol, with no I/O in it.
 *
 * Split from the server so it can be exercised on a host: every rule about
 * what a controller may ask and what comes back lives here, and the socket
 * half below it only moves bytes.
 */

#include "utils/remote_control.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace vitaplex {
namespace remote {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "a=1&b=two%20words" into a map, values decoded.
void parseQuery(const std::string& qs, std::map<std::string, std::string>& out) {
    size_t pos = 0;
    while (pos < qs.size()) {
        size_t amp = qs.find('&', pos);
        if (amp == std::string::npos) amp = qs.size();
        const std::string pair = qs.substr(pos, amp - pos);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos)
            out[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
        else if (!pair.empty())
            out[urlDecode(pair)] = "";
        pos = amp + 1;
    }
}

int64_t toInt(const std::map<std::string, std::string>& q, const char* key, int64_t dflt = 0) {
    auto it = q.find(key);
    if (it == q.end() || it->second.empty()) return dflt;
    return (int64_t)std::strtoll(it->second.c_str(), nullptr, 10);
}

std::string toStr(const std::map<std::string, std::string>& q, const char* key,
                  const std::string& dflt = std::string()) {
    auto it = q.find(key);
    return it == q.end() ? dflt : it->second;
}

}  // namespace

std::string urlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '+') { out += ' '; continue; }
        if (in[i] != '%' || i + 2 >= in.size()) { out += in[i]; continue; }
        const int hi = hexVal(in[i + 1]), lo = hexVal(in[i + 2]);
        // A stray '%' is not an escape. Keeping it is better than eating the
        // two characters after it.
        if (hi < 0 || lo < 0) { out += in[i]; continue; }
        out += (char)((hi << 4) | lo);
        i += 2;
    }
    return out;
}

std::string xmlEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                // Control characters are not legal in XML 1.0 at all, and a
                // title carrying one would make the whole document unparseable
                // rather than just look odd.
                if ((unsigned char)c < 0x20 && c != '\t' && c != '\n' && c != '\r') break;
                out += c;
        }
    }
    return out;
}

bool parseRequest(const std::string& raw, Request& out) {
    const size_t headEnd = raw.find("\r\n\r\n");
    if (headEnd == std::string::npos) return false;   // still arriving

    size_t lineEnd = raw.find("\r\n");
    if (lineEnd == std::string::npos || lineEnd > headEnd) return false;
    const std::string line = raw.substr(0, lineEnd);

    const size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) return false;
    const size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;

    out.method = line.substr(0, sp1);
    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);

    const size_t q = target.find('?');
    if (q != std::string::npos) {
        parseQuery(target.substr(q + 1), out.query);
        target = target.substr(0, q);
    }
    out.path = urlDecode(target);

    size_t pos = lineEnd + 2;
    while (pos < headEnd) {
        size_t end = raw.find("\r\n", pos);
        if (end == std::string::npos || end > headEnd) end = headEnd;
        const std::string h = raw.substr(pos, end - pos);
        const size_t colon = h.find(':');
        if (colon != std::string::npos) {
            std::string val = h.substr(colon + 1);
            const size_t a = val.find_first_not_of(" \t");
            const size_t b = val.find_last_not_of(" \t\r");
            val = (a == std::string::npos) ? std::string() : val.substr(a, b - a + 1);
            out.headers[lower(h.substr(0, colon))] = val;
        }
        pos = end + 2;
    }
    return true;
}

std::string Protocol::gdmBody() const {
    // The field set a controller expects from a player, in the shape
    // python-plexapi documents reading back. Content-Type is what separates a
    // player from a server on the same protocol.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%ld", (long)std::time(nullptr));
    std::string s;
    s += "Content-Type: plex/media-player\r\n";
    s += "Resource-Identifier: " + m_id.identifier + "\r\n";
    s += "Name: " + m_id.name + "\r\n";
    s += "Port: " + std::to_string(m_id.httpPort) + "\r\n";
    s += "Product: " + m_id.product + "\r\n";
    s += "Version: " + m_id.version + "\r\n";
    s += "Protocol: plex\r\n";
    s += "Protocol-Version: 1\r\n";
    // Deliberately no "navigation": this app has no menu a controller could
    // drive, and claiming it would make those buttons appear and do nothing.
    s += "Protocol-Capabilities: timeline,playback,playqueues\r\n";
    s += "Device-Class: " + m_id.deviceClass + "\r\n";
    s += "Updated-At: " + std::string(buf) + "\r\n";
    return s;
}

std::string Protocol::gdmReply() const {
    return "HTTP/1.0 200 OK\r\n" + gdmBody();
}

std::string Protocol::gdmHello() const {
    return "HELLO * HTTP/1.0\r\n" + gdmBody();
}

std::string Protocol::resourcesXml() const {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<MediaContainer size=\"1\">\n";
    s += "  <Player title=\"" + xmlEscape(m_id.name) + "\"";
    s += " machineIdentifier=\"" + xmlEscape(m_id.identifier) + "\"";
    s += " product=\"" + xmlEscape(m_id.product) + "\"";
    s += " platform=\"" + xmlEscape(m_id.platform) + "\"";
    s += " protocol=\"plex\" protocolVersion=\"1\"";
    s += " protocolCapabilities=\"timeline,playback,playqueues\"";
    s += " deviceClass=\"" + xmlEscape(m_id.deviceClass) + "\"";
    s += " version=\"" + xmlEscape(m_id.version) + "\"/>\n</MediaContainer>\n";
    return s;
}

std::string Protocol::timelineXml(const Snapshot& now, int commandID) const {
    // Three timelines, one per media type, with only the live one filled in.
    // A controller reads all three and decides what to show; sending only the
    // active one leaves it unable to tell "not playing video" from "no answer".
    static const char* kTypes[] = {"music", "video", "photo"};

    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<MediaContainer";
    if (commandID > 0) s += " commandID=\"" + std::to_string(commandID) + "\"";
    s += " machineIdentifier=\"" + xmlEscape(m_id.identifier) + "\">\n";

    for (const char* t : kTypes) {
        const bool live = (now.type == t) && now.state != "stopped";
        s += "  <Timeline type=\"";
        s += t;
        s += "\" state=\"";
        s += live ? xmlEscape(now.state) : "stopped";
        s += "\"";
        if (live) {
            s += " time=\"" + std::to_string(now.timeMs) + "\"";
            s += " duration=\"" + std::to_string(now.durationMs) + "\"";
            if (!now.ratingKey.empty())    s += " ratingKey=\"" + xmlEscape(now.ratingKey) + "\"";
            if (!now.key.empty())          s += " key=\"" + xmlEscape(now.key) + "\"";
            if (!now.containerKey.empty()) s += " containerKey=\"" + xmlEscape(now.containerKey) + "\"";
            if (now.playQueueID > 0)       s += " playQueueID=\"" + std::to_string(now.playQueueID) + "\"";
            if (!now.machineIdentifier.empty())
                s += " machineIdentifier=\"" + xmlEscape(now.machineIdentifier) + "\"";
            if (!now.address.empty()) {
                s += " address=\"" + xmlEscape(now.address) + "\"";
                s += " port=\"" + std::to_string(now.port) + "\"";
                s += " protocol=\"" + xmlEscape(now.protocol) + "\"";
            }
            s += " volume=\"" + std::to_string(now.volume) + "\"";
            s += " shuffle=\"" + std::string(now.shuffle ? "1" : "0") + "\"";
            s += " repeat=\"" + std::string(now.repeat ? "1" : "0") + "\"";
            s += " seekRange=\"0-" + std::to_string(now.canSeek ? now.durationMs : 0) + "\"";
            // What the controller may offer. Nothing here claims a capability
            // the player does not have: no chapter or subtitle control.
            s += " controllable=\"playPause,stop,skipPrevious,skipNext,stepBack,stepForward,seekTo,volume\"";
        }
        s += "/>\n";
    }
    s += "</MediaContainer>\n";
    return s;
}

Response Protocol::handle(const Request& req, const Snapshot& now, Command* outCmd) {
    Command cmd;
    Response res;
    // Every response carries who answered, so a controller talking to several
    // players can tell them apart.
    res.headers["X-Plex-Client-Identifier"] = m_id.identifier;
    res.headers["Access-Control-Allow-Origin"] = "*";

    // A controller raises commandID on every command. One that arrives lower
    // than the last is a retransmit or a reorder, and replaying it would undo
    // something the user has since changed.
    const int commandID = (int)toInt(req.query, "commandID", 0);
    const bool stale = commandID > 0 && commandID < m_lastCommandID;
    if (commandID > m_lastCommandID) m_lastCommandID = commandID;

    // OPTIONS is the browser preflight: Plex Web talks to players from a page.
    if (req.method == "OPTIONS") {
        res.headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS";
        res.headers["Access-Control-Allow-Headers"] =
            "X-Plex-Client-Identifier, X-Plex-Target-Client-Identifier, X-Plex-Token, "
            "X-Plex-Device-Name, X-Plex-Product, X-Plex-Version, Accept";
        res.contentType = "text/plain";
        res.body.clear();
        if (outCmd) *outCmd = cmd;
        return res;
    }

    // A command addressed to a different player must not be obeyed. The header
    // is how a controller aims through a shared connection, so an absent one
    // means "whoever answers".
    auto target = req.headers.find("x-plex-target-client-identifier");
    if (target != req.headers.end() && !target->second.empty() &&
        target->second != m_id.identifier) {
        res.status = 404;
        res.body = "<MediaContainer size=\"0\"/>\n";
        if (outCmd) *outCmd = cmd;
        return res;
    }

    const std::string& p = req.path;

    if (p == "/resources" || p == "/player/resources") {
        res.body = resourcesXml();
        if (outCmd) *outCmd = cmd;
        return res;
    }

    if (p == "/player/timeline/poll" || p == "/player/timeline/subscribe" ||
        p == "/player/timeline/unsubscribe") {
        // Subscribe is answered but not honoured as a push: this player is
        // polled, and saying yes to a subscription it will not service would
        // leave a controller waiting for updates that never come.
        res.body = timelineXml(now, commandID);
        if (outCmd) *outCmd = cmd;
        return res;
    }

    auto ok = [&](Command::Kind k) {
        if (!stale) cmd.kind = k;
        res.body = timelineXml(now, commandID);
    };

    if      (p == "/player/playback/play")          ok(Command::Kind::PLAY);
    else if (p == "/player/playback/pause")         ok(Command::Kind::PAUSE);
    else if (p == "/player/playback/playPause")     ok(Command::Kind::PLAY_PAUSE);
    else if (p == "/player/playback/stop")          ok(Command::Kind::STOP);
    else if (p == "/player/playback/skipNext")      ok(Command::Kind::NEXT);
    else if (p == "/player/playback/skipPrevious")  ok(Command::Kind::PREVIOUS);
    else if (p == "/player/playback/stepForward")   ok(Command::Kind::STEP_FORWARD);
    else if (p == "/player/playback/stepBack")      ok(Command::Kind::STEP_BACK);
    else if (p == "/player/playback/seekTo") {
        ok(Command::Kind::SEEK_TO);
        cmd.offsetMs = toInt(req.query, "offset", 0);
    }
    else if (p == "/player/playback/setParameters") {
        // Only volume is claimed in `controllable`, so only volume is read.
        if (req.query.count("volume")) {
            ok(Command::Kind::SET_VOLUME);
            cmd.volume = (int)toInt(req.query, "volume", -1);
            if (cmd.volume < 0)   cmd.volume = 0;
            if (cmd.volume > 100) cmd.volume = 100;
        } else {
            res.body = timelineXml(now, commandID);
        }
    }
    else if (p == "/player/playback/playMedia") {
        ok(Command::Kind::PLAY_MEDIA);
        cmd.key               = toStr(req.query, "key");
        cmd.containerKey      = toStr(req.query, "containerKey");
        cmd.machineIdentifier = toStr(req.query, "machineIdentifier");
        cmd.address           = toStr(req.query, "address");
        cmd.protocol          = toStr(req.query, "protocol", "http");
        cmd.token             = toStr(req.query, "token");
        cmd.type              = toStr(req.query, "type", "music");
        cmd.port              = (int)toInt(req.query, "port", 32400);
        cmd.offsetMs          = toInt(req.query, "offset", 0);
        // The queue id is inside containerKey: "/playQueues/1234?window=100".
        const size_t at = cmd.containerKey.find("/playQueues/");
        if (at != std::string::npos)
            cmd.playQueueID = (int)std::strtol(cmd.containerKey.c_str() + at + 12, nullptr, 10);
        // Nothing to fetch means nothing to play; obeying it would clear what
        // is playing on the strength of a malformed request.
        if (cmd.key.empty() && cmd.playQueueID <= 0) {
            cmd.kind = Command::Kind::NONE;
            res.status = 400;
        }
    }
    else {
        res.status = 404;
        res.body = "<MediaContainer size=\"0\"/>\n";
    }

    if (outCmd) *outCmd = cmd;
    return res;
}

}  // namespace remote
}  // namespace vitaplex
