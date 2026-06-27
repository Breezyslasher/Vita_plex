/**
 * VitaPlex - Plex API Client implementation
 */

#include "app/plex_client.hpp"
#include "app/application.hpp"
#include "utils/http_client.hpp"
#include "utils/http_cache.hpp"
#include "platform/platform.hpp"

#include <borealis.hpp>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <set>

namespace vitaplex {

// Redact JSON/XML "authToken": "<value>" pairs before dumping a response
// body to the log. Login and PIN-verify responses embed the long-lived
// account token in the body; any debug log that prints the body verbatim
// would otherwise persist that token in on-device logs / crash reports.
static std::string redactBodyForLog(const std::string& body) {
    static const char* const keys[] = {
        "\"authToken\"", "\"AuthToken\"", "authenticationToken=",
    };
    std::string out = body;
    for (const char* k : keys) {
        size_t pos = 0;
        while ((pos = out.find(k, pos)) != std::string::npos) {
            // Find the start of the value (first quote or '=' after the key).
            size_t cursor = pos + strlen(k);
            // Skip ':' / whitespace / '='
            while (cursor < out.size() &&
                   (out[cursor] == ':' || out[cursor] == ' ' || out[cursor] == '=' ||
                    out[cursor] == '"' || out[cursor] == '\t')) {
                cursor++;
            }
            size_t valEnd = out.find_first_of("\",&}<\n\r", cursor);
            if (valEnd == std::string::npos) valEnd = out.size();
            if (valEnd > cursor) {
                out.replace(cursor, valEnd - cursor, "[redacted]");
            }
            pos = cursor + sizeof("[redacted]") - 1;
        }
    }
    return out;
}

PlexClient& PlexClient::getInstance() {
    static PlexClient instance;
    return instance;
}

std::string PlexClient::buildApiUrl(const std::string& endpoint) {
    std::string url = m_serverUrl;

    // Remove trailing slash
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }

    url += endpoint;

    // Add token
    if (!m_authToken.empty()) {
        if (endpoint.find('?') != std::string::npos) {
            url += "&X-Plex-Token=" + m_authToken;
        } else {
            url += "?X-Plex-Token=" + m_authToken;
        }
    }

    return url;
}

MediaType PlexClient::parseMediaType(const std::string& typeStr) {
    if (typeStr == "movie") return MediaType::MOVIE;
    if (typeStr == "show") return MediaType::SHOW;
    if (typeStr == "season") return MediaType::SEASON;
    if (typeStr == "episode") return MediaType::EPISODE;
    if (typeStr == "artist") return MediaType::MUSIC_ARTIST;
    if (typeStr == "album") return MediaType::MUSIC_ALBUM;
    if (typeStr == "track") return MediaType::MUSIC_TRACK;
    if (typeStr == "clip") return MediaType::CLIP;
    if (typeStr == "photo") return MediaType::PHOTO;
    return MediaType::UNKNOWN;
}

std::string PlexClient::extractJsonValue(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return "";

    size_t colonPos = json.find(':', keyPos);
    if (colonPos == std::string::npos) return "";

    size_t valueStart = json.find_first_not_of(" \t\n\r", colonPos + 1);
    if (valueStart == std::string::npos) return "";

    if (json[valueStart] == '"') {
        // Find closing quote, skipping escaped quotes
        size_t valueEnd = valueStart + 1;
        while (valueEnd < json.length()) {
            if (json[valueEnd] == '"' && json[valueEnd - 1] != '\\') break;
            valueEnd++;
        }
        if (valueEnd >= json.length()) return "";
        return json.substr(valueStart + 1, valueEnd - valueStart - 1);
    } else if (json[valueStart] == 'n' && json.substr(valueStart, 4) == "null") {
        return "";
    } else {
        size_t valueEnd = json.find_first_of(",}]", valueStart);
        if (valueEnd == std::string::npos) return "";
        std::string value = json.substr(valueStart, valueEnd - valueStart);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\n' || value.back() == '\r')) {
            value.pop_back();
        }
        return value;
    }
}

int PlexClient::extractJsonInt(const std::string& json, const std::string& key) {
    std::string value = extractJsonValue(json, key);
    if (value.empty()) return 0;
    return atoi(value.c_str());
}

float PlexClient::extractJsonFloat(const std::string& json, const std::string& key) {
    std::string value = extractJsonValue(json, key);
    if (value.empty()) return 0.0f;
    return (float)atof(value.c_str());
}

bool PlexClient::extractJsonBool(const std::string& json, const std::string& key) {
    std::string value = extractJsonValue(json, key);
    return (value == "true" || value == "1");
}

// In-place extraction: searches within [start, end) of json without creating a substring.
std::string PlexClient::extractJsonValueRange(const std::string& json, size_t start, size_t end, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey, start);
    if (keyPos == std::string::npos || keyPos >= end) return "";

    size_t colonPos = json.find(':', keyPos);
    if (colonPos == std::string::npos || colonPos >= end) return "";

    size_t valueStart = colonPos + 1;
    while (valueStart < end && (json[valueStart] == ' ' || json[valueStart] == '\t' ||
           json[valueStart] == '\n' || json[valueStart] == '\r')) {
        valueStart++;
    }
    if (valueStart >= end) return "";

    if (json[valueStart] == '"') {
        size_t valueEnd = valueStart + 1;
        while (valueEnd < end) {
            if (json[valueEnd] == '"' && json[valueEnd - 1] != '\\') break;
            valueEnd++;
        }
        if (valueEnd >= end) return "";
        return json.substr(valueStart + 1, valueEnd - valueStart - 1);
    } else if (valueStart + 4 <= end && json[valueStart] == 'n' &&
               json[valueStart+1] == 'u' && json[valueStart+2] == 'l' && json[valueStart+3] == 'l') {
        return "";
    } else {
        size_t valueEnd = valueStart;
        while (valueEnd < end && json[valueEnd] != ',' && json[valueEnd] != '}' && json[valueEnd] != ']') {
            valueEnd++;
        }
        std::string value = json.substr(valueStart, valueEnd - valueStart);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\n' || value.back() == '\r')) {
            value.pop_back();
        }
        return value;
    }
}

int PlexClient::extractJsonIntRange(const std::string& json, size_t start, size_t end, const std::string& key) {
    std::string value = extractJsonValueRange(json, start, end, key);
    if (value.empty()) return 0;
    return atoi(value.c_str());
}

float PlexClient::extractJsonFloatRange(const std::string& json, size_t start, size_t end, const std::string& key) {
    std::string value = extractJsonValueRange(json, start, end, key);
    if (value.empty()) return 0.0f;
    return (float)atof(value.c_str());
}

std::string PlexClient::base64Encode(const std::string& input) {
    static const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    int val = 0, valb = -6;

    for (unsigned char c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            output.push_back(base64_chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }

    if (valb > -6) {
        output.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
    }

    while (output.size() % 4) {
        output.push_back('=');
    }

    return output;
}

int PlexClient::extractXmlAttr(const std::string& xml, const std::string& attr) {
    // Extract integer attribute from XML like: attr="123"
    std::string search = attr + "=\"";
    size_t pos = xml.find(search);
    if (pos == std::string::npos) return 0;
    pos += search.length();
    size_t end = xml.find("\"", pos);
    if (end == std::string::npos) return 0;
    return atoi(xml.substr(pos, end - pos).c_str());
}

std::string PlexClient::extractXmlAttrStr(const std::string& xml, const std::string& attr) {
    // Extract string attribute from XML like: attr="value"
    std::string search = attr + "=\"";
    size_t pos = xml.find(search);
    if (pos == std::string::npos) return "";
    pos += search.length();
    size_t end = xml.find("\"", pos);
    if (end == std::string::npos) return "";
    return xml.substr(pos, end - pos);
}

bool PlexClient::login(const std::string& username, const std::string& password) {
    brls::Logger::info("Attempting login for user: {}", username);

    HttpClient client;
    HttpRequest req;
    req.url = "https://plex.tv/api/v2/users/signin";
    req.method = "POST";
    req.headers["Accept"] = "application/json";
    req.headers["Content-Type"] = "application/x-www-form-urlencoded";
    {
        const auto& vc = platform::getVideoConstraints();
        req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
        req.headers["X-Plex-Product"] = PLEX_CLIENT_NAME;
        req.headers["X-Plex-Version"] = PLEX_CLIENT_VERSION;
        req.headers["X-Plex-Platform"] = vc.plexPlatform;
        req.headers["X-Plex-Device"] = vc.plexDevice;
    }

    req.body = "login=" + HttpClient::urlEncode(username) + "&password=" + HttpClient::urlEncode(password);

    HttpResponse resp = client.request(req);

    if (resp.statusCode == 201 || resp.statusCode == 200) {
        m_authToken = extractJsonValue(resp.body, "authToken");
        if (!m_authToken.empty()) {
            brls::Logger::info("Login successful");
            m_reauthTriggered = false;  // Reset reauth guard on successful login
            Application::getInstance().setAuthToken(m_authToken);
            return true;
        }
    }

    brls::Logger::error("Login failed: {}", resp.statusCode);
    return false;
}

bool PlexClient::requestPin(PinAuth& pinAuth) {
    brls::Logger::info("Requesting PIN for plex.tv/link authentication");

    HttpClient client;
    HttpRequest req;
    req.url = "https://plex.tv/api/v2/pins";
    req.method = "POST";
    req.headers["Accept"] = "application/json";
    req.headers["Content-Type"] = "application/x-www-form-urlencoded";
    {
        const auto& vc = platform::getVideoConstraints();
        req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
        req.headers["X-Plex-Product"] = PLEX_CLIENT_NAME;
        req.headers["X-Plex-Version"] = PLEX_CLIENT_VERSION;
        req.headers["X-Plex-Platform"] = vc.plexPlatform;
        req.headers["X-Plex-Device"] = vc.plexDevice;
    }

    req.body = "strong=false";

    HttpResponse resp = client.request(req);

    if (resp.statusCode == 201 || resp.statusCode == 200) {
        pinAuth.id = extractJsonInt(resp.body, "id");
        pinAuth.code = extractJsonValue(resp.body, "code");
        pinAuth.expiresIn = extractJsonInt(resp.body, "expiresIn");
        pinAuth.expired = false;

        brls::Logger::info("PIN requested: {}", pinAuth.code);
        return !pinAuth.code.empty();
    }

    brls::Logger::error("PIN request failed: {}", resp.statusCode);
    return false;
}

bool PlexClient::checkPin(PinAuth& pinAuth) {
    HttpClient client;
    HttpRequest req;
    req.url = "https://plex.tv/api/v2/pins/" + std::to_string(pinAuth.id);
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;

    HttpResponse resp = client.request(req);

    if (resp.statusCode == 200) {
        pinAuth.authToken = extractJsonValue(resp.body, "authToken");
        pinAuth.expired = extractJsonBool(resp.body, "expired");

        if (!pinAuth.authToken.empty()) {
            m_authToken = pinAuth.authToken;
            m_reauthTriggered = false;  // Reset reauth guard on successful login
            Application::getInstance().setAuthToken(m_authToken);
            brls::Logger::info("PIN authenticated successfully");
            return true;
        }
    }

    return false;
}

bool PlexClient::fetchHomeUsers(const std::string& masterToken,
                                std::vector<HomeUser>& users) {
    users.clear();
    if (masterToken.empty()) {
        brls::Logger::error("fetchHomeUsers: missing master token");
        return false;
    }

    HttpClient client;
    HttpRequest req;
    req.url = "https://plex.tv/api/v2/home/users";
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    req.headers["X-Plex-Token"] = masterToken;
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    req.timeout = 15;

    HttpResponse resp = client.request(req);
    // 401 here usually means the account doesn't have Plex Home enabled
    // (regular single-user account). Treat as "no managed users" rather
    // than an error — caller will fall back to using the master token.
    if (resp.statusCode == 401 || resp.statusCode == 403) {
        brls::Logger::info("fetchHomeUsers: account has no Plex Home (HTTP {})",
                           resp.statusCode);
        return true;
    }
    if (resp.statusCode != 200 || resp.body.empty()) {
        brls::Logger::error("fetchHomeUsers: HTTP {} body={}",
                            resp.statusCode,
                            resp.body.empty() ? "(empty)" : resp.body.substr(0, 200));
        return false;
    }

    // Response shape is {"id":N, "name":"...", ..., "users":[ {user1}, {user2}, ... ]}
    // The previous naive object walker scooped up the *outer* wrapper as a
    // single match (it contains uuid/title from users[0] when scanning the
    // whole body) and stopped, so a Home with N members reported "got 1".
    // Find the "users" array first and only walk the objects inside it.
    const std::string& body = resp.body;
    brls::Logger::debug("fetchHomeUsers: body ({} bytes), first 600: {}",
                        body.length(), body.substr(0, 600));

    size_t arrStart = std::string::npos;
    size_t arrEnd   = std::string::npos;
    size_t usersKey = body.find("\"users\"");
    if (usersKey != std::string::npos) {
        size_t open = body.find('[', usersKey);
        if (open != std::string::npos) {
            int depth = 1;
            size_t scan = open + 1;
            while (depth > 0 && scan < body.length()) {
                if (body[scan] == '[') depth++;
                else if (body[scan] == ']') depth--;
                scan++;
            }
            if (depth == 0) {
                arrStart = open;
                arrEnd   = scan;
            }
        }
    }
    // Fallback for endpoints that return a bare array.
    if (arrStart == std::string::npos) {
        arrStart = body.find('[');
        if (arrStart != std::string::npos) {
            int depth = 1;
            size_t scan = arrStart + 1;
            while (depth > 0 && scan < body.length()) {
                if (body[scan] == '[') depth++;
                else if (body[scan] == ']') depth--;
                scan++;
            }
            if (depth == 0) arrEnd = scan;
        }
    }
    if (arrStart == std::string::npos || arrEnd == std::string::npos) {
        brls::Logger::error("fetchHomeUsers: no users array found in response");
        return false;
    }

    size_t pos = arrStart + 1;
    while (pos < arrEnd) {
        size_t objStart = body.find('{', pos);
        if (objStart == std::string::npos || objStart >= arrEnd) break;

        int depth = 1;
        size_t objEnd = objStart + 1;
        while (depth > 0 && objEnd < body.length()) {
            if (body[objEnd] == '{') depth++;
            else if (body[objEnd] == '}') depth--;
            objEnd++;
        }
        if (depth != 0) break;
        std::string obj = body.substr(objStart, objEnd - objStart);
        pos = objEnd;

        HomeUser u;
        u.uuid     = extractJsonValue(obj, "uuid");
        u.id       = extractJsonValue(obj, "id");
        u.title    = extractJsonValue(obj, "title");
        u.username = extractJsonValue(obj, "username");
        u.thumb    = extractJsonValue(obj, "thumb");
        // "protected" is the only field that means a PIN is required at
        // /switch time. "restricted" just means content-restrictions
        // (kid accounts) — those users may or may not have a PIN, so
        // OR'ing them in here was prompting unprotected restricted users
        // for a PIN that doesn't exist.
        u.hasPin   = extractJsonBool(obj, "protected");
        u.admin    = extractJsonBool(obj, "admin") ||
                     extractJsonBool(obj, "homeAdmin");

        if (!u.uuid.empty() && !u.title.empty()) {
            users.push_back(std::move(u));
        }
    }

    brls::Logger::info("fetchHomeUsers: got {} users", users.size());
    return true;
}

bool PlexClient::switchHomeUser(const std::string& masterToken,
                                const std::string& userUuid,
                                const std::string& pin,
                                std::string& outToken) {
    outToken.clear();
    if (masterToken.empty() || userUuid.empty()) return false;

    HttpClient client;
    HttpRequest req;
    req.url = "https://plex.tv/api/v2/home/users/" + userUuid + "/switch";
    req.method = "POST";
    req.headers["Accept"] = "application/json";
    // plex.tv's nginx rejects empty-body POSTs that don't declare a
    // content type — that's the 400 Bad Request the original version
    // hit. Encode the PIN (empty when the user isn't protected) into a
    // form body so the request always has a real Content-Type + body.
    req.headers["Content-Type"] = "application/x-www-form-urlencoded";
    req.headers["X-Plex-Token"] = masterToken;
    // The full X-Plex-* identification block is what every other
    // plex.tv POST in this client sends; without it the API often
    // 401s or 400s on /home/users/{uuid}/switch.
    {
        const auto& vc = platform::getVideoConstraints();
        req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
        req.headers["X-Plex-Product"] = PLEX_CLIENT_NAME;
        req.headers["X-Plex-Version"] = PLEX_CLIENT_VERSION;
        req.headers["X-Plex-Platform"] = vc.plexPlatform;
        req.headers["X-Plex-Device"] = vc.plexDevice;
    }
    req.body = pin.empty() ? std::string("pin=")
                            : ("pin=" + HttpClient::urlEncode(pin));
    req.timeout = 15;

    HttpResponse resp = client.request(req);
    if (resp.statusCode != 200 && resp.statusCode != 201) {
        brls::Logger::error("switchHomeUser: HTTP {} body={}",
                            resp.statusCode,
                            resp.body.empty() ? "(empty)" : resp.body.substr(0, 200));
        return false;
    }

    outToken = extractJsonValue(resp.body, "authToken");
    if (outToken.empty()) {
        brls::Logger::error("switchHomeUser: response missing authToken (body={})",
                            resp.body.substr(0, 200));
        return false;
    }
    brls::Logger::info("switchHomeUser: switched to user {}", userUuid);
    return true;
}

bool PlexClient::refreshToken() {
    // Legacy Plex tokens don't expire, but they can be revoked.
    // Validate the current token against plex.tv; if invalid, trigger reauth.
    if (validateToken()) {
        brls::Logger::info("Token is still valid");
        return true;
    }
    brls::Logger::error("Token validation failed - token may have been revoked");
    return false;
}

bool PlexClient::validateToken() {
    if (m_authToken.empty()) return false;

    // Check token validity by hitting plex.tv/api/v2/user
    HttpClient client;
    HttpRequest req;
    req.url = "https://plex.tv/api/v2/user";
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    req.headers["X-Plex-Token"] = m_authToken;
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    req.timeout = 10;

    HttpResponse resp = client.request(req);

    if (resp.statusCode == 200) {
        return true;
    }

    brls::Logger::error("Token validation returned status {}", resp.statusCode);
    return false;
}

void PlexClient::handleUnauthorized() {
    if (m_reauthTriggered) {
        // Already handling reauth, don't recurse
        return;
    }
    m_reauthTriggered = true;

    brls::Logger::error("Authentication failed (401) - clearing session and redirecting to login");

    // Clear auth state
    logout();

    // Redirect to login on the UI thread
    brls::sync([]() {
        Application::getInstance().pushLoginActivity();
    });
}

bool PlexClient::fetchServers(std::vector<PlexServer>& servers) {
    brls::Logger::info("Fetching user's servers from plex.tv");

    if (m_authToken.empty()) {
        brls::Logger::error("No auth token - please login first");
        return false;
    }

    HttpClient client;
    HttpRequest req;
    req.url = "https://plex.tv/api/v2/resources?includeHttps=1&includeRelay=1&includeIPv6=0";
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    req.headers["X-Plex-Token"] = m_authToken;
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;

    HttpResponse resp = client.request(req);

    brls::Logger::debug("Servers response: {} - {} bytes", resp.statusCode, resp.body.length());

    if (resp.statusCode != 200) {
        brls::Logger::error("Failed to fetch servers: {}", resp.statusCode);
        if (isAuthError(resp.statusCode)) handleUnauthorized();
        return false;
    }

    servers.clear();

    // Parse server resources - look for devices that provide "server"
    // Response is an array of resources
    size_t pos = 0;
    while ((pos = resp.body.find("\"name\"", pos)) != std::string::npos) {
        // Find the start of this object (go back to find opening brace)
        size_t objStart = resp.body.rfind('{', pos);
        if (objStart == std::string::npos) {
            pos++;
            continue;
        }

        // Find end of object
        int braceCount = 1;
        size_t objEnd = objStart + 1;
        while (braceCount > 0 && objEnd < resp.body.length()) {
            if (resp.body[objEnd] == '{') braceCount++;
            else if (resp.body[objEnd] == '}') braceCount--;
            objEnd++;
        }

        std::string obj = resp.body.substr(objStart, objEnd - objStart);

        // Check if this resource provides "server"
        if (obj.find("\"provides\"") != std::string::npos &&
            obj.find("\"server\"") != std::string::npos) {

            PlexServer server;
            server.name             = extractJsonValue(obj, "name");
            server.machineIdentifier = extractJsonValue(obj, "clientIdentifier");
            // owned + version + sourceTitle drive the new server-picker
            // card: gold tint + "OWNED" chip for owned, "Shared by …"
            // line for friends, and a version readout next to the
            // address. All three are scalar fields on the resource
            // object, so extractJsonValue / extractJsonBool reach them.
            server.owned       = extractJsonBool(obj, "owned");
            server.version     = extractJsonValue(obj, "productVersion");
            server.sourceTitle = extractJsonValue(obj, "sourceTitle");

            // Parse connections array - store ALL connections for fallback
            size_t connPos = obj.find("\"connections\"");
            if (connPos != std::string::npos) {
                // Find the connections array start
                size_t arrStart = obj.find('[', connPos);
                if (arrStart != std::string::npos) {
                    // Parse each connection object in the array
                    size_t connObjPos = arrStart;
                    while ((connObjPos = obj.find('{', connObjPos)) != std::string::npos) {
                        // Find end of this connection object
                        int connBraceCount = 1;
                        size_t connObjEnd = connObjPos + 1;
                        while (connBraceCount > 0 && connObjEnd < obj.length()) {
                            if (obj[connObjEnd] == '{') connBraceCount++;
                            else if (obj[connObjEnd] == '}') connBraceCount--;
                            connObjEnd++;
                        }

                        std::string connObj = obj.substr(connObjPos, connObjEnd - connObjPos);
                        std::string uri = extractJsonValue(connObj, "uri");
                        bool isLocal = (connObj.find("\"local\":true") != std::string::npos ||
                                       connObj.find("\"local\": true") != std::string::npos);
                        bool isRelay = (connObj.find("\"relay\":true") != std::string::npos ||
                                       connObj.find("\"relay\": true") != std::string::npos);

                        if (!uri.empty()) {
                            ServerConnection conn;
                            conn.uri = uri;
                            conn.local = isLocal;
                            conn.relay = isRelay;
                            server.connections.push_back(conn);
                            brls::Logger::debug("Found connection: {} (local={}, relay={})",
                                               uri, isLocal, isRelay);
                        }

                        connObjPos = connObjEnd;
                    }
                }
            }

            // Sort connections: local first, then non-relay remote, then relay
            std::sort(server.connections.begin(), server.connections.end(),
                     [](const ServerConnection& a, const ServerConnection& b) {
                         // Local connections first
                         if (a.local != b.local) return a.local;
                         // Then non-relay connections
                         if (a.relay != b.relay) return !a.relay;
                         return false;
                     });

            // Set primary address to first (best) connection
            if (!server.connections.empty()) {
                server.address = server.connections[0].uri;
            }

            if (!server.name.empty() && !server.address.empty()) {
                brls::Logger::info("Found server: {} with {} connections (primary: {})",
                                   server.name, server.connections.size(), server.address);
                servers.push_back(server);
            }
        }

        pos = objEnd;
    }

    brls::Logger::info("Found {} servers", servers.size());
    return !servers.empty();
}

bool PlexClient::connectToServer(const std::string& url) {
    // Use connection timeout from settings (default 3 minutes for slow connections)
    int timeout = Application::getInstance().getSettings().connectionTimeout;
    if (timeout <= 0) timeout = 180;
    return connectToServer(url, timeout);
}

bool PlexClient::connectToServer(const std::string& url, int timeoutSeconds) {
    brls::Logger::info("Connecting to server: {} (timeout: {}s)", url, timeoutSeconds);

    // Normalize URL - ensure http/https is lowercase
    m_serverUrl = url;
    if (m_serverUrl.length() > 7) {
        size_t colonPos = m_serverUrl.find("://");
        if (colonPos != std::string::npos && colonPos < 6) {
            for (size_t i = 0; i < colonPos; i++) {
                m_serverUrl[i] = tolower(m_serverUrl[i]);
            }
        }
    }
    Application::getInstance().setServerUrl(m_serverUrl);  // Use normalized URL

    HttpClient client;
    HttpRequest req;
    req.url = buildApiUrl("/");
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    req.timeout = timeoutSeconds;

    brls::Logger::debug("Connection timeout: {} seconds", timeoutSeconds);

    HttpResponse resp = client.request(req);

    if (resp.statusCode == 200) {
        m_currentServer.name = extractJsonValue(resp.body, "friendlyName");
        m_currentServer.machineIdentifier = extractJsonValue(resp.body, "machineIdentifier");
        m_currentServer.address = url;

        brls::Logger::info("Connected to: {}", m_currentServer.name);

        // Check if Live TV is available on this server
        checkLiveTVAvailability();

        return true;
    }

    brls::Logger::error("Connection failed: {}", resp.statusCode);
    return false;
}

void PlexClient::logout() {
    m_authToken.clear();
    m_serverUrl.clear();
    m_reauthTriggered = false;
    Application::getInstance().setAuthToken("");
    Application::getInstance().setServerUrl("");
}

bool PlexClient::fetchLibrarySections(std::vector<LibrarySection>& sections) {
    brls::Logger::debug("fetchLibrarySections: serverUrl={}, hasToken={}",
                        m_serverUrl, !m_authToken.empty());

    std::string url = buildApiUrl("/library/sections");
    brls::Logger::debug("Fetching: {}", url);

    // Cache check — library sections rarely change. Skipping the
    // network entirely on a hit cuts a 100-500ms round-trip every time
    // a tab that needs the section list is opened.
    const int ttlSec = Application::getInstance().getSettings().cacheLifetimeMinutes * 60;
    std::string body;
    bool fromCache = HttpCache::get(url, ttlSec, body);

    if (!fromCache) {
        HttpClient client;
        // Request JSON format (Plex returns XML by default)
        HttpRequest req;
        req.url = url;
        req.method = "GET";
        req.headers["Accept"] = "application/json";
        HttpResponse resp = client.request(req);
        brls::Logger::debug("Response: {} - {} bytes", resp.statusCode, resp.body.length());

        if (resp.statusCode != 200) {
            brls::Logger::error("Failed to fetch sections: {}", resp.statusCode);
            if (isAuthError(resp.statusCode)) {
                handleUnauthorized();
            }
            if (!resp.body.empty()) {
                brls::Logger::debug("Body: {}", redactBodyForLog(resp.body.substr(0, 500)));
            }
            return false;
        }
        body = resp.body;
        HttpCache::put(url, body, ttlSec);
    }

    // Log first part of response for debugging
    brls::Logger::debug("Response body: {}", redactBodyForLog(body.substr(0, std::min((size_t)500, body.length()))));

    sections.clear();

    // Find all Directory entries - in JSON arrays
    size_t pos = 0;
    while ((pos = body.find("\"key\"", pos)) != std::string::npos) {
        // Go back to find the start of this object
        size_t objStart = body.rfind('{', pos);
        if (objStart == std::string::npos) {
            pos++;
            continue;
        }

        // Check if we've already processed this object
        std::string beforeObj = body.substr(objStart, pos - objStart);
        if (beforeObj.find("\"key\"") != std::string::npos) {
            pos++;
            continue;
        }

        // Find end of object
        int braceCount = 1;
        size_t objEnd = objStart + 1;
        while (braceCount > 0 && objEnd < body.length()) {
            if (body[objEnd] == '{') braceCount++;
            else if (body[objEnd] == '}') braceCount--;
            objEnd++;
        }

        std::string obj = body.substr(objStart, objEnd - objStart);

        // Check if this looks like a library section (has title and type)
        if (obj.find("\"title\"") != std::string::npos &&
            obj.find("\"type\"") != std::string::npos) {

            LibrarySection section;
            section.key = extractJsonValue(obj, "key");
            section.title = extractJsonValue(obj, "title");
            section.type = extractJsonValue(obj, "type");
            section.art = extractJsonValue(obj, "art");
            section.thumb = extractJsonValue(obj, "thumb");

            if (!section.key.empty() && !section.title.empty()) {
                brls::Logger::debug("Found section: {} ({})", section.title, section.type);
                sections.push_back(section);
            }
        }

        pos = objEnd;
    }

    brls::Logger::info("Found {} library sections", sections.size());
    return !sections.empty();
}

bool PlexClient::fetchLibraryContent(const std::string& sectionKey, std::vector<MediaItem>& items, int metadataType, int limit, int offset, int* totalCount, const std::string& extraParams) {
    brls::Logger::debug("fetchLibraryContent: section={} type={} limit={} offset={} extra={}", sectionKey, metadataType, limit, offset, extraParams);

    HttpClient client;
    std::string url = buildApiUrl("/library/sections/" + sectionKey + "/all");
    if (metadataType > 0) {
        url += "&type=" + std::to_string(metadataType);
    }
    // Caller-supplied sort / filter fragment (each token already '&'-prefixed).
    if (!extraParams.empty()) {
        url += extraParams;
    }

    // Server-side pagination: only fetch what we need to reduce response size.
    // Default page size is platform-specific (60 on Vita, 500 on desktop/PS4,
    // 200-300 on Switch/Android) so beefier platforms can load most libraries
    // in a single page instead of chunking through 60-item calls.
    int defaultPageSize = platform::getImageConstraints().libraryPageSize;
    if (defaultPageSize <= 0) defaultPageSize = 60;
    int fetchLimit = (limit > 0) ? limit : defaultPageSize;
    url += "&X-Plex-Container-Start=" + std::to_string(offset) +
           "&X-Plex-Container-Size=" + std::to_string(fetchLimit);

    // Request JSON format
    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    brls::Logger::debug("Response: {} - {} bytes", resp.statusCode, resp.body.length());

    if (resp.statusCode != 200) {
        brls::Logger::error("Failed to fetch content: {}", resp.statusCode);
        if (isAuthError(resp.statusCode)) handleUnauthorized();
        return false;
    }

    items.clear();
    items.reserve(fetchLimit);

    // Extract total count from Plex container metadata (for pagination)
    if (totalCount) {
        *totalCount = extractJsonInt(resp.body, "totalSize");
        if (*totalCount <= 0) {
            // Fallback: some Plex versions use "size" for container total
            *totalCount = extractJsonInt(resp.body, "size");
        }
    }

    // Parse items in-place without creating per-object substrings.
    // Uses extractJsonValueRange to search within [objStart, objEnd) of resp.body.
    size_t pos = 0;
    while ((pos = resp.body.find("\"ratingKey\"", pos)) != std::string::npos) {
        size_t objStart = resp.body.rfind('{', pos);
        if (objStart == std::string::npos) {
            pos++;
            continue;
        }

        int braceCount = 1;
        size_t objEnd = objStart + 1;
        while (braceCount > 0 && objEnd < resp.body.length()) {
            if (resp.body[objEnd] == '{') braceCount++;
            else if (resp.body[objEnd] == '}') braceCount--;
            objEnd++;
        }

        // Parse in-place - no substr copy of the object
        MediaItem item;
        item.ratingKey = extractJsonValueRange(resp.body, objStart, objEnd, "ratingKey");
        item.key = extractJsonValueRange(resp.body, objStart, objEnd, "key");
        item.title = extractJsonValueRange(resp.body, objStart, objEnd, "title");
        item.thumb = extractJsonValueRange(resp.body, objStart, objEnd, "thumb");
        item.type = extractJsonValueRange(resp.body, objStart, objEnd, "type");
        item.mediaType = parseMediaType(item.type);
        item.year = extractJsonIntRange(resp.body, objStart, objEnd, "year");
        item.duration = extractJsonIntRange(resp.body, objStart, objEnd, "duration");
        item.viewOffset = extractJsonIntRange(resp.body, objStart, objEnd, "viewOffset");
        item.rating = extractJsonFloatRange(resp.body, objStart, objEnd, "rating");
        item.audienceRating = extractJsonFloatRange(resp.body, objStart, objEnd, "audienceRating");
        item.contentRating = extractJsonValueRange(resp.body, objStart, objEnd, "contentRating");
        item.subtype = extractJsonValueRange(resp.body, objStart, objEnd, "subtype");
        // Skip summary and art for grid display - saves ~200-500 bytes per item

        if (!item.ratingKey.empty() && !item.title.empty()) {
            items.push_back(std::move(item));
        }

        pos = objEnd;
    }

    brls::Logger::info("Found {} items in library section {}", items.size(), sectionKey);
    return !items.empty() || resp.statusCode == 200;
}

bool PlexClient::fetchSectionRecentlyAdded(const std::string& sectionKey, std::vector<MediaItem>& items) {
    brls::Logger::debug("fetchSectionRecentlyAdded: section={}", sectionKey);

    HttpClient client;
    // Correct endpoint: /library/sections/{key}/recentlyAdded (no /all)
    std::string url = buildApiUrl("/library/sections/" + sectionKey + "/recentlyAdded");

    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    brls::Logger::debug("RecentlyAdded response: {} - {} bytes", resp.statusCode, resp.body.length());

    if (resp.statusCode != 200) {
        brls::Logger::error("Failed to fetch recently added: {}", resp.statusCode);
        if (isAuthError(resp.statusCode)) handleUnauthorized();
        return false;
    }

    items.clear();

    // Parse items by looking for objects with "ratingKey"
    size_t pos = 0;
    while ((pos = resp.body.find("\"ratingKey\"", pos)) != std::string::npos) {
        size_t objStart = resp.body.rfind('{', pos);
        if (objStart == std::string::npos) {
            pos++;
            continue;
        }

        int braceCount = 1;
        size_t objEnd = objStart + 1;
        while (braceCount > 0 && objEnd < resp.body.length()) {
            if (resp.body[objEnd] == '{') braceCount++;
            else if (resp.body[objEnd] == '}') braceCount--;
            objEnd++;
        }

        std::string obj = resp.body.substr(objStart, objEnd - objStart);

        MediaItem item;
        item.ratingKey = extractJsonValue(obj, "ratingKey");
        item.key = extractJsonValue(obj, "key");
        item.title = extractJsonValue(obj, "title");
        item.summary = extractJsonValue(obj, "summary");
        item.thumb = extractJsonValue(obj, "thumb");
        item.art = extractJsonValue(obj, "art");
        item.type = extractJsonValue(obj, "type");
        item.mediaType = parseMediaType(item.type);
        item.year = extractJsonInt(obj, "year");
        item.duration = extractJsonInt(obj, "duration");
        item.viewOffset = extractJsonInt(obj, "viewOffset");

        // Parse episode-specific fields for show cover
        item.grandparentTitle = extractJsonValue(obj, "grandparentTitle");
        item.grandparentThumb = extractJsonValue(obj, "grandparentThumb");
        item.parentTitle = extractJsonValue(obj, "parentTitle");
        item.parentThumb = extractJsonValue(obj, "parentThumb");
        item.parentIndex = extractJsonInt(obj, "parentIndex");
        item.index = extractJsonInt(obj, "index");

        if (!item.ratingKey.empty() && !item.title.empty()) {
            items.push_back(item);
        }

        pos = objEnd;
    }

    brls::Logger::info("Found {} recently added items in section {}", items.size(), sectionKey);
    return true;
}

bool PlexClient::fetchChildren(const std::string& ratingKey, std::vector<MediaItem>& items) {
    brls::Logger::debug("fetchChildren: ratingKey={}", ratingKey);

    HttpClient client;
    std::string url = buildApiUrl("/library/metadata/" + ratingKey + "/children");

    // Request JSON format
    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    brls::Logger::debug("Children response: {} - {} bytes", resp.statusCode, resp.body.length());

    if (resp.statusCode != 200) {
        brls::Logger::error("Failed to fetch children: {}", resp.statusCode);
        if (isAuthError(resp.statusCode)) handleUnauthorized();
        return false;
    }

    items.clear();

    // Parse media items by looking for objects with ratingKey
    size_t pos = 0;
    while ((pos = resp.body.find("\"ratingKey\"", pos)) != std::string::npos) {
        // Go back to find start of this object
        size_t objStart = resp.body.rfind('{', pos);
        if (objStart == std::string::npos) {
            pos++;
            continue;
        }

        // Find end of object
        int braceCount = 1;
        size_t objEnd = objStart + 1;
        while (braceCount > 0 && objEnd < resp.body.length()) {
            if (resp.body[objEnd] == '{') braceCount++;
            else if (resp.body[objEnd] == '}') braceCount--;
            objEnd++;
        }

        std::string obj = resp.body.substr(objStart, objEnd - objStart);

        MediaItem item;
        item.ratingKey = extractJsonValue(obj, "ratingKey");
        item.key = extractJsonValue(obj, "key");
        item.title = extractJsonValue(obj, "title");
        item.summary = extractJsonValue(obj, "summary");
        item.thumb = extractJsonValue(obj, "thumb");
        item.art = extractJsonValue(obj, "art");
        item.type = extractJsonValue(obj, "type");
        item.mediaType = parseMediaType(item.type);
        item.year = extractJsonInt(obj, "year");
        item.duration = extractJsonInt(obj, "duration");
        item.viewOffset = extractJsonInt(obj, "viewOffset");
        item.rating = extractJsonFloat(obj, "rating");
        item.audienceRating = extractJsonFloat(obj, "audienceRating");
        item.contentRating = extractJsonValue(obj, "contentRating");
        item.index = extractJsonInt(obj, "index");
        item.parentIndex = extractJsonInt(obj, "parentIndex");
        item.grandparentTitle = extractJsonValue(obj, "grandparentTitle");
        item.parentTitle = extractJsonValue(obj, "parentTitle");
        item.leafCount = extractJsonInt(obj, "leafCount");
        item.viewedLeafCount = extractJsonInt(obj, "viewedLeafCount");

        // Extract subtype for albums (album, single, ep, compilation, soundtrack, live)
        item.subtype = extractJsonValue(obj, "subtype");
        if (item.subtype.empty()) {
            // Try alternative field names
            item.subtype = extractJsonValue(obj, "albumType");
        }

        if (!item.ratingKey.empty() && !item.title.empty()) {
            items.push_back(item);
        }

        pos = objEnd;
    }

    brls::Logger::info("Found {} children", items.size());
    return true;
}

bool PlexClient::fetchMediaDetails(const std::string& ratingKey, MediaItem& item) {
    HttpClient client;
    std::string url = buildApiUrl("/library/metadata/" + ratingKey);

    // Request JSON format
    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    if (resp.statusCode != 200) {
        if (isAuthError(resp.statusCode)) handleUnauthorized();
        return false;
    }

    item.ratingKey = extractJsonValue(resp.body, "ratingKey");
    item.title = extractJsonValue(resp.body, "title");
    item.summary = extractJsonValue(resp.body, "summary");
    item.thumb = extractJsonValue(resp.body, "thumb");
    item.art = extractJsonValue(resp.body, "art");
    item.type = extractJsonValue(resp.body, "type");
    item.mediaType = parseMediaType(item.type);
    item.year = extractJsonInt(resp.body, "year");
    item.duration = extractJsonInt(resp.body, "duration");
    item.viewOffset = extractJsonInt(resp.body, "viewOffset");
    item.rating = extractJsonFloat(resp.body, "rating");
    item.contentRating = extractJsonValue(resp.body, "contentRating");
    item.studio = extractJsonValue(resp.body, "studio");
    // leafCount = track count for artists, episode count for shows/seasons.
    item.leafCount = extractJsonInt(resp.body, "leafCount");

    // Genres: Plex returns "Genre":[{"tag":"Anime"},{"tag":"J-Pop"}]. Pull the
    // tag of each entry (used by the artist detail meta row). Bounded scan over
    // the Genre array only, so it never picks up "tag" fields from other arrays.
    {
        size_t gPos = resp.body.find("\"Genre\":");
        if (gPos != std::string::npos) {
            size_t arrStart = resp.body.find('[', gPos);
            size_t arrEnd   = resp.body.find(']', arrStart == std::string::npos ? gPos : arrStart);
            if (arrStart != std::string::npos && arrEnd != std::string::npos && arrEnd > arrStart) {
                std::string arr = resp.body.substr(arrStart, arrEnd - arrStart + 1);
                size_t oPos = 0;
                while ((oPos = arr.find('{', oPos)) != std::string::npos) {
                    size_t oEnd = arr.find('}', oPos);
                    if (oEnd == std::string::npos) break;
                    std::string tag = extractJsonValue(arr.substr(oPos, oEnd - oPos + 1), "tag");
                    if (!tag.empty()) item.genres.push_back(tag);
                    oPos = oEnd + 1;
                }
            }
        }
    }

    // Episode info
    item.grandparentTitle = extractJsonValue(resp.body, "grandparentTitle");
    item.parentTitle = extractJsonValue(resp.body, "parentTitle");
    item.parentRatingKey = extractJsonValue(resp.body, "parentRatingKey");
    item.grandparentRatingKey = extractJsonValue(resp.body, "grandparentRatingKey");
    item.index = extractJsonInt(resp.body, "index");
    item.parentIndex = extractJsonInt(resp.body, "parentIndex");

    // Extract part path for downloads from Media[0].Part[0].key
    // Look for "Part":[{"key":"/library/parts/...
    size_t partPos = resp.body.find("\"Part\":");
    if (partPos != std::string::npos) {
        size_t partKeyPos = resp.body.find("\"key\":", partPos);
        if (partKeyPos != std::string::npos && partKeyPos < partPos + 500) {
            // Extract the part key value
            size_t start = resp.body.find('"', partKeyPos + 6);
            if (start != std::string::npos) {
                size_t end = resp.body.find('"', start + 1);
                if (end != std::string::npos) {
                    item.partPath = resp.body.substr(start + 1, end - start - 1);
                    brls::Logger::debug("fetchMediaDetails: partPath={}", item.partPath);
                }
            }
        }

        // Also try to get the file size
        size_t sizePos = resp.body.find("\"size\":", partPos);
        if (sizePos != std::string::npos && sizePos < partPos + 500) {
            size_t numStart = sizePos + 7;
            while (numStart < resp.body.length() && !isdigit(resp.body[numStart])) numStart++;
            size_t numEnd = numStart;
            while (numEnd < resp.body.length() && isdigit(resp.body[numEnd])) numEnd++;
            if (numEnd > numStart) {
                item.partSize = std::stoll(resp.body.substr(numStart, numEnd - numStart));
                brls::Logger::debug("fetchMediaDetails: partSize={}", item.partSize);
            }
        }
    }

    // Parse markers (intro/credits) from "Marker" array in response
    // Plex returns: "Marker":[{"type":"intro","startTimeOffset":0,"endTimeOffset":30000}, ...]
    size_t markerArrayPos = resp.body.find("\"Marker\":");
    if (markerArrayPos != std::string::npos) {
        size_t arrStart = resp.body.find('[', markerArrayPos);
        if (arrStart != std::string::npos) {
            size_t arrEnd = resp.body.find(']', arrStart);
            if (arrEnd != std::string::npos) {
                std::string markerArr = resp.body.substr(arrStart, arrEnd - arrStart + 1);
                // Parse each marker object in the array
                size_t mPos = 0;
                while ((mPos = markerArr.find('{', mPos)) != std::string::npos) {
                    size_t mEnd = markerArr.find('}', mPos);
                    if (mEnd == std::string::npos) break;
                    std::string markerObj = markerArr.substr(mPos, mEnd - mPos + 1);

                    MediaItem::Marker marker;
                    marker.type = extractJsonValue(markerObj, "type");
                    marker.startTimeMs = extractJsonInt(markerObj, "startTimeOffset");
                    marker.endTimeMs = extractJsonInt(markerObj, "endTimeOffset");

                    if (!marker.type.empty() && marker.endTimeMs > marker.startTimeMs) {
                        item.markers.push_back(marker);
                        brls::Logger::debug("fetchMediaDetails: marker type={} start={}ms end={}ms",
                                           marker.type, marker.startTimeMs, marker.endTimeMs);
                    }
                    mPos = mEnd + 1;
                }
            }
        }
    }

    // Library section id, so the detail view can browse a person's other
    // titles within the same section ("more by this person").
    {
        int sid = extractJsonInt(resp.body, "librarySectionID");
        if (sid > 0) item.librarySectionKey = std::to_string(sid);
    }

    // Parse cast & crew. The full metadata response carries "Role" (actors,
    // each with a character "role" and a headshot "thumb"), plus "Director"
    // and "Writer" tag arrays. Flatten them into item.cast for the detail view.
    {
        auto parsePeople = [&](const char* arrayKey, const std::string& jobLabel,
                               const char* filterField) {
            std::string needle = std::string("\"") + arrayKey + "\":";
            size_t keyPos = resp.body.find(needle);
            if (keyPos == std::string::npos) return;
            size_t arrStart = resp.body.find('[', keyPos);
            if (arrStart == std::string::npos) return;
            // Walk to the matching close bracket for this array.
            int depth = 1;
            size_t i = arrStart + 1;
            while (i < resp.body.size() && depth > 0) {
                char c = resp.body[i];
                if (c == '[') depth++;
                else if (c == ']') depth--;
                i++;
            }
            std::string arr = resp.body.substr(arrStart, i - arrStart);
            size_t p = 0;
            while ((p = arr.find('{', p)) != std::string::npos) {
                int d = 1;
                size_t e = p + 1;
                while (e < arr.size() && d > 0) {
                    if (arr[e] == '{') d++;
                    else if (arr[e] == '}') d--;
                    e++;
                }
                std::string obj = arr.substr(p, e - p);
                MediaItem::Person person;
                person.tag = extractJsonValue(obj, "tag");
                person.role = jobLabel.empty() ? extractJsonValue(obj, "role") : jobLabel;
                person.thumb = extractJsonValue(obj, "thumb");
                // Prefer the server-provided filter ("actor=12345"); otherwise
                // build it from the numeric tag id.
                person.filter = extractJsonValue(obj, "filter");
                if (person.filter.empty()) {
                    int tagId = extractJsonInt(obj, "id");
                    if (tagId > 0)
                        person.filter = std::string(filterField) + "=" + std::to_string(tagId);
                }
                if (!person.tag.empty()) item.cast.push_back(person);
                p = e;
            }
        };
        parsePeople("Role", "", "actor");          // actors: role = character
        parsePeople("Director", "Director", "director");
        parsePeople("Writer", "Writer", "writer");
        if (item.cast.size() > 30) item.cast.resize(30);
    }

    return true;
}

bool PlexClient::fetchByPersonFilter(const std::string& sectionKey, const std::string& filter,
                                     std::vector<MediaItem>& items) {
    items.clear();
    if (sectionKey.empty() || filter.empty()) return false;

    HttpClient client;
    // e.g. /library/sections/2/all?actor=12345 — every title in this section
    // the person is credited on.
    std::string url = buildApiUrl("/library/sections/" + sectionKey + "/all?" + filter);

    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    if (resp.statusCode != 200) {
        brls::Logger::warning("fetchByPersonFilter: status {}", resp.statusCode);
        if (isAuthError(resp.statusCode)) handleUnauthorized();
        return false;
    }

    size_t pos = 0;
    while ((pos = resp.body.find("\"ratingKey\"", pos)) != std::string::npos) {
        size_t objStart = resp.body.rfind('{', pos);
        if (objStart == std::string::npos) { pos++; continue; }

        int braceCount = 1;
        size_t objEnd = objStart + 1;
        while (braceCount > 0 && objEnd < resp.body.length()) {
            if (resp.body[objEnd] == '{') braceCount++;
            else if (resp.body[objEnd] == '}') braceCount--;
            objEnd++;
        }

        std::string obj = resp.body.substr(objStart, objEnd - objStart);

        MediaItem item;
        item.ratingKey = extractJsonValue(obj, "ratingKey");
        item.key = extractJsonValue(obj, "key");
        item.title = extractJsonValue(obj, "title");
        item.thumb = extractJsonValue(obj, "thumb");
        item.art = extractJsonValue(obj, "art");
        item.type = extractJsonValue(obj, "type");
        item.mediaType = parseMediaType(item.type);
        item.year = extractJsonInt(obj, "year");
        item.duration = extractJsonInt(obj, "duration");
        item.viewOffset = extractJsonInt(obj, "viewOffset");
        item.rating = extractJsonFloat(obj, "rating");
        item.audienceRating = extractJsonFloat(obj, "audienceRating");
        // Per-title role for the cast member, when the server includes it in the
        // filtered listing (often absent for actors; present for some credits).
        item.character = extractJsonValue(obj, "role");

        bool playable = (item.mediaType == MediaType::MOVIE ||
                         item.mediaType == MediaType::SHOW);
        if (!item.ratingKey.empty() && !item.title.empty() && playable) {
            bool dup = false;
            for (const auto& e : items) {
                if (e.ratingKey == item.ratingKey) { dup = true; break; }
            }
            if (!dup) items.push_back(item);
        }

        pos = objEnd;
    }

    brls::Logger::info("fetchByPersonFilter: {} items for {}", items.size(), filter);
    return true;
}

bool PlexClient::fetchRelated(const std::string& ratingKey, std::vector<MediaItem>& items) {
    brls::Logger::debug("fetchRelated: ratingKey={}", ratingKey);
    items.clear();

    HttpClient client;
    // The server's "related" hubs (e.g. "Related Movies", "From the director").
    std::string url = buildApiUrl("/hubs/metadata/" + ratingKey + "/related?excludeFields=summary");

    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    if (resp.statusCode != 200) {
        brls::Logger::warning("fetchRelated: status {}", resp.statusCode);
        if (isAuthError(resp.statusCode)) handleUnauthorized();
        return false;
    }

    // Flatten every Metadata item across the related hubs, de-duplicated and
    // excluding the item itself. Each item is an object carrying "ratingKey".
    size_t pos = 0;
    while ((pos = resp.body.find("\"ratingKey\"", pos)) != std::string::npos) {
        size_t objStart = resp.body.rfind('{', pos);
        if (objStart == std::string::npos) { pos++; continue; }

        int braceCount = 1;
        size_t objEnd = objStart + 1;
        while (braceCount > 0 && objEnd < resp.body.length()) {
            if (resp.body[objEnd] == '{') braceCount++;
            else if (resp.body[objEnd] == '}') braceCount--;
            objEnd++;
        }

        std::string obj = resp.body.substr(objStart, objEnd - objStart);

        MediaItem item;
        item.ratingKey = extractJsonValue(obj, "ratingKey");
        item.key = extractJsonValue(obj, "key");
        item.title = extractJsonValue(obj, "title");
        item.thumb = extractJsonValue(obj, "thumb");
        item.art = extractJsonValue(obj, "art");
        item.type = extractJsonValue(obj, "type");
        item.mediaType = parseMediaType(item.type);
        item.year = extractJsonInt(obj, "year");
        item.duration = extractJsonInt(obj, "duration");
        item.viewOffset = extractJsonInt(obj, "viewOffset");
        item.rating = extractJsonFloat(obj, "rating");
        item.audienceRating = extractJsonFloat(obj, "audienceRating");

        bool playable = (item.mediaType == MediaType::MOVIE ||
                         item.mediaType == MediaType::SHOW);
        if (!item.ratingKey.empty() && !item.title.empty() &&
            item.ratingKey != ratingKey && playable) {
            bool dup = false;
            for (const auto& e : items) {
                if (e.ratingKey == item.ratingKey) { dup = true; break; }
            }
            if (!dup) items.push_back(item);
        }

        pos = objEnd;
    }

    // Keep the carousel light.
    if (items.size() > 24) items.resize(24);
    brls::Logger::info("fetchRelated: {} related items", items.size());
    return true;
}

bool PlexClient::fetchExtras(const std::string& ratingKey, std::vector<MediaItem>& items) {
    brls::Logger::debug("fetchExtras: ratingKey={}", ratingKey);

    HttpClient client;
    std::string url = buildApiUrl("/library/metadata/" + ratingKey + "/extras");

    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    brls::Logger::debug("Extras response: {} - {} bytes", resp.statusCode, resp.body.length());

    if (resp.statusCode != 200) {
        brls::Logger::error("Failed to fetch extras: {}", resp.statusCode);
        if (isAuthError(resp.statusCode)) handleUnauthorized();
        return false;
    }

    items.clear();

    // Parse media items by looking for objects with ratingKey
    size_t pos = 0;
    while ((pos = resp.body.find("\"ratingKey\"", pos)) != std::string::npos) {
        size_t objStart = resp.body.rfind('{', pos);
        if (objStart == std::string::npos) {
            pos++;
            continue;
        }

        int braceCount = 1;
        size_t objEnd = objStart + 1;
        while (braceCount > 0 && objEnd < resp.body.length()) {
            if (resp.body[objEnd] == '{') braceCount++;
            else if (resp.body[objEnd] == '}') braceCount--;
            objEnd++;
        }

        std::string obj = resp.body.substr(objStart, objEnd - objStart);

        MediaItem item;
        item.ratingKey = extractJsonValue(obj, "ratingKey");
        item.key = extractJsonValue(obj, "key");
        item.title = extractJsonValue(obj, "title");
        item.summary = extractJsonValue(obj, "summary");
        item.thumb = extractJsonValue(obj, "thumb");
        item.type = extractJsonValue(obj, "type");
        item.mediaType = parseMediaType(item.type);
        item.duration = extractJsonInt(obj, "duration");
        item.subtype = extractJsonValue(obj, "subtype");

        if (!item.ratingKey.empty() && !item.title.empty()) {
            items.push_back(item);
        }

        pos = objEnd;
    }

    brls::Logger::info("Found {} extras", items.size());
    return true;
}

bool PlexClient::fetchArtistHubs(const std::string& ratingKey, std::vector<Hub>& hubs) {
    brls::Logger::debug("fetchArtistHubs: ratingKey={}", ratingKey);

    HttpClient client;
    std::string url = buildApiUrl("/hubs/metadata/" + ratingKey + "?count=999");

    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    brls::Logger::debug("ArtistHubs response: {} - {} bytes", resp.statusCode, resp.body.length());

    if (resp.statusCode != 200) {
        brls::Logger::error("Failed to fetch artist hubs: {}", resp.statusCode);
        if (isAuthError(resp.statusCode)) handleUnauthorized();
        return false;
    }

    hubs.clear();

    // Parse hubs - look for objects with "hubIdentifier" field
    size_t pos = 0;
    while ((pos = resp.body.find("\"hubIdentifier\"", pos)) != std::string::npos) {
        size_t hubStart = resp.body.rfind('{', pos);
        if (hubStart == std::string::npos) {
            pos++;
            continue;
        }

        int braceCount = 1;
        size_t hubEnd = hubStart + 1;
        while (braceCount > 0 && hubEnd < resp.body.length()) {
            if (resp.body[hubEnd] == '{') braceCount++;
            else if (resp.body[hubEnd] == '}') braceCount--;
            hubEnd++;
        }

        std::string hubObj = resp.body.substr(hubStart, hubEnd - hubStart);

        Hub hub;
        hub.title = extractJsonValue(hubObj, "title");
        hub.type = extractJsonValue(hubObj, "type");
        hub.hubIdentifier = extractJsonValue(hubObj, "hubIdentifier");
        hub.key = extractJsonValue(hubObj, "key");
        hub.more = extractJsonBool(hubObj, "more");

        // Parse items inside the hub
        size_t itemPos = 0;
        while ((itemPos = hubObj.find("\"ratingKey\"", itemPos)) != std::string::npos) {
            size_t itemStart = hubObj.rfind('{', itemPos);
            if (itemStart == std::string::npos) {
                itemPos++;
                continue;
            }

            int itemBraceCount = 1;
            size_t itemEnd = itemStart + 1;
            while (itemBraceCount > 0 && itemEnd < hubObj.length()) {
                if (hubObj[itemEnd] == '{') itemBraceCount++;
                else if (hubObj[itemEnd] == '}') itemBraceCount--;
                itemEnd++;
            }

            std::string itemObj = hubObj.substr(itemStart, itemEnd - itemStart);

            MediaItem item;
            item.ratingKey = extractJsonValue(itemObj, "ratingKey");
            item.title = extractJsonValue(itemObj, "title");
            item.thumb = extractJsonValue(itemObj, "thumb");
            item.art = extractJsonValue(itemObj, "art");
            item.type = extractJsonValue(itemObj, "type");
            item.mediaType = parseMediaType(item.type);
            item.year = extractJsonInt(itemObj, "year");
            item.summary = extractJsonValue(itemObj, "summary");
            item.leafCount = extractJsonInt(itemObj, "leafCount");
            item.viewedLeafCount = extractJsonInt(itemObj, "viewedLeafCount");
            item.subtype = extractJsonValue(itemObj, "subtype");
            item.parentTitle = extractJsonValue(itemObj, "parentTitle");
            item.grandparentTitle = extractJsonValue(itemObj, "grandparentTitle");

            if (!item.ratingKey.empty() && !item.title.empty()) {
                hub.items.push_back(item);
            }

            itemPos = itemEnd;
        }

        if (!hub.title.empty() && !hub.items.empty()) {
            hubs.push_back(hub);
        }

        pos = hubEnd;
    }

    brls::Logger::info("Found {} artist hubs", hubs.size());

    // For hubs with more items available, fetch the full list using the hub's key
    for (auto& hub : hubs) {
        if (!hub.more || hub.key.empty()) continue;

        brls::Logger::info("Fetching full hub '{}' ({} items so far, more=true)", hub.title, hub.items.size());

        HttpClient hubClient;
        std::string hubUrl = buildApiUrl(hub.key);
        HttpRequest hubReq;
        hubReq.url = hubUrl;
        hubReq.method = "GET";
        hubReq.headers["Accept"] = "application/json";
        HttpResponse hubResp = hubClient.request(hubReq);

        if (hubResp.statusCode != 200) {
            brls::Logger::error("Failed to fetch full hub '{}': {}", hub.title, hubResp.statusCode);
            continue;
        }

        // Parse all items from the full hub response
        std::vector<MediaItem> fullItems;
        size_t itemPos = 0;
        while ((itemPos = hubResp.body.find("\"ratingKey\"", itemPos)) != std::string::npos) {
            size_t itemStart = hubResp.body.rfind('{', itemPos);
            if (itemStart == std::string::npos) {
                itemPos++;
                continue;
            }

            int itemBraceCount = 1;
            size_t itemEnd = itemStart + 1;
            while (itemBraceCount > 0 && itemEnd < hubResp.body.length()) {
                if (hubResp.body[itemEnd] == '{') itemBraceCount++;
                else if (hubResp.body[itemEnd] == '}') itemBraceCount--;
                itemEnd++;
            }

            std::string itemObj = hubResp.body.substr(itemStart, itemEnd - itemStart);

            MediaItem item;
            item.ratingKey = extractJsonValue(itemObj, "ratingKey");
            item.title = extractJsonValue(itemObj, "title");
            item.thumb = extractJsonValue(itemObj, "thumb");
            item.art = extractJsonValue(itemObj, "art");
            item.type = extractJsonValue(itemObj, "type");
            item.mediaType = parseMediaType(item.type);
            item.year = extractJsonInt(itemObj, "year");
            item.summary = extractJsonValue(itemObj, "summary");
            item.leafCount = extractJsonInt(itemObj, "leafCount");
            item.viewedLeafCount = extractJsonInt(itemObj, "viewedLeafCount");
            item.subtype = extractJsonValue(itemObj, "subtype");
            item.parentTitle = extractJsonValue(itemObj, "parentTitle");
            item.grandparentTitle = extractJsonValue(itemObj, "grandparentTitle");

            if (!item.ratingKey.empty() && !item.title.empty()) {
                fullItems.push_back(item);
            }

            itemPos = itemEnd;
        }

        if (!fullItems.empty()) {
            brls::Logger::info("Hub '{}': replaced {} items with {} from full fetch", hub.title, hub.items.size(), fullItems.size());
            hub.items = std::move(fullItems);
        }
    }

    return true;
}

bool PlexClient::fetchArtistAlbumsByFilter(const std::string& sectionKey,
                                          const std::string& artistRatingKey,
                                          const std::string& filter,
                                          std::vector<MediaItem>& items) {
    items.clear();
    if (sectionKey.empty() || artistRatingKey.empty() || filter.empty()) return false;

    brls::Logger::debug("fetchArtistAlbumsByFilter: section={} artist={} filter={}",
                        sectionKey, artistRatingKey, filter);

    HttpClient client;
    // Mirror the official client: a section query scoped to the artist (type=9
    // = album) filtered by a release-type token (album.format=... for primary
    // types like Single/EP, album.subformat=... for secondary types). No
    // group=title here: an artist can have several distinct same-titled releases
    // (e.g. multiple "Total Coverage" compilations) and grouping would collapse
    // them into one.
    std::string url = buildApiUrl("/library/sections/" + sectionKey +
        "/all?type=9&artist.id=" + artistRatingKey +
        "&" + filter +
        "&sort=year:desc");

    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    if (resp.statusCode != 200) {
        brls::Logger::error("fetchArtistAlbumsByFilter failed: {}", resp.statusCode);
        if (isAuthError(resp.statusCode)) handleUnauthorized();
        return false;
    }

    size_t pos = 0;
    while ((pos = resp.body.find("\"ratingKey\"", pos)) != std::string::npos) {
        size_t objStart = resp.body.rfind('{', pos);
        if (objStart == std::string::npos) { pos++; continue; }
        int braceCount = 1;
        size_t objEnd = objStart + 1;
        while (braceCount > 0 && objEnd < resp.body.length()) {
            if (resp.body[objEnd] == '{') braceCount++;
            else if (resp.body[objEnd] == '}') braceCount--;
            objEnd++;
        }
        std::string obj = resp.body.substr(objStart, objEnd - objStart);

        MediaItem item;
        item.ratingKey   = extractJsonValue(obj, "ratingKey");
        item.key         = extractJsonValue(obj, "key");
        item.title       = extractJsonValue(obj, "title");
        item.thumb       = extractJsonValue(obj, "thumb");
        item.art         = extractJsonValue(obj, "art");
        item.type        = extractJsonValue(obj, "type");
        item.mediaType   = parseMediaType(item.type);
        item.year        = extractJsonInt(obj, "year");
        item.parentTitle = extractJsonValue(obj, "parentTitle");
        item.leafCount   = extractJsonInt(obj, "leafCount");

        if (!item.ratingKey.empty() && !item.title.empty() &&
            item.mediaType == MediaType::MUSIC_ALBUM) {
            items.push_back(item);
        }
        pos = objEnd;
    }

    brls::Logger::info("fetchArtistAlbumsByFilter [{}]: {} albums", filter, items.size());
    return true;
}

bool PlexClient::fetchHubs(std::vector<Hub>& hubs) {
    brls::Logger::debug("fetchHubs: serverUrl={}", m_serverUrl);

    std::string url = buildApiUrl("/hubs");

    // Cache the Home hub bundle — biggest single response on the Home
    // tab, fetched every time it gets focus. Stale by at most one TTL
    // tick after the user adds new content; user can Clear Cache in
    // Settings if they want the new items immediately.
    const int ttlSec = Application::getInstance().getSettings().cacheLifetimeMinutes * 60;
    std::string body;
    bool fromCache = HttpCache::get(url, ttlSec, body);

    if (!fromCache) {
        HttpClient client;
        // Request JSON format
        HttpRequest req;
        req.url = url;
        req.method = "GET";
        req.headers["Accept"] = "application/json";
        HttpResponse resp = client.request(req);
        brls::Logger::debug("Hubs response: {} - {} bytes", resp.statusCode, resp.body.length());

        if (resp.statusCode != 200) {
            brls::Logger::error("Failed to fetch hubs: {}", resp.statusCode);
            if (isAuthError(resp.statusCode)) handleUnauthorized();
            return false;
        }
        body = resp.body;
        HttpCache::put(url, body, ttlSec);
    }

    hubs.clear();

    // Parse hubs - look for objects with "hubIdentifier" field
    size_t pos = 0;
    while ((pos = body.find("\"hubIdentifier\"", pos)) != std::string::npos) {
        // Go back to find start of this hub object
        size_t hubStart = body.rfind('{', pos);
        if (hubStart == std::string::npos) {
            pos++;
            continue;
        }

        // Find end of hub object
        int braceCount = 1;
        size_t hubEnd = hubStart + 1;
        while (braceCount > 0 && hubEnd < body.length()) {
            if (body[hubEnd] == '{') braceCount++;
            else if (body[hubEnd] == '}') braceCount--;
            hubEnd++;
        }

        std::string hubObj = body.substr(hubStart, hubEnd - hubStart);

        Hub hub;
        hub.title = extractJsonValue(hubObj, "title");
        hub.type = extractJsonValue(hubObj, "type");
        hub.hubIdentifier = extractJsonValue(hubObj, "hubIdentifier");
        hub.key = extractJsonValue(hubObj, "key");
        hub.more = extractJsonBool(hubObj, "more");

        // Parse items inside the hub by looking for ratingKey
        size_t itemPos = 0;
        while ((itemPos = hubObj.find("\"ratingKey\"", itemPos)) != std::string::npos) {
            // Go back to find start of this item object
            size_t itemStart = hubObj.rfind('{', itemPos);
            if (itemStart == std::string::npos) {
                itemPos++;
                continue;
            }

            // Find end of item object
            int itemBraceCount = 1;
            size_t itemEnd = itemStart + 1;
            while (itemBraceCount > 0 && itemEnd < hubObj.length()) {
                if (hubObj[itemEnd] == '{') itemBraceCount++;
                else if (hubObj[itemEnd] == '}') itemBraceCount--;
                itemEnd++;
            }

            std::string itemObj = hubObj.substr(itemStart, itemEnd - itemStart);

            MediaItem item;
            item.ratingKey = extractJsonValue(itemObj, "ratingKey");
            item.title = extractJsonValue(itemObj, "title");
            item.thumb = extractJsonValue(itemObj, "thumb");
            item.type = extractJsonValue(itemObj, "type");
            item.mediaType = parseMediaType(item.type);
            item.year = extractJsonInt(itemObj, "year");
            item.viewOffset = extractJsonInt(itemObj, "viewOffset");
            item.rating = extractJsonFloat(itemObj, "rating");
            item.audienceRating = extractJsonFloat(itemObj, "audienceRating");

            if (!item.ratingKey.empty() && !item.title.empty()) {
                hub.items.push_back(item);
            }

            itemPos = itemEnd;
        }

        if (!hub.title.empty()) {
            hubs.push_back(hub);
        }

        pos = hubEnd;
    }

    brls::Logger::info("Found {} hubs", hubs.size());
    return true;
}

bool PlexClient::fetchContinueWatching(std::vector<MediaItem>& items) {
    brls::Logger::debug("fetchContinueWatching: serverUrl={}", m_serverUrl);

    HttpClient client;
    std::string url = buildApiUrl("/hubs/continueWatching");

    // Request JSON format
    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    brls::Logger::debug("ContinueWatching response: {} - {} bytes", resp.statusCode, resp.body.length());

    if (resp.statusCode != 200) {
        brls::Logger::error("Failed to fetch continue watching: {}", resp.statusCode);
        if (isAuthError(resp.statusCode)) handleUnauthorized();
        return false;
    }

    items.clear();

    // Parse media items by looking for objects with ratingKey
    size_t pos = 0;
    while ((pos = resp.body.find("\"ratingKey\"", pos)) != std::string::npos) {
        // Go back to find start of this object
        size_t objStart = resp.body.rfind('{', pos);
        if (objStart == std::string::npos) {
            pos++;
            continue;
        }

        // Find end of object
        int braceCount = 1;
        size_t objEnd = objStart + 1;
        while (braceCount > 0 && objEnd < resp.body.length()) {
            if (resp.body[objEnd] == '{') braceCount++;
            else if (resp.body[objEnd] == '}') braceCount--;
            objEnd++;
        }

        std::string obj = resp.body.substr(objStart, objEnd - objStart);

        MediaItem item;
        item.ratingKey = extractJsonValue(obj, "ratingKey");
        item.key = extractJsonValue(obj, "key");
        item.title = extractJsonValue(obj, "title");
        item.summary = extractJsonValue(obj, "summary");
        item.thumb = extractJsonValue(obj, "thumb");
        item.art = extractJsonValue(obj, "art");
        item.type = extractJsonValue(obj, "type");
        item.mediaType = parseMediaType(item.type);
        item.year = extractJsonInt(obj, "year");
        item.duration = extractJsonInt(obj, "duration");
        item.viewOffset = extractJsonInt(obj, "viewOffset");
        item.grandparentTitle = extractJsonValue(obj, "grandparentTitle");
        item.parentTitle = extractJsonValue(obj, "parentTitle");
        item.grandparentThumb = extractJsonValue(obj, "grandparentThumb");
        item.parentThumb = extractJsonValue(obj, "parentThumb");
        item.index = extractJsonInt(obj, "index");
        item.parentIndex = extractJsonInt(obj, "parentIndex");
        item.rating = extractJsonFloat(obj, "rating");
        item.audienceRating = extractJsonFloat(obj, "audienceRating");

        if (!item.ratingKey.empty() && !item.title.empty()) {
            items.push_back(item);
        }

        pos = objEnd;
    }

    brls::Logger::info("Found {} continue watching items", items.size());
    return true;
}

bool PlexClient::fetchRecentlyAdded(std::vector<MediaItem>& items) {
    brls::Logger::debug("fetchRecentlyAdded: serverUrl={}", m_serverUrl);

    HttpClient client;
    std::string url = buildApiUrl("/library/recentlyAdded");

    // Request JSON format
    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    brls::Logger::debug("RecentlyAdded response: {} - {} bytes", resp.statusCode, resp.body.length());

    if (resp.statusCode != 200) {
        brls::Logger::error("Failed to fetch recently added: {}", resp.statusCode);
        if (isAuthError(resp.statusCode)) handleUnauthorized();
        return false;
    }

    items.clear();

    // Parse media items by looking for objects with ratingKey
    size_t pos = 0;
    while ((pos = resp.body.find("\"ratingKey\"", pos)) != std::string::npos) {
        // Go back to find start of this object
        size_t objStart = resp.body.rfind('{', pos);
        if (objStart == std::string::npos) {
            pos++;
            continue;
        }

        // Find end of object
        int braceCount = 1;
        size_t objEnd = objStart + 1;
        while (braceCount > 0 && objEnd < resp.body.length()) {
            if (resp.body[objEnd] == '{') braceCount++;
            else if (resp.body[objEnd] == '}') braceCount--;
            objEnd++;
        }

        std::string obj = resp.body.substr(objStart, objEnd - objStart);

        MediaItem item;
        item.ratingKey = extractJsonValue(obj, "ratingKey");
        item.key = extractJsonValue(obj, "key");
        item.title = extractJsonValue(obj, "title");
        item.summary = extractJsonValue(obj, "summary");
        item.thumb = extractJsonValue(obj, "thumb");
        item.art = extractJsonValue(obj, "art");
        item.type = extractJsonValue(obj, "type");
        item.mediaType = parseMediaType(item.type);
        item.year = extractJsonInt(obj, "year");
        item.duration = extractJsonInt(obj, "duration");
        item.viewOffset = extractJsonInt(obj, "viewOffset");
        item.rating = extractJsonFloat(obj, "rating");
        item.audienceRating = extractJsonFloat(obj, "audienceRating");

        if (!item.ratingKey.empty() && !item.title.empty()) {
            items.push_back(item);
        }

        pos = objEnd;
    }

    brls::Logger::info("Found {} recently added items", items.size());
    return true;
}

bool PlexClient::fetchRecentlyAddedByType(MediaType type, std::vector<MediaItem>& items) {
    brls::Logger::debug("fetchRecentlyAddedByType: type={}", static_cast<int>(type));

    // Plex API type codes: 1=movie, 2=show, 8=artist (music), 9=album, 10=track
    int typeCode = 0;
    std::string typeName;
    switch (type) {
        case MediaType::MOVIE:
            typeCode = 1;
            typeName = "movie";
            break;
        case MediaType::SHOW:
        case MediaType::EPISODE:
            typeCode = 2;
            typeName = "show";
            break;
        case MediaType::MUSIC_ARTIST:
            typeCode = 8;
            typeName = "artist";
            break;
        case MediaType::MUSIC_ALBUM:
            typeCode = 9;
            typeName = "album";
            break;
        case MediaType::MUSIC_TRACK:
            typeCode = 10;
            typeName = "track";
            break;
        default:
            return fetchRecentlyAdded(items);  // Fallback to all types
    }

    HttpClient client;
    std::string url = buildApiUrl("/library/recentlyAdded?type=" + std::to_string(typeCode));

    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    brls::Logger::debug("RecentlyAddedByType response: {} - {} bytes", resp.statusCode, resp.body.length());

    if (resp.statusCode != 200) {
        brls::Logger::error("Failed to fetch recently added by type: {}", resp.statusCode);
        return false;
    }

    items.clear();

    // Parse media items by looking for objects with ratingKey
    size_t pos = 0;
    while ((pos = resp.body.find("\"ratingKey\"", pos)) != std::string::npos) {
        size_t objStart = resp.body.rfind('{', pos);
        if (objStart == std::string::npos) {
            pos++;
            continue;
        }

        int braceCount = 1;
        size_t objEnd = objStart + 1;
        while (braceCount > 0 && objEnd < resp.body.length()) {
            if (resp.body[objEnd] == '{') braceCount++;
            else if (resp.body[objEnd] == '}') braceCount--;
            objEnd++;
        }

        std::string obj = resp.body.substr(objStart, objEnd - objStart);

        MediaItem item;
        item.ratingKey = extractJsonValue(obj, "ratingKey");
        item.key = extractJsonValue(obj, "key");
        item.title = extractJsonValue(obj, "title");
        item.summary = extractJsonValue(obj, "summary");
        item.thumb = extractJsonValue(obj, "thumb");
        item.art = extractJsonValue(obj, "art");
        item.type = extractJsonValue(obj, "type");
        item.mediaType = parseMediaType(item.type);
        item.year = extractJsonInt(obj, "year");
        item.duration = extractJsonInt(obj, "duration");
        item.viewOffset = extractJsonInt(obj, "viewOffset");
        item.rating = extractJsonFloat(obj, "rating");
        item.audienceRating = extractJsonFloat(obj, "audienceRating");

        if (!item.ratingKey.empty() && !item.title.empty()) {
            items.push_back(item);
        }

        pos = objEnd;
    }

    brls::Logger::info("Found {} recently added {} items", items.size(), typeName);
    return true;
}

bool PlexClient::search(const std::string& query, std::vector<MediaItem>& results) {
    brls::Logger::debug("Searching for: {}", query);

    HttpClient client;
    // /hubs/search caps each hub (Movies, Episodes, …) at a small default (~3),
    // so pass an explicit limit to return the full result set per type.
    std::string url = buildApiUrl("/hubs/search?query=" + HttpClient::urlEncode(query) + "&limit=100");

    // Request JSON format
    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    brls::Logger::debug("Search response: {} - {} bytes", resp.statusCode, resp.body.length());

    if (resp.statusCode != 200) {
        brls::Logger::error("Search failed: {}", resp.statusCode);
        if (isAuthError(resp.statusCode)) handleUnauthorized();
        return false;
    }

    results.clear();

    // Parse search results by looking for objects with ratingKey
    size_t pos = 0;
    while ((pos = resp.body.find("\"ratingKey\"", pos)) != std::string::npos) {
        // Go back to find start of this object
        size_t objStart = resp.body.rfind('{', pos);
        if (objStart == std::string::npos) {
            pos++;
            continue;
        }

        // Find end of object
        int braceCount = 1;
        size_t objEnd = objStart + 1;
        while (braceCount > 0 && objEnd < resp.body.length()) {
            if (resp.body[objEnd] == '{') braceCount++;
            else if (resp.body[objEnd] == '}') braceCount--;
            objEnd++;
        }

        std::string obj = resp.body.substr(objStart, objEnd - objStart);

        MediaItem item;
        item.ratingKey = extractJsonValue(obj, "ratingKey");
        item.key = extractJsonValue(obj, "key");
        item.title = extractJsonValue(obj, "title");
        item.summary = extractJsonValue(obj, "summary");
        item.thumb = extractJsonValue(obj, "thumb");
        item.art = extractJsonValue(obj, "art");
        item.type = extractJsonValue(obj, "type");
        item.mediaType = parseMediaType(item.type);
        item.year = extractJsonInt(obj, "year");
        item.duration = extractJsonInt(obj, "duration");
        item.index = extractJsonInt(obj, "index");             // episode / track number
        item.parentIndex = extractJsonInt(obj, "parentIndex"); // season number
        item.grandparentTitle = extractJsonValue(obj, "grandparentTitle");
        item.parentTitle = extractJsonValue(obj, "parentTitle");
        item.parentThumb = extractJsonValue(obj, "parentThumb");
        item.grandparentThumb = extractJsonValue(obj, "grandparentThumb");
        item.rating = extractJsonFloat(obj, "rating");
        item.audienceRating = extractJsonFloat(obj, "audienceRating");

        if (!item.ratingKey.empty() && !item.title.empty()) {
            results.push_back(item);
        }

        pos = objEnd;
    }

    brls::Logger::info("Found {} search results for '{}'", results.size(), query);
    return true;
}

bool PlexClient::fetchCollections(const std::string& sectionKey, std::vector<MediaItem>& collections) {
    brls::Logger::debug("Fetching collections for section: {}", sectionKey);

    HttpClient client;
    // Use proper Plex API: /library/sections/{key}/all?type=18 (18 = collection type)
    // Also try /library/sections/{key}/collections as fallback
    std::string url = buildApiUrl("/library/sections/" + sectionKey + "/all?type=18");

    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    // Fallback to /collections endpoint if type=18 fails
    if (resp.statusCode != 200) {
        url = buildApiUrl("/library/sections/" + sectionKey + "/collections");
        req.url = url;
        resp = client.request(req);
    }

    if (resp.statusCode != 200) {
        brls::Logger::error("Failed to fetch collections: {}", resp.statusCode);
        return false;
    }

    collections.clear();

    // Parse collections from JSON - look for Metadata entries
    size_t pos = 0;
    while ((pos = resp.body.find("\"ratingKey\"", pos)) != std::string::npos) {
        size_t objStart = resp.body.rfind('{', pos);
        if (objStart == std::string::npos) {
            pos++;
            continue;
        }

        int braceCount = 1;
        size_t objEnd = objStart + 1;
        while (braceCount > 0 && objEnd < resp.body.length()) {
            if (resp.body[objEnd] == '{') braceCount++;
            else if (resp.body[objEnd] == '}') braceCount--;
            objEnd++;
        }

        std::string obj = resp.body.substr(objStart, objEnd - objStart);

        MediaItem item;
        item.ratingKey = extractJsonValue(obj, "ratingKey");
        item.key = extractJsonValue(obj, "key");
        item.title = extractJsonValue(obj, "title");
        item.summary = extractJsonValue(obj, "summary");
        item.thumb = extractJsonValue(obj, "thumb");
        item.art = extractJsonValue(obj, "art");
        item.type = "collection";
        item.subtype = extractJsonValue(obj, "subtype");
        item.mediaType = MediaType::UNKNOWN;  // Collections are containers
        item.leafCount = extractJsonInt(obj, "childCount");
        if (item.leafCount == 0) {
            item.leafCount = extractJsonInt(obj, "ratingCount");
        }

        if (!item.ratingKey.empty() && !item.title.empty()) {
            collections.push_back(item);
        }

        pos = objEnd;
    }

    brls::Logger::info("Found {} collections for section {}", collections.size(), sectionKey);
    return true;
}

bool PlexClient::fetchPlaylists(std::vector<MediaItem>& playlists) {
    brls::Logger::debug("Fetching playlists");

    HttpClient client;
    std::string url = buildApiUrl("/playlists");

    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    if (resp.statusCode != 200) {
        brls::Logger::error("Failed to fetch playlists: {}", resp.statusCode);
        return false;
    }

    playlists.clear();

    // Parse playlists from JSON
    size_t pos = 0;
    while ((pos = resp.body.find("\"ratingKey\"", pos)) != std::string::npos) {
        size_t objStart = resp.body.rfind('{', pos);
        if (objStart == std::string::npos) {
            pos++;
            continue;
        }

        int braceCount = 1;
        size_t objEnd = objStart + 1;
        while (braceCount > 0 && objEnd < resp.body.length()) {
            if (resp.body[objEnd] == '{') braceCount++;
            else if (resp.body[objEnd] == '}') braceCount--;
            objEnd++;
        }

        std::string obj = resp.body.substr(objStart, objEnd - objStart);

        MediaItem item;
        item.ratingKey = extractJsonValue(obj, "ratingKey");
        item.key = extractJsonValue(obj, "key");
        item.title = extractJsonValue(obj, "title");
        item.summary = extractJsonValue(obj, "summary");
        item.thumb = extractJsonValue(obj, "composite");  // Playlists use composite image
        if (item.thumb.empty()) {
            item.thumb = extractJsonValue(obj, "thumb");
        }
        item.type = extractJsonValue(obj, "playlistType");  // video, audio, photo
        item.mediaType = MediaType::UNKNOWN;
        item.leafCount = extractJsonInt(obj, "leafCount");
        item.duration = extractJsonInt(obj, "duration");

        if (!item.ratingKey.empty() && !item.title.empty()) {
            playlists.push_back(item);
        }

        pos = objEnd;
    }

    brls::Logger::info("Found {} playlists", playlists.size());
    return true;
}

bool PlexClient::fetchGenres(const std::string& sectionKey, std::vector<std::string>& genres) {
    brls::Logger::debug("Fetching genres for section: {}", sectionKey);

    HttpClient client;
    // Plex API: /library/sections/{key}/genre returns list of genres
    std::string url = buildApiUrl("/library/sections/" + sectionKey + "/genre");

    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    if (resp.statusCode != 200) {
        brls::Logger::error("Failed to fetch genres: {}", resp.statusCode);
        return false;
    }

    genres.clear();

    // Parse genres - look for "title" fields in Directory entries
    size_t pos = 0;
    while ((pos = resp.body.find("\"title\"", pos)) != std::string::npos) {
        std::string title = extractJsonValue(resp.body.substr(pos > 20 ? pos - 20 : 0, 300), "title");
        if (!title.empty() && title != "genre" && title.find("MediaContainer") == std::string::npos) {
            // Avoid duplicates
            bool found = false;
            for (const auto& g : genres) {
                if (g == title) { found = true; break; }
            }
            if (!found) {
                genres.push_back(title);
            }
        }
        pos++;
    }

    brls::Logger::info("Found {} genres for section {}", genres.size(), sectionKey);
    return true;
}

bool PlexClient::fetchFilterValues(const std::string& sectionKey,
                                   const std::string& field,
                                   std::vector<GenreItem>& values) {
    brls::Logger::debug("Fetching filter '{}' values for section: {}", field, sectionKey);

    HttpClient client;
    // Plex exposes each filter's choices at /library/sections/{key}/{field}
    // (genre, year, decade, contentRating, resolution, studio, country, …),
    // each a Directory with a title and a key. The key is what feeds the
    // matching ?{field}={key} query when filtering /all.
    std::string url = buildApiUrl("/library/sections/" + sectionKey + "/" + field);

    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    if (resp.statusCode != 200) {
        brls::Logger::error("Failed to fetch filter '{}' values: {}", field, resp.statusCode);
        return false;
    }

    values.clear();

    // Parse Directory entries with "title", "key", and "fastKey"
    size_t pos = 0;
    while ((pos = resp.body.find("\"key\"", pos)) != std::string::npos) {
        // Go back to find start of this object
        size_t objStart = resp.body.rfind('{', pos);
        if (objStart == std::string::npos) {
            pos++;
            continue;
        }

        // Find end of object
        int braceCount = 1;
        size_t objEnd = objStart + 1;
        while (braceCount > 0 && objEnd < resp.body.length()) {
            if (resp.body[objEnd] == '{') braceCount++;
            else if (resp.body[objEnd] == '}') braceCount--;
            objEnd++;
        }

        std::string obj = resp.body.substr(objStart, objEnd - objStart);

        GenreItem item;
        item.title = extractJsonValue(obj, "title");
        item.key = extractJsonValue(obj, "key");
        item.fastKey = extractJsonValue(obj, "fastKey");

        // Skip container/meta entries (the wrapper's title is the field name)
        if (!item.title.empty() && !item.key.empty() &&
            item.title != field && item.title.find("MediaContainer") == std::string::npos) {
            // Avoid duplicates
            bool found = false;
            for (const auto& g : values) {
                if (g.title == item.title) { found = true; break; }
            }
            if (!found) {
                values.push_back(item);
            }
        }

        pos = objEnd;
    }

    brls::Logger::info("Found {} '{}' filter values for section {}", values.size(), field, sectionKey);
    return true;
}

bool PlexClient::fetchGenreItems(const std::string& sectionKey, std::vector<GenreItem>& genres) {
    return fetchFilterValues(sectionKey, "genre", genres);
}

bool PlexClient::fetchByGenre(const std::string& sectionKey, const std::string& genre, std::vector<MediaItem>& items, int metadataType) {
    brls::Logger::debug("Fetching items by genre: {} in section {} type={}", genre, sectionKey, metadataType);

    HttpClient client;
    // Plex API: /library/sections/{key}/all?genre={genreId}&type={metadataType}
    std::string url = buildApiUrl("/library/sections/" + sectionKey + "/all?genre=" + HttpClient::urlEncode(genre));
    if (metadataType > 0) {
        url += "&type=" + std::to_string(metadataType);
    }

    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    if (resp.statusCode != 200) {
        brls::Logger::error("Failed to fetch by genre: {}", resp.statusCode);
        return false;
    }

    items.clear();

    // Parse items - same as fetchLibraryContent
    size_t pos = 0;
    while ((pos = resp.body.find("\"ratingKey\"", pos)) != std::string::npos) {
        size_t objStart = resp.body.rfind('{', pos);
        if (objStart == std::string::npos) {
            pos++;
            continue;
        }

        int braceCount = 1;
        size_t objEnd = objStart + 1;
        while (braceCount > 0 && objEnd < resp.body.length()) {
            if (resp.body[objEnd] == '{') braceCount++;
            else if (resp.body[objEnd] == '}') braceCount--;
            objEnd++;
        }

        std::string obj = resp.body.substr(objStart, objEnd - objStart);

        MediaItem item;
        item.ratingKey = extractJsonValue(obj, "ratingKey");
        item.key = extractJsonValue(obj, "key");
        item.title = extractJsonValue(obj, "title");
        item.summary = extractJsonValue(obj, "summary");
        item.thumb = extractJsonValue(obj, "thumb");
        item.art = extractJsonValue(obj, "art");
        item.type = extractJsonValue(obj, "type");
        item.mediaType = parseMediaType(item.type);
        item.year = extractJsonInt(obj, "year");
        item.duration = extractJsonInt(obj, "duration");
        item.rating = extractJsonFloat(obj, "rating");
        item.audienceRating = extractJsonFloat(obj, "audienceRating");

        if (!item.ratingKey.empty() && !item.title.empty()) {
            items.push_back(item);
        }

        pos = objEnd;
    }

    brls::Logger::info("Found {} items for genre '{}' in section {}", items.size(), genre, sectionKey);
    return true;
}

bool PlexClient::fetchByGenreKey(const std::string& sectionKey, const std::string& genreKey, std::vector<MediaItem>& items, int metadataType) {
    brls::Logger::debug("Fetching items by genre key: {} in section {} type={}", genreKey, sectionKey, metadataType);

    HttpClient client;
    // Plex API: Use the fastKey or construct filter with genre key/ID
    // The genreKey is typically the numeric ID from the genre list
    std::string url = buildApiUrl("/library/sections/" + sectionKey + "/all?genre=" + genreKey);
    if (metadataType > 0) {
        url += "&type=" + std::to_string(metadataType);
    }

    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    if (resp.statusCode != 200) {
        brls::Logger::error("Failed to fetch by genre key: {}", resp.statusCode);
        return false;
    }

    items.clear();

    // Parse items
    size_t pos = 0;
    while ((pos = resp.body.find("\"ratingKey\"", pos)) != std::string::npos) {
        size_t objStart = resp.body.rfind('{', pos);
        if (objStart == std::string::npos) {
            pos++;
            continue;
        }

        int braceCount = 1;
        size_t objEnd = objStart + 1;
        while (braceCount > 0 && objEnd < resp.body.length()) {
            if (resp.body[objEnd] == '{') braceCount++;
            else if (resp.body[objEnd] == '}') braceCount--;
            objEnd++;
        }

        std::string obj = resp.body.substr(objStart, objEnd - objStart);

        MediaItem item;
        item.ratingKey = extractJsonValue(obj, "ratingKey");
        item.key = extractJsonValue(obj, "key");
        item.title = extractJsonValue(obj, "title");
        item.summary = extractJsonValue(obj, "summary");
        item.thumb = extractJsonValue(obj, "thumb");
        item.art = extractJsonValue(obj, "art");
        item.type = extractJsonValue(obj, "type");
        item.mediaType = parseMediaType(item.type);
        item.year = extractJsonInt(obj, "year");
        item.duration = extractJsonInt(obj, "duration");
        item.rating = extractJsonFloat(obj, "rating");
        item.audienceRating = extractJsonFloat(obj, "audienceRating");

        if (!item.ratingKey.empty() && !item.title.empty()) {
            items.push_back(item);
        }

        pos = objEnd;
    }

    brls::Logger::info("Found {} items for genre key '{}' in section {}", items.size(), genreKey, sectionKey);
    return true;
}

bool PlexClient::getPlaybackUrl(const std::string& ratingKey, std::string& url) {
    brls::Logger::debug("getPlaybackUrl: ratingKey={}", ratingKey);

    // Fetch media details to get the Part key for streaming
    HttpClient client;
    std::string apiUrl = buildApiUrl("/library/metadata/" + ratingKey);

    HttpRequest req;
    req.url = apiUrl;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    if (resp.statusCode != 200) {
        brls::Logger::error("getPlaybackUrl: Failed to fetch metadata: {}", resp.statusCode);
        if (isAuthError(resp.statusCode)) handleUnauthorized();
        return false;
    }

    // Find the Part key in the response
    // Look for "Part":[{"key":"/library/parts/..."
    size_t partPos = resp.body.find("\"Part\"");
    if (partPos == std::string::npos) {
        brls::Logger::error("getPlaybackUrl: No Part found in metadata");
        return false;
    }

    // Find the key within Part
    size_t keyPos = resp.body.find("\"key\"", partPos);
    if (keyPos == std::string::npos || keyPos > partPos + 500) {
        brls::Logger::error("getPlaybackUrl: No key found in Part");
        return false;
    }

    std::string partKey = extractJsonValue(resp.body.substr(keyPos, 200), "key");
    if (partKey.empty()) {
        brls::Logger::error("getPlaybackUrl: Part key is empty");
        return false;
    }

    // Build stream URL from Part key
    // The Part key is something like /library/parts/12345/1234567890/file.mkv
    url = m_serverUrl + partKey + "?X-Plex-Token=" + m_authToken;

    brls::Logger::info("getPlaybackUrl: Stream URL = {}", url);
    return true;
}

bool PlexClient::fetchStreams(const std::string& ratingKey, std::vector<PlexStream>& streams, int& partId) {
    HttpClient client;
    std::string url = buildApiUrl("/library/metadata/" + ratingKey);

    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    if (resp.statusCode != 200) {
        brls::Logger::error("fetchStreams: Failed to fetch metadata: {}", resp.statusCode);
        return false;
    }

    streams.clear();
    partId = 0;

    // Find Part section and extract part ID
    size_t partPos = resp.body.find("\"Part\"");
    if (partPos == std::string::npos) {
        brls::Logger::error("fetchStreams: No Part found");
        return false;
    }

    // Extract part id
    partId = extractJsonInt(resp.body.substr(partPos, 500), "id");
    brls::Logger::debug("fetchStreams: partId={}", partId);

    // Find Stream array within Part
    size_t streamPos = resp.body.find("\"Stream\"", partPos);
    if (streamPos == std::string::npos) {
        brls::Logger::info("fetchStreams: No streams found in Part");
        return true;  // Valid - just no streams
    }

    // Find the stream array opening bracket
    size_t arrayStart = resp.body.find('[', streamPos);
    if (arrayStart == std::string::npos) return true;

    // Find the matching closing bracket
    int bracketCount = 1;
    size_t arrayEnd = arrayStart + 1;
    while (bracketCount > 0 && arrayEnd < resp.body.length()) {
        if (resp.body[arrayEnd] == '[') bracketCount++;
        else if (resp.body[arrayEnd] == ']') bracketCount--;
        arrayEnd++;
    }

    std::string streamArray = resp.body.substr(arrayStart, arrayEnd - arrayStart);

    // Parse individual stream objects
    size_t objPos = 0;
    while ((objPos = streamArray.find('{', objPos)) != std::string::npos) {
        // Find end of this object
        int braceCount = 1;
        size_t objEnd = objPos + 1;
        while (braceCount > 0 && objEnd < streamArray.length()) {
            if (streamArray[objEnd] == '{') braceCount++;
            else if (streamArray[objEnd] == '}') braceCount--;
            objEnd++;
        }

        std::string obj = streamArray.substr(objPos, objEnd - objPos);

        PlexStream stream;
        stream.id = extractJsonInt(obj, "id");
        stream.streamType = extractJsonInt(obj, "streamType");
        stream.codec = extractJsonValue(obj, "codec");
        stream.displayTitle = extractJsonValue(obj, "displayTitle");
        stream.language = extractJsonValue(obj, "language");
        stream.languageCode = extractJsonValue(obj, "languageCode");
        stream.selected = extractJsonBool(obj, "selected");
        stream.channels = extractJsonInt(obj, "channels");
        stream.title = extractJsonValue(obj, "title");
        stream.forced = extractJsonBool(obj, "forced");
        stream.hearingImpaired = extractJsonBool(obj, "hearingImpaired");
        // External (sidecar) subtitles carry a stream key; embedded don't.
        stream.external = !extractJsonValue(obj, "key").empty();

        if (stream.id > 0 && stream.streamType > 0) {
            streams.push_back(stream);
            brls::Logger::debug("fetchStreams: type={} id={} codec={} title={}",
                               stream.streamType, stream.id, stream.codec, stream.displayTitle);
        }

        objPos = objEnd;
    }

    brls::Logger::info("fetchStreams: Found {} streams for ratingKey {}", streams.size(), ratingKey);
    return true;
}

bool PlexClient::setStreamSelection(int partId, int audioStreamID, int subtitleStreamID) {
    // Plex API: PUT /library/parts/{partId}?audioStreamID=X&subtitleStreamID=Y
    std::string endpoint = "/library/parts/" + std::to_string(partId) + "?allParts=1";

    if (audioStreamID >= 0) {
        endpoint += "&audioStreamID=" + std::to_string(audioStreamID);
    }
    if (subtitleStreamID >= 0) {
        endpoint += "&subtitleStreamID=" + std::to_string(subtitleStreamID);
    }

    HttpClient client;
    std::string url = buildApiUrl(endpoint);

    HttpRequest req;
    req.url = url;
    req.method = "PUT";
    HttpResponse resp = client.request(req);

    if (resp.statusCode != 200) {
        brls::Logger::error("setStreamSelection: Failed: {}", resp.statusCode);
        return false;
    }

    brls::Logger::info("setStreamSelection: partId={} audio={} sub={}", partId, audioStreamID, subtitleStreamID);
    return true;
}

bool PlexClient::searchSubtitles(const std::string& ratingKey, const std::string& language,
                                  std::vector<SubtitleResult>& results) {
    HttpClient client;
    std::string endpoint = "/library/metadata/" + ratingKey + "/subtitles?language=" + language;
    std::string url = buildApiUrl(endpoint);

    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    if (resp.statusCode != 200) {
        brls::Logger::error("searchSubtitles: Failed: {}", resp.statusCode);
        return false;
    }

    results.clear();

    brls::Logger::debug("searchSubtitles: Response {} bytes, first 500: {}",
                       resp.body.size(), resp.body.substr(0, 500));

    // Try to find the subtitle entries array - Plex uses different keys depending on version
    // Try "Metadata", "MediaContainer", or look for arrays with "key" fields
    size_t arrayStart = std::string::npos;

    // Try "Metadata" key first
    size_t metaPos = resp.body.find("\"Metadata\"");
    if (metaPos != std::string::npos) {
        arrayStart = resp.body.find('[', metaPos);
    }

    // Try "Stream" key (some Plex versions return streams)
    if (arrayStart == std::string::npos) {
        size_t streamPos = resp.body.find("\"Stream\"");
        if (streamPos != std::string::npos) {
            arrayStart = resp.body.find('[', streamPos);
        }
    }

    // Try to find any JSON array that contains subtitle data
    if (arrayStart == std::string::npos) {
        // Look for first array in the response that contains "key" fields
        size_t searchPos = 0;
        while (searchPos < resp.body.size()) {
            size_t arrPos = resp.body.find('[', searchPos);
            if (arrPos == std::string::npos) break;
            // Check if this array contains subtitle-like objects
            size_t keyCheck = resp.body.find("\"key\"", arrPos);
            if (keyCheck != std::string::npos && keyCheck < arrPos + 500) {
                arrayStart = arrPos;
                break;
            }
            searchPos = arrPos + 1;
        }
    }

    if (arrayStart == std::string::npos) {
        brls::Logger::info("searchSubtitles: No subtitle array found in response");
        return true;
    }

    // Find the matching closing bracket
    int bracketCount = 1;
    size_t arrayEnd = arrayStart + 1;
    while (bracketCount > 0 && arrayEnd < resp.body.length()) {
        if (resp.body[arrayEnd] == '[') bracketCount++;
        else if (resp.body[arrayEnd] == ']') bracketCount--;
        arrayEnd++;
    }

    std::string metaArray = resp.body.substr(arrayStart, arrayEnd - arrayStart);

    size_t objPos = 0;
    while ((objPos = metaArray.find('{', objPos)) != std::string::npos) {
        int braceCount = 1;
        size_t objEnd = objPos + 1;
        while (braceCount > 0 && objEnd < metaArray.length()) {
            if (metaArray[objEnd] == '{') braceCount++;
            else if (metaArray[objEnd] == '}') braceCount--;
            objEnd++;
        }

        std::string obj = metaArray.substr(objPos, objEnd - objPos);

        SubtitleResult sub;
        sub.id = extractJsonInt(obj, "id");
        sub.key = extractJsonValue(obj, "key");
        sub.codec = extractJsonValue(obj, "codec");
        sub.displayTitle = extractJsonValue(obj, "displayTitle");
        sub.language = extractJsonValue(obj, "language");
        sub.languageCode = extractJsonValue(obj, "languageCode");
        sub.provider = extractJsonValue(obj, "provider");
        sub.score = extractJsonInt(obj, "score");
        sub.hearingImpaired = extractJsonBool(obj, "hearingImpaired");
        sub.forced = extractJsonBool(obj, "forced");

        // Also try alternate field names
        if (sub.displayTitle.empty()) {
            sub.displayTitle = extractJsonValue(obj, "title");
        }
        if (sub.provider.empty()) {
            sub.provider = extractJsonValue(obj, "providerTitle");
        }

        if (!sub.key.empty()) {
            results.push_back(sub);
            brls::Logger::debug("searchSubtitles: {} - {} [{}]",
                               sub.displayTitle, sub.language, sub.provider);
        }

        objPos = objEnd;
    }

    brls::Logger::info("searchSubtitles: Found {} results for ratingKey {}", results.size(), ratingKey);
    return true;
}

bool PlexClient::selectSearchedSubtitle(const std::string& ratingKey, int partId,
                                         const std::string& subtitleKey) {
    // Plex API: PUT /library/metadata/{id}/subtitles
    //   with key and partId as query parameters
    brls::Logger::debug("selectSearchedSubtitle: ratingKey={} partId={} key={}", ratingKey, partId, subtitleKey);

    HttpClient client;
    std::string endpoint = "/library/metadata/" + ratingKey + "/subtitles"
                          + "?key=" + HttpClient::urlEncode(subtitleKey)
                          + "&partId=" + std::to_string(partId);
    std::string url = buildApiUrl(endpoint);

    HttpRequest req;
    req.url = url;
    req.method = "PUT";
    HttpResponse resp = client.request(req);

    brls::Logger::debug("selectSearchedSubtitle: response status={}", resp.statusCode);

    if (resp.statusCode != 200 && resp.statusCode != 201) {
        brls::Logger::error("selectSearchedSubtitle: Failed: {}", resp.statusCode);
        return false;
    }

    brls::Logger::info("selectSearchedSubtitle: Selected subtitle key={}", subtitleKey);
    return true;
}

void PlexClient::stopTranscode() {
    if (m_lastSessionId.empty()) return;

    HttpClient client;
    std::string url = buildApiUrl("/video/:/transcode/universal/stop?session=" + m_lastSessionId);

    HttpRequest req;
    req.url = url;
    req.method = "GET";
    HttpResponse resp = client.request(req);

    brls::Logger::debug("stopTranscode: session={} status={}", m_lastSessionId, resp.statusCode);
    m_lastSessionId.clear();
}

bool PlexClient::getTranscodeUrl(const std::string& ratingKey, std::string& url, int offsetMs) {
    brls::Logger::debug("getTranscodeUrl: ratingKey={}, offsetMs={}", ratingKey, offsetMs);

    // Fetch media details to get the Part key and determine if audio or video
    HttpClient client;
    std::string apiUrl = buildApiUrl("/library/metadata/" + ratingKey);

    HttpRequest req;
    req.url = apiUrl;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    if (resp.statusCode != 200) {
        brls::Logger::error("getTranscodeUrl: Failed to fetch metadata: {}", resp.statusCode);
        return false;
    }

    // Find the Part key in the response
    size_t partPos = resp.body.find("\"Part\"");
    if (partPos == std::string::npos) {
        brls::Logger::error("getTranscodeUrl: No Part found in metadata");
        return false;
    }

    // Find the key within Part
    size_t keyPos = resp.body.find("\"key\"", partPos);
    if (keyPos == std::string::npos || keyPos > partPos + 500) {
        brls::Logger::error("getTranscodeUrl: No key found in Part");
        return false;
    }

    std::string partKey = extractJsonValue(resp.body.substr(keyPos, 200), "key");
    if (partKey.empty()) {
        brls::Logger::error("getTranscodeUrl: Part key is empty");
        return false;
    }

    brls::Logger::debug("getTranscodeUrl: partKey={}", partKey);

    // Detect if this is audio (track) or video
    bool isAudio = (resp.body.find("\"type\":\"track\"") != std::string::npos);
    brls::Logger::debug("getTranscodeUrl: isAudio={}", isAudio);

    // Per official Plex API (developer.plex.tv/pms), X-Plex-* params
    // must be sent as HTTP headers (in=header), not query params.
    // transcodeType is a path param: "video", "music", or "audio".
    std::string metadataPath = "/library/metadata/" + ratingKey;
    std::string encodedPath = HttpClient::urlEncode(metadataPath);

    // Generate a unique session ID
    char sessionBuf[32];
    snprintf(sessionBuf, sizeof(sessionBuf), "%lu", (unsigned long)time(nullptr));
    std::string sessionId = sessionBuf;

    // Build query string with query-type parameters (per official API spec)
    AppSettings& settings = Application::getInstance().getSettings();

    std::string queryParams;
    queryParams += "path=" + encodedPath;
    queryParams += "&mediaIndex=0&partIndex=0";

    // directPlay/directStream based on settings
    if (settings.directPlay && !settings.forceTranscode) {
        queryParams += "&directPlay=1&directStream=1";
    } else if (settings.forceTranscode) {
        queryParams += "&directPlay=0&directStream=0";
    } else {
        queryParams += "&directPlay=0&directStream=1";
    }
    queryParams += "&directStreamAudio=1";
    queryParams += "&hasMDE=1";
    queryParams += "&location=lan";
    queryParams += "&audioBoost=100";
    queryParams += "&audioChannelCount=2";

    char buf[256];

    if (isAudio) {
        // Audio: HTTP progressive download of mp3
        queryParams += "&protocol=http";
        queryParams += "&musicBitrate=320";
    } else {
        // Video: HLS (HTTP Live Streaming) with TS segments.
        // This matches switchfin's proven Vita configuration:
        // protocol=hls, container=mpegts, codec=h264, level<=4.0, max 720p
        queryParams += "&protocol=hls";

        const auto& vc = platform::getVideoConstraints();
        int bitrate = settings.maxBitrate > 0 ? settings.maxBitrate : vc.defaultBitrate;
        const char* resolution = vc.defaultResolution;

        snprintf(buf, sizeof(buf), "&videoBitrate=%d", bitrate);
        queryParams += buf;
        snprintf(buf, sizeof(buf), "&videoResolution=%s", resolution);
        queryParams += buf;
        queryParams += "&videoQuality=100";

        // Subtitle handling based on settings
        if (!settings.showSubtitles) {
            queryParams += "&subtitles=none";
        } else {
            queryParams += "&subtitles=auto";
        }
    }

    // Resume offset (in seconds)
    if (offsetMs > 0) {
        snprintf(buf, sizeof(buf), "&offset=%.1f", offsetMs / 1000.0);
        queryParams += buf;
    }

    // Session ID
    m_lastSessionId = sessionId;
    queryParams += "&session=" + sessionId;

    // Auth token
    queryParams += "&X-Plex-Token=" + m_authToken;

    // Profile augmentation (per official API Profile Augmentations spec).
    // Tell Plex exactly what transcode targets this client supports.
    // Without this, the Generic profile may return generalDecisionCode=2000
    // ("Neither direct play nor conversion is available").
    std::string profileExtra;
    if (isAudio) {
        // Audio: transcode to mp3 via HTTP
        profileExtra = "add-transcode-target(type=musicProfile"
                       "&context=streaming&protocol=http"
                       "&container=mp3&audioCodec=mp3)";
    } else {
        // Video: HLS with MPEG-TS segments, h264+aac.
        // Platform-specific limits for resolution and H.264 level come
        // from the platform abstraction layer instead of #defines.
        const auto& vc = platform::getVideoConstraints();
        char profileBuf[512];
        snprintf(profileBuf, sizeof(profileBuf),
            "add-transcode-target(type=videoProfile"
            "&context=streaming&protocol=hls"
            "&container=mpegts&videoCodec=h264"
            "&audioCodec=aac"
            "&subtitleCodec=srt)"
            "+add-limitation(scope=videoCodec&scopeName=h264"
            "&type=upperBound&name=video.level&value=%d)"
            "+add-limitation(scope=videoCodec&scopeName=h264"
            "&type=upperBound&name=video.width&value=%d)"
            "+add-limitation(scope=videoCodec&scopeName=h264"
            "&type=upperBound&name=video.height&value=%d)",
            vc.maxVideoLevel, vc.maxVideoWidth, vc.maxVideoHeight);
        profileExtra = profileBuf;
    }

    // Determine transcode type path segment
    const char* transcodeType = isAudio ? "music" : "video";

    // Step 1: Call /decision with X-Plex-* as HTTP headers
    snprintf(buf, sizeof(buf), "/%s/:/transcode/universal/decision?", transcodeType);
    std::string decisionUrl = m_serverUrl + buf + queryParams;
    brls::Logger::info("getTranscodeUrl: Calling decision endpoint...");

    HttpClient decisionClient;
    HttpRequest decisionReq;
    decisionReq.url = decisionUrl;
    decisionReq.method = "GET";
    decisionReq.headers["Accept"] = "application/json";
    // Per official API: X-Plex-Client-Identifier is REQUIRED, in=header
    {
        const auto& vc = platform::getVideoConstraints();
        decisionReq.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_NAME;
        decisionReq.headers["X-Plex-Product"] = PLEX_CLIENT_NAME;
        decisionReq.headers["X-Plex-Version"] = PLEX_CLIENT_VERSION;
        decisionReq.headers["X-Plex-Platform"] = vc.plexPlatform;
        decisionReq.headers["X-Plex-Device"] = vc.plexDevice;
        decisionReq.headers["X-Plex-Device-Name"] = vc.plexDevice;
    }
    decisionReq.headers["X-Plex-Client-Profile-Name"] = "Generic";
    decisionReq.headers["X-Plex-Client-Profile-Extra"] = profileExtra;
    HttpResponse decisionResp = decisionClient.request(decisionReq);

    brls::Logger::info("getTranscodeUrl: Decision response: {} body: {}",
                      decisionResp.statusCode, redactBodyForLog(decisionResp.body.substr(0, 500)));

    if (decisionResp.statusCode != 200) {
        brls::Logger::warning("getTranscodeUrl: Decision returned {}, trying start anyway",
                             decisionResp.statusCode);
    }

    // If the server chose DIRECT PLAY (the user enabled it and the file is
    // compatible), stream the original file directly. start.m3u8 is the HLS
    // transcode endpoint and 400s for a direct-play decision — you can't ask for
    // an HLS playlist of a file that's meant to be played as-is. mpv seeks the
    // file via HTTP range requests, so no offset goes in the URL; the player
    // detects the direct URL and seeks to the resume point itself.
    if (settings.directPlay && !settings.forceTranscode && !isAudio &&
        decisionResp.statusCode == 200 &&
        (decisionResp.body.find("\"decision\":\"directplay\"") != std::string::npos ||
         decisionResp.body.find("Direct play OK") != std::string::npos)) {
        url = m_serverUrl + partKey + "?X-Plex-Token=" + m_authToken;
        brls::Logger::info("getTranscodeUrl: Direct play — original file {}", partKey);
        return true;
    }

    // Step 2: Build the /start URL for MPV to stream.
    // Include X-Plex-* as query params AND MPV sends them as headers too.
    std::string startQuery = queryParams;
    {
        const auto& vc = platform::getVideoConstraints();
        startQuery += "&X-Plex-Client-Identifier=" + std::string(PLEX_CLIENT_NAME);
        startQuery += "&X-Plex-Product=" + std::string(PLEX_CLIENT_NAME);
        startQuery += "&X-Plex-Version=" + std::string(PLEX_CLIENT_VERSION);
        startQuery += "&X-Plex-Platform=" + HttpClient::urlEncode(vc.plexPlatform);
        startQuery += "&X-Plex-Device=" + HttpClient::urlEncode(vc.plexDevice);
        startQuery += "&X-Plex-Device-Name=" + HttpClient::urlEncode(vc.plexDevice);
    }
    startQuery += "&X-Plex-Client-Profile-Name=Generic";
    startQuery += "&X-Plex-Client-Profile-Extra=" + HttpClient::urlEncode(profileExtra);

    // HLS playlist for video (m3u8), mp3 for audio
    const char* container = isAudio ? "mp3" : "m3u8";
    snprintf(buf, sizeof(buf), "/%s/:/transcode/universal/start.%s?", transcodeType, container);
    url = m_serverUrl + buf + startQuery;
    brls::Logger::info("getTranscodeUrl: Transcode URL = {}", url);

    return true;
}

bool PlexClient::updatePlayProgress(const std::string& ratingKey, int timeMs) {
    HttpClient client;
    std::string url = buildApiUrl("/:/progress?key=" + ratingKey + "&time=" + std::to_string(timeMs) + "&identifier=com.plexapp.plugins.library");
    HttpResponse resp = client.get(url);
    return resp.statusCode == 200;
}

bool PlexClient::reportTimeline(const std::string& ratingKey, const std::string& key,
                                const std::string& state, int timeMs, int durationMs,
                                int playQueueItemID) {
    HttpClient client;
    std::string params = "/:/timeline?ratingKey=" + ratingKey +
        "&key=" + key +
        "&state=" + state +
        "&time=" + std::to_string(timeMs) +
        "&duration=" + std::to_string(durationMs);
    if (playQueueItemID > 0) {
        params += "&playQueueItemID=" + std::to_string(playQueueItemID);
    }
    std::string url = buildApiUrl(params);

    HttpRequest req;
    req.url = url;
    req.method = "POST";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    req.headers["X-Plex-Product"] = PLEX_CLIENT_NAME;

    HttpResponse resp = client.request(req);
    return resp.statusCode == 200;
}

bool PlexClient::reportLiveTimeline(const std::string& liveSessionUuid, int playbackTimeMs,
                                    const std::string& state) {
    if (liveSessionUuid.empty()) return false;

    // Match the official Plex app's keep-alive call: GET /:/timeline with the
    // live session path as `key=`. The server's parser fires
    //   "[Now] Updated play state for /livetv/sessions/{uuid}"
    // on receipt, which resets the rolling subscription's 300-sec stop-grab
    // timer. The server *resolves* the playing item via ratingKey first,
    // and 404s if it can't — so we must pass the live-session metadata id
    // the tune created (captured into m_lastLiveRatingKey).
    if (m_lastLiveRatingKey.empty()) {
        brls::Logger::warning("reportLiveTimeline: no live ratingKey captured; skipping keep-alive");
        return false;
    }
    std::string keyPath = "/livetv/sessions/" + liveSessionUuid;
    std::string params = "/:/timeline?key=" + HttpClient::urlEncode(keyPath) +
                         "&ratingKey=" + m_lastLiveRatingKey +
                         "&duration=0" +
                         "&time=0" +
                         "&playbackTime=" + std::to_string(playbackTimeMs) +
                         "&hasMDE=1" +
                         "&state=" + state;

    HttpRequest req;
    req.url = buildApiUrl(params);
    req.method = "GET";
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    req.headers["X-Plex-Product"] = PLEX_CLIENT_NAME;
    // Bind the timeline to the same consumer the tune/decision used so the
    // server attributes it to the right rolling subscription.
    req.headers["X-Plex-Session-Identifier"] = PLEX_CLIENT_ID;
    req.timeout = 10;

    HttpClient client;
    HttpResponse resp = client.request(req);
    if (resp.statusCode != 200) {
        brls::Logger::debug("reportLiveTimeline: status={} for /livetv/sessions/{}",
                            resp.statusCode, liveSessionUuid);
    }
    return resp.statusCode == 200;
}

bool PlexClient::markAsWatched(const std::string& ratingKey) {
    HttpClient client;
    std::string url = buildApiUrl("/:/scrobble?key=" + ratingKey + "&identifier=com.plexapp.plugins.library");
    HttpResponse resp = client.get(url);
    return resp.statusCode == 200;
}

bool PlexClient::markAsUnwatched(const std::string& ratingKey) {
    HttpClient client;
    std::string url = buildApiUrl("/:/unscrobble?key=" + ratingKey + "&identifier=com.plexapp.plugins.library");
    HttpResponse resp = client.get(url);
    return resp.statusCode == 200;
}

void PlexClient::checkLiveTVAvailability() {
    // Official Plex API: GET /livetv/dvrs
    // Returns DVR list with key, lineup, uuid, Device array, and ChannelMapping
    HttpClient client;
    std::string url = buildApiUrl("/livetv/dvrs");
    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    req.timeout = 10;

    HttpResponse resp = client.request(req);

    m_hasLiveTV = (resp.statusCode == 200);

    if (m_hasLiveTV && !resp.body.empty()) {
        brls::Logger::debug("DVR response (first 1000): {}",
                            resp.body.substr(0, 1000));

        // Parse DVR key - the "key" field is the DVR ID (e.g., "28")
        // Per openapi.json example: "key": "28"
        std::string key = extractJsonValue(resp.body, "key");
        if (!key.empty()) {
            // Key may be just a number like "28" or a path like "/livetv/dvrs/28"
            size_t lastSlash = key.rfind('/');
            if (lastSlash != std::string::npos) {
                m_dvrId = key.substr(lastSlash + 1);
            } else {
                m_dvrId = key;
            }
        }
        brls::Logger::info("Live TV DVR ID: {}", m_dvrId.empty() ? "(none)" : m_dvrId);

        // Parse lineup URI from DVR response
        // Per openapi.json: "lineup": "lineup://tv.plex.providers.epg.onconnect/USA-HI51418-X"
        m_lineupUri = extractJsonValue(resp.body, "lineup");
        if (!m_lineupUri.empty()) {
            brls::Logger::info("Live TV Lineup URI: {}", m_lineupUri);
        }

        // Parse Device array for device UUIDs
        // Per openapi.json: Device items have "uuid" like "device://tv.plex.grabbers.hdhomerun/1053C0CA"
        m_deviceIds.clear();
        size_t pos = 0;
        while ((pos = resp.body.find("\"uuid\"", pos)) != std::string::npos) {
            std::string uuid = extractJsonValue(resp.body.substr(pos), "uuid");
            if (!uuid.empty() && uuid.find("device://") != std::string::npos) {
                bool found = false;
                for (const auto& d : m_deviceIds) {
                    if (d == uuid) { found = true; break; }
                }
                if (!found) {
                    m_deviceIds.push_back(uuid);
                    brls::Logger::info("Live TV Device UUID: {}", uuid);
                }
            }
            pos++;
        }

        // Parse ChannelMapping to get available channel identifiers
        // Per openapi.json: "channelKey", "deviceIdentifier", "enabled", "lineupIdentifier"
        m_channelMappings.clear();
        pos = 0;
        while ((pos = resp.body.find("\"channelKey\"", pos)) != std::string::npos) {
            std::string region = resp.body.substr(pos, std::min((size_t)300, resp.body.length() - pos));
            std::string channelKey = extractJsonValue(region, "channelKey");
            std::string deviceId = extractJsonValue(region, "deviceIdentifier");
            std::string enabled = extractJsonValue(region, "enabled");

            if (!channelKey.empty() && !deviceId.empty() && enabled != "0") {
                ChannelMapping mapping;
                mapping.channelKey = channelKey;
                mapping.deviceIdentifier = deviceId;
                mapping.lineupIdentifier = extractJsonValue(region, "lineupIdentifier");
                m_channelMappings.push_back(mapping);
            }
            pos++;
        }
        brls::Logger::info("Live TV: Found {} channel mappings", m_channelMappings.size());

        // Extract EPG provider key from the DVR response's "epgIdentifier" field
        // This is the full provider key including DVR-specific suffix (e.g., "tv.plex.providers.epg.cloud:40")
        // The grid endpoint uses this as: GET /{epgIdentifier}/grid
        if (m_epgProviderKey.empty()) {
            m_epgProviderKey = extractJsonValue(resp.body, "epgIdentifier");
            if (!m_epgProviderKey.empty()) {
                brls::Logger::info("Live TV EPG provider key (from epgIdentifier): {}", m_epgProviderKey);
            }
        }

        // Fallback: derive from lineup URI if epgIdentifier not found
        if (m_epgProviderKey.empty() && !m_lineupUri.empty()) {
            size_t protoEnd = m_lineupUri.find("://");
            if (protoEnd != std::string::npos) {
                size_t hostStart = protoEnd + 3;
                size_t hostEnd = m_lineupUri.find('/', hostStart);
                if (hostEnd != std::string::npos) {
                    m_epgProviderKey = m_lineupUri.substr(hostStart, hostEnd - hostStart);
                } else {
                    m_epgProviderKey = m_lineupUri.substr(hostStart);
                }
            }
            if (!m_epgProviderKey.empty()) {
                brls::Logger::info("Live TV EPG provider key (from lineup URI): {}", m_epgProviderKey);
            }
        }
    }

    brls::Logger::info("Live TV availability check: {} (dvr: {}, devices: {}, lineup: {}, mappings: {}, epg: {})",
                        m_hasLiveTV ? "available" : "not available",
                        m_dvrId.empty() ? "(none)" : m_dvrId,
                        m_deviceIds.size(),
                        m_lineupUri.empty() ? "(none)" : "set",
                        m_channelMappings.size(),
                        m_epgProviderKey.empty() ? "(none)" : m_epgProviderKey);
}

bool PlexClient::fetchLiveTVChannels(std::vector<LiveTVChannel>& channels) {
    HttpClient client;

    // Ensure DVR info is loaded
    if (m_dvrId.empty()) {
        checkLiveTVAvailability();
    }

    channels.clear();

    HttpRequest req;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    req.timeout = 15;

    // Official API: GET /livetv/epg/channels?lineup={lineupUri}
    // Returns Channel array with: callSign, identifier, channelVcn, hd, thumb, title, key
    if (!m_lineupUri.empty()) {
        std::string url = buildApiUrl("/livetv/epg/channels");
        url += "&lineup=" + HttpClient::urlEncode(m_lineupUri);
        req.url = url;
        brls::Logger::debug("fetchLiveTVChannels: GET /livetv/epg/channels?lineup={}", m_lineupUri);

        // Cache the channels list — call signs, channel numbers, and
        // station logos almost never change. This is the body the user
        // specifically called out as worth caching ("EPG channel number
        // and logo stay relatively consistent"). Programs themselves
        // come from fetchEPGGrid below and aren't cached.
        const int ttlSec = Application::getInstance().getSettings().cacheLifetimeMinutes * 60;
        std::string respBody;
        bool fromCache = HttpCache::get(url, ttlSec, respBody);
        int statusCode = 200;
        if (!fromCache) {
            HttpResponse resp = client.request(req);
            statusCode = resp.statusCode;
            respBody = std::move(resp.body);
            if (statusCode == 200 && !respBody.empty()) {
                HttpCache::put(url, respBody, ttlSec);
            }
        }
        // Shim — the rest of this block was written against `resp.body`
        // and `resp.statusCode`. Re-expose them as a local without
        // touching the parser.
        struct { int statusCode; std::string body; } resp{statusCode, std::move(respBody)};
        if (resp.statusCode == 200 && !resp.body.empty()) {
            brls::Logger::debug("fetchLiveTVChannels: EPG channels response ({} bytes, first 500): {}",
                                resp.body.length(), resp.body.substr(0, 500));

            // Parse Channel array from response
            // Per openapi.json schema: Channel objects have callSign, identifier, channelVcn, thumb, title, key
            //
            // The response can reference a single channel from more than
            // one section of the JSON — once as the lineup's Channel
            // object and again as a ChannelMapping's nested copy — and
            // our "find every \"callSign\" then walk back to its
            // enclosing object" parser catches all of them. Dedupe by
            // channel.key (or VCN as a fallback for entries without a
            // key) before pushing so the EPG doesn't show double rows
            // for 4.1, 4.2, 4.3 etc.
            std::set<std::string> seenChannelKeys;
            size_t pos = 0;
            while ((pos = resp.body.find("\"callSign\"", pos)) != std::string::npos) {
                size_t objStart = resp.body.rfind('{', pos);
                if (objStart == std::string::npos) { pos++; continue; }

                int braceCount = 1;
                size_t objEnd = objStart + 1;
                while (braceCount > 0 && objEnd < resp.body.length()) {
                    if (resp.body[objEnd] == '{') braceCount++;
                    else if (resp.body[objEnd] == '}') braceCount--;
                    objEnd++;
                }

                std::string obj = resp.body.substr(objStart, objEnd - objStart);

                LiveTVChannel channel;
                channel.callSign = extractJsonValue(obj, "callSign");
                channel.key = extractJsonValue(obj, "key");
                channel.title = extractJsonValue(obj, "title");
                if (channel.title.empty()) {
                    channel.title = channel.callSign;
                }
                channel.thumb = extractJsonValue(obj, "thumb");

                // channelVcn is the virtual channel number like "2.1"
                std::string vcn = extractJsonValue(obj, "channelVcn");
                if (!vcn.empty()) {
                    channel.channelIdentifier = vcn;
                    size_t dotPos = vcn.find('.');
                    if (dotPos != std::string::npos) {
                        int major = atoi(vcn.substr(0, dotPos).c_str());
                        int minor = atoi(vcn.substr(dotPos + 1).c_str());
                        channel.channelNumber = major * 10 + minor;
                    } else {
                        channel.channelNumber = atoi(vcn.c_str()) * 10;
                    }
                }

                // identifier field from EPG
                std::string identifier = extractJsonValue(obj, "identifier");
                if (!identifier.empty() && channel.channelIdentifier.empty()) {
                    channel.channelIdentifier = identifier;
                }

                // Cross-reference with ChannelMapping to (a) get the
                // device identifier for tuning and (b) drop channels
                // that aren't actually mapped to a tuner on this
                // server. Without (b), the EPG was showing every
                // channel the lineup *covers* (~106 on a typical
                // OTA list) rather than the ones the user's DVR is
                // set up to receive (~32 here), so the grid was full
                // of "No guide data" rows for channels the user
                // can't tune anyway.
                const ChannelMapping* matchedMapping = nullptr;
                for (const auto& mapping : m_channelMappings) {
                    if (!channel.key.empty() && channel.key == mapping.channelKey) {
                        matchedMapping = &mapping;
                        break;
                    }
                    if (!identifier.empty() && identifier == mapping.lineupIdentifier) {
                        matchedMapping = &mapping;
                        if (channel.key.empty()) channel.key = mapping.channelKey;
                        break;
                    }
                }
                if (matchedMapping) {
                    channel.channelIdentifier = matchedMapping->deviceIdentifier;
                }

                // Only include channels the user can actually tune.
                // If we have no ChannelMapping data at all (some EPG
                // providers don't), fall back to the old behaviour so
                // the EPG isn't empty.
                if (matchedMapping || m_channelMappings.empty()) {
                    if (!channel.callSign.empty() || !channel.title.empty()) {
                        // Dedupe by the mapping's channelKey when one
                        // matched — two lineup entries can resolve to
                        // the *same* physical channel via different
                        // identifiers (lineup key vs lineupIdentifier),
                        // so keying off channel.key alone wasn't enough
                        // and the EPG was still showing duplicates of
                        // 4.1 / 4.2 / 4.3, 22.3 / 22.4, etc.
                        std::string dedupeKey = matchedMapping
                            ? matchedMapping->channelKey
                            : (!channel.key.empty()
                                ? channel.key
                                : (!channel.channelIdentifier.empty()
                                    ? channel.channelIdentifier
                                    : channel.callSign));
                        if (seenChannelKeys.insert(dedupeKey).second) {
                            channels.push_back(channel);
                        }
                    }
                }

                pos = objEnd;
            }

            if (!channels.empty()) {
                brls::Logger::info("fetchLiveTVChannels: Found {} channels from EPG lineup", channels.size());
            }
        }
    }

    // Fallback: GET /livetv/dvrs/{dvrId} to get ChannelMapping and build channel list
    if (channels.empty() && !m_dvrId.empty()) {
        std::string url = buildApiUrl("/livetv/dvrs/" + m_dvrId);
        req.url = url;
        brls::Logger::debug("fetchLiveTVChannels: GET /livetv/dvrs/{}", m_dvrId);

        HttpResponse resp = client.request(req);
        if (resp.statusCode == 200 && !resp.body.empty()) {
            brls::Logger::debug("fetchLiveTVChannels: DVR response ({} bytes, first 500): {}",
                                resp.body.length(), resp.body.substr(0, 500));

            // Build channels from ChannelMapping entries
            size_t pos = 0;
            while ((pos = resp.body.find("\"deviceIdentifier\"", pos)) != std::string::npos) {
                size_t objStart = resp.body.rfind('{', pos);
                if (objStart == std::string::npos) { pos++; continue; }

                int braceCount = 1;
                size_t objEnd = objStart + 1;
                while (braceCount > 0 && objEnd < resp.body.length()) {
                    if (resp.body[objEnd] == '{') braceCount++;
                    else if (resp.body[objEnd] == '}') braceCount--;
                    objEnd++;
                }

                std::string obj = resp.body.substr(objStart, objEnd - objStart);

                std::string deviceId = extractJsonValue(obj, "deviceIdentifier");
                std::string channelKey = extractJsonValue(obj, "channelKey");
                std::string enabled = extractJsonValue(obj, "enabled");

                if (!deviceId.empty() && enabled != "0") {
                    LiveTVChannel channel;
                    channel.channelIdentifier = deviceId;
                    channel.key = channelKey;
                    channel.title = "Ch " + deviceId;
                    channel.callSign = deviceId;

                    // Parse deviceIdentifier as channel number (e.g. "48.1")
                    size_t dotPos = deviceId.find('.');
                    if (dotPos != std::string::npos) {
                        int major = atoi(deviceId.substr(0, dotPos).c_str());
                        int minor = atoi(deviceId.substr(dotPos + 1).c_str());
                        channel.channelNumber = major * 10 + minor;
                    } else {
                        channel.channelNumber = atoi(deviceId.c_str()) * 10;
                    }

                    channels.push_back(channel);
                }

                pos = objEnd;
            }

            if (!channels.empty()) {
                brls::Logger::info("fetchLiveTVChannels: Found {} channels from DVR ChannelMapping", channels.size());
            }
        }
    }

    // Sort by channel number
    std::sort(channels.begin(), channels.end(),
              [](const LiveTVChannel& a, const LiveTVChannel& b) {
                  return a.channelNumber < b.channelNumber;
              });

    brls::Logger::info("fetchLiveTVChannels: Found {} channels total", channels.size());
    return true;
}

bool PlexClient::fetchEPGGrid(std::vector<LiveTVChannel>& channelsWithPrograms, int hoursAhead) {
    brls::Logger::debug("fetchEPGGrid: fetching {} hours of programming", hoursAhead);

    // Ensure DVR info is loaded
    if (m_dvrId.empty()) {
        checkLiveTVAvailability();
    }

    // First get channel list via official API
    if (!fetchLiveTVChannels(channelsWithPrograms)) {
        return false;
    }

    if (channelsWithPrograms.empty()) {
        return false;
    }

    // Get current time for grid query
    time_t now = time(nullptr);
    time_t endTime = now + (hoursAhead * 3600);

    HttpClient client;
    HttpRequest req;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    req.timeout = 30;

    // Use EPG provider grid endpoint for program data
    // The provider key comes from the lineup URI (e.g., "tv.plex.providers.epg.onconnect")
    // Grid endpoint: GET /{epgProviderKey}/grid?type={type}&beginsAt<={end}&endsAt>={now}
    bool gotProgramData = false;

    if (!m_epgProviderKey.empty()) {
        // Query grid for TV shows (type=4) and movies (type=1)
        for (int gridType : {4, 1}) {
            std::string gridUrl = buildApiUrl("/" + m_epgProviderKey + "/grid");
            gridUrl += "&type=" + std::to_string(gridType);
            gridUrl += "&beginsAt%3C=" + std::to_string(endTime);
            gridUrl += "&endsAt%3E=" + std::to_string(now);
            req.url = gridUrl;

            brls::Logger::debug("fetchEPGGrid: Trying grid endpoint: {}", gridUrl);
            HttpResponse resp = client.request(req);
            if (resp.statusCode == 200 && !resp.body.empty()) {
                brls::Logger::debug("fetchEPGGrid: Grid response ({} bytes, type={}), first 1000: {}",
                                    resp.body.length(), gridType, resp.body.substr(0, 1000));

                // Parse Metadata array containing program entries
                size_t metaArrayPos = resp.body.find("\"Metadata\"");
                if (metaArrayPos == std::string::npos) continue;

                size_t arrayStart = resp.body.find('[', metaArrayPos);
                if (arrayStart == std::string::npos) continue;

                // Iterate through Metadata objects
                size_t pos = arrayStart + 1;
                while (pos < resp.body.length()) {
                    // Skip whitespace and commas
                    while (pos < resp.body.length() && (resp.body[pos] == ' ' || resp.body[pos] == ',' ||
                           resp.body[pos] == '\n' || resp.body[pos] == '\r' || resp.body[pos] == '\t')) {
                        pos++;
                    }
                    if (pos >= resp.body.length() || resp.body[pos] == ']') break;
                    if (resp.body[pos] != '{') { pos++; continue; }

                    // Extract Metadata object
                    size_t objStart = pos;
                    int braceCount = 1;
                    pos++;
                    while (braceCount > 0 && pos < resp.body.length()) {
                        if (resp.body[pos] == '{') braceCount++;
                        else if (resp.body[pos] == '}') braceCount--;
                        pos++;
                    }
                    std::string metaObj = resp.body.substr(objStart, pos - objStart);

                    // Extract program title
                    std::string progTitle = extractJsonValue(metaObj, "title");
                    std::string grandparentTitle = extractJsonValue(metaObj, "grandparentTitle");
                    if (progTitle.empty()) continue;

                    std::string displayTitle;
                    if (!grandparentTitle.empty() && gridType == 4) {
                        displayTitle = grandparentTitle + ": " + progTitle;
                    } else {
                        displayTitle = progTitle;
                    }

                    std::string progRatingKey = extractJsonValue(metaObj, "ratingKey");
                    std::string progMetadataKey = extractJsonValue(metaObj, "key");
                    // Pull summary + thumb so the Live TV hero can show the show's
                    // description and poster, not just the title.
                    std::string progSummary = extractJsonValue(metaObj, "summary");
                    std::string progThumb = extractJsonValue(metaObj, "thumb");
                    if (progThumb.empty()) progThumb = extractJsonValue(metaObj, "grandparentThumb");
                    if (progThumb.empty()) progThumb = extractJsonValue(metaObj, "parentThumb");
                    if (progThumb.empty()) progThumb = extractJsonValue(metaObj, "art");

                    // Parse Media array for channel + timing info
                    size_t mediaPos = metaObj.find("\"Media\"");
                    if (mediaPos == std::string::npos) continue;

                    size_t mediaArrayStart = metaObj.find('[', mediaPos);
                    if (mediaArrayStart == std::string::npos) continue;

                    size_t mPos = mediaArrayStart + 1;
                    while (mPos < metaObj.length()) {
                        size_t mObjStart = metaObj.find('{', mPos);
                        if (mObjStart == std::string::npos || mObjStart >= metaObj.length()) break;

                        int mBraceCount = 1;
                        size_t mObjEnd = mObjStart + 1;
                        while (mBraceCount > 0 && mObjEnd < metaObj.length()) {
                            if (metaObj[mObjEnd] == '{') mBraceCount++;
                            else if (metaObj[mObjEnd] == '}') mBraceCount--;
                            mObjEnd++;
                        }
                        std::string mediaObj = metaObj.substr(mObjStart, mObjEnd - mObjStart);
                        mPos = mObjEnd;

                        std::string beginsAtStr = extractJsonValue(mediaObj, "beginsAt");
                        std::string endsAtStr = extractJsonValue(mediaObj, "endsAt");
                        if (beginsAtStr.empty() || endsAtStr.empty()) continue;

                        int64_t progStart = atoll(beginsAtStr.c_str());
                        int64_t progEnd = atoll(endsAtStr.c_str());

                        if (progEnd < (int64_t)now) continue;

                        // Match to channel list using callSign, channelIdentifier, VCN, or key
                        std::string chanCallSign = extractJsonValue(mediaObj, "channelCallSign");
                        std::string chanId = extractJsonValue(mediaObj, "channelIdentifier");
                        std::string chanVcn = extractJsonValue(mediaObj, "channelVcn");
                        std::string chanTitle = extractJsonValue(mediaObj, "channelTitle");
                        std::string chanShortTitle = extractJsonValue(mediaObj, "channelShortTitle");

                        for (auto& channel : channelsWithPrograms) {
                            bool matched = false;

                            // Match by channelIdentifier == channel.key
                            if (!matched && !chanId.empty() && !channel.key.empty()) {
                                if (chanId == channel.key) matched = true;
                            }

                            // Match by VCN == channelIdentifier
                            if (!matched && !chanVcn.empty() && !channel.channelIdentifier.empty()) {
                                if (chanVcn == channel.channelIdentifier) matched = true;
                            }

                            // Match by exact callSign
                            if (!matched && !chanCallSign.empty() && !channel.callSign.empty()) {
                                if (chanCallSign == channel.callSign) matched = true;
                            }

                            // Match by channel title
                            if (!matched && !channel.title.empty()) {
                                if ((!chanShortTitle.empty() && chanShortTitle == channel.title) ||
                                    (!chanTitle.empty() && chanTitle == channel.title)) {
                                    matched = true;
                                }
                            }

                            if (matched) {
                                ChannelProgram prog;
                                prog.title = displayTitle;
                                prog.startTime = progStart;
                                prog.endTime = progEnd;
                                prog.ratingKey = progRatingKey;
                                prog.metadataKey = progMetadataKey;
                                prog.summary = progSummary;
                                prog.thumb = progThumb;

                                // Avoid duplicate programs
                                bool duplicate = false;
                                for (const auto& existing : channel.programs) {
                                    if (existing.startTime == progStart && existing.title == displayTitle) {
                                        duplicate = true;
                                        break;
                                    }
                                }
                                if (!duplicate) {
                                    channel.programs.push_back(prog);
                                    gotProgramData = true;
                                }

                                // Populate current/next program fields
                                if (progStart <= (int64_t)now && progEnd > (int64_t)now) {
                                    if (channel.currentProgram.empty()) {
                                        channel.currentProgram = displayTitle;
                                        channel.programStart = progStart;
                                        channel.programEnd = progEnd;
                                    }
                                } else if (progStart >= (int64_t)now) {
                                    if (channel.nextProgram.empty()) {
                                        channel.nextProgram = displayTitle;
                                    }
                                }
                                break;
                            }
                        }

                        size_t nextComma = metaObj.find_first_of(",]", mPos);
                        if (nextComma != std::string::npos && metaObj[nextComma] == ']') break;
                    }
                }
            } else {
                brls::Logger::debug("fetchEPGGrid: Grid endpoint returned {} for type={}",
                                    resp.statusCode, gridType);
            }
        }
    }

    // Per-channel grid. Mirrors what the official Plex apps do — they
    // skip the type-filtered grid entirely and just query each channel's
    // grid for the day:
    //   GET /{provider}/grid?channelGridKey=<key>&date=YYYY-MM-DD
    // which returns *every* airing for that channel regardless of EPG
    // type. The type-filtered loop above only catches movies (1) and
    // episodes (4); sports, news, and talk-shows tagged with other
    // types would otherwise show up empty. Running this for every
    // channel ensures parity with the official app.
    if (!m_epgProviderKey.empty()) {
        // Build the list of calendar dates the lookahead window spans.
        // The per-channel grid is keyed by `date=YYYY-MM-DD`, so a 12h
        // window starting after noon will need both today *and*
        // tomorrow to cover the whole range; querying only today drops
        // everything past midnight.
        auto formatLocalDate = [](time_t t) {
            struct tm* lt = localtime(&t);
            char buf[16];
            strftime(buf, sizeof(buf), "%Y-%m-%d", lt);
            return std::string(buf);
        };
        std::vector<std::string> dates;
        {
            std::string lastDate;
            for (time_t t = (time_t)now; t <= (time_t)endTime; t += 12 * 3600) {
                std::string d = formatLocalDate(t);
                if (d != lastDate) { dates.push_back(d); lastDate = d; }
            }
            std::string endDate = formatLocalDate((time_t)endTime);
            if (dates.empty() || dates.back() != endDate) dates.push_back(endDate);
        }

        for (auto& channel : channelsWithPrograms) {
            if (channel.key.empty()) continue;

            for (const std::string& date : dates) {
                std::string url = buildApiUrl("/" + m_epgProviderKey + "/grid");
                url += "&channelGridKey=" + HttpClient::urlEncode(channel.key);
                url += "&date=" + date;
                req.url = url;
                HttpResponse resp = client.request(req);
                if (resp.statusCode != 200 || resp.body.empty()) continue;

            size_t metaArrayPos = resp.body.find("\"Metadata\"");
            if (metaArrayPos == std::string::npos) continue;
            size_t arrayStart = resp.body.find('[', metaArrayPos);
            if (arrayStart == std::string::npos) continue;

            size_t pos = arrayStart + 1;
            while (pos < resp.body.length()) {
                while (pos < resp.body.length() && (resp.body[pos] == ' ' || resp.body[pos] == ',' ||
                       resp.body[pos] == '\n' || resp.body[pos] == '\r' || resp.body[pos] == '\t')) {
                    pos++;
                }
                if (pos >= resp.body.length() || resp.body[pos] == ']') break;
                if (resp.body[pos] != '{') { pos++; continue; }

                size_t objStart = pos;
                int braceCount = 1;
                pos++;
                while (braceCount > 0 && pos < resp.body.length()) {
                    if (resp.body[pos] == '{') braceCount++;
                    else if (resp.body[pos] == '}') braceCount--;
                    pos++;
                }
                std::string metaObj = resp.body.substr(objStart, pos - objStart);

                std::string progTitle = extractJsonValue(metaObj, "title");
                if (progTitle.empty()) continue;
                std::string grandparentTitle = extractJsonValue(metaObj, "grandparentTitle");
                std::string displayTitle = (!grandparentTitle.empty())
                    ? grandparentTitle + ": " + progTitle : progTitle;

                std::string progRatingKey   = extractJsonValue(metaObj, "ratingKey");
                std::string progMetadataKey = extractJsonValue(metaObj, "key");
                std::string progSummary     = extractJsonValue(metaObj, "summary");
                std::string progThumb       = extractJsonValue(metaObj, "thumb");
                if (progThumb.empty()) progThumb = extractJsonValue(metaObj, "grandparentThumb");
                if (progThumb.empty()) progThumb = extractJsonValue(metaObj, "parentThumb");
                if (progThumb.empty()) progThumb = extractJsonValue(metaObj, "art");

                size_t mediaPos = metaObj.find("\"Media\"");
                if (mediaPos == std::string::npos) continue;
                size_t mediaArrayStart = metaObj.find('[', mediaPos);
                if (mediaArrayStart == std::string::npos) continue;

                size_t mPos = mediaArrayStart + 1;
                while (mPos < metaObj.length()) {
                    size_t mObjStart = metaObj.find('{', mPos);
                    if (mObjStart == std::string::npos || mObjStart >= metaObj.length()) break;

                    int mBraceCount = 1;
                    size_t mObjEnd = mObjStart + 1;
                    while (mBraceCount > 0 && mObjEnd < metaObj.length()) {
                        if (metaObj[mObjEnd] == '{') mBraceCount++;
                        else if (metaObj[mObjEnd] == '}') mBraceCount--;
                        mObjEnd++;
                    }
                    std::string mediaObj = metaObj.substr(mObjStart, mObjEnd - mObjStart);
                    mPos = mObjEnd;

                    std::string beginsAtStr = extractJsonValue(mediaObj, "beginsAt");
                    std::string endsAtStr   = extractJsonValue(mediaObj, "endsAt");
                    if (beginsAtStr.empty() || endsAtStr.empty()) continue;

                    int64_t progStart = atoll(beginsAtStr.c_str());
                    int64_t progEnd   = atoll(endsAtStr.c_str());
                    if (progEnd < (int64_t)now) continue;

                    // Channel is fixed by the channelGridKey query, so no
                    // cross-matching needed.
                    bool duplicate = false;
                    for (const auto& existing : channel.programs) {
                        if (existing.startTime == progStart && existing.title == displayTitle) {
                            duplicate = true; break;
                        }
                    }
                    if (!duplicate) {
                        ChannelProgram prog;
                        prog.title       = displayTitle;
                        prog.startTime   = progStart;
                        prog.endTime     = progEnd;
                        prog.ratingKey   = progRatingKey;
                        prog.metadataKey = progMetadataKey;
                        prog.summary     = progSummary;
                        prog.thumb       = progThumb;
                        channel.programs.push_back(prog);
                        gotProgramData = true;
                    }

                    if (progStart <= (int64_t)now && progEnd > (int64_t)now) {
                        if (channel.currentProgram.empty()) {
                            channel.currentProgram = displayTitle;
                            channel.programStart   = progStart;
                            channel.programEnd     = progEnd;
                        }
                    } else if (progStart >= (int64_t)now) {
                        if (channel.nextProgram.empty()) channel.nextProgram = displayTitle;
                    }

                    size_t nextComma = metaObj.find_first_of(",]", mPos);
                    if (nextComma != std::string::npos && metaObj[nextComma] == ']') break;
                }
            }
            }  // per-date loop
        }
    }

    // Sort each channel's programs by start time
    int programCount = 0;
    for (auto& ch : channelsWithPrograms) {
        if (!ch.programs.empty()) {
            std::sort(ch.programs.begin(), ch.programs.end(),
                      [](const ChannelProgram& a, const ChannelProgram& b) {
                          return a.startTime < b.startTime;
                      });
            programCount++;
        }
    }
    brls::Logger::info("fetchEPGGrid: Got {} channels, {} with program info", channelsWithPrograms.size(), programCount);
    return !channelsWithPrograms.empty();
}

bool PlexClient::tuneLiveTVChannel(const std::string& channelKey, std::string& streamUrl,
                                   std::string& liveSessionUuid,
                                   const std::string& programMetadataKey) {
    liveSessionUuid.clear();
    brls::Logger::info("tuneLiveTVChannel: channelKey={}, programMetadataKey={}", channelKey, programMetadataKey);

    // Ensure we have DVR ID
    if (m_dvrId.empty()) {
        checkLiveTVAvailability();
        if (m_dvrId.empty()) {
            brls::Logger::error("tuneLiveTVChannel: No DVR ID available");
            return false;
        }
    }

    HttpClient client;

    // Official API: POST /livetv/dvrs/{dvrId}/channels/{channel}/tune
    // {channel} must be the full EPG channel key (<lineup>-<channelId>) that the
    // device's channel map is keyed by; the VCN ("2.1") shown in the spec example
    // is not recognized by the grabber and yields "device does not tune" errors.
    // Returns Media with uuid (session ID) which can be used for HLS streaming
    std::string tuneUrl = buildApiUrl("/livetv/dvrs/" + m_dvrId + "/channels/" + channelKey + "/tune");

    HttpRequest tuneReq;
    tuneReq.url = tuneUrl;
    tuneReq.method = "POST";
    tuneReq.headers["Accept"] = "application/json";
    tuneReq.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    tuneReq.headers["X-Plex-Product"] = PLEX_CLIENT_NAME;
    // Name the rolling-subscription session deterministically so the transcode
    // decision (which passes the same X-Plex-Session-Identifier) resolves to
    // this grab's consumer at /livetv/sessions/{uuid}/{sessionIdentifier}/...
    tuneReq.headers["X-Plex-Session-Identifier"] = PLEX_CLIENT_ID;
    // A successful tune starts a long-lived "rolling subscription": the server
    // sends the MediaContainer (with the Media/session uuid we need) up front
    // but then holds the chunked HTTP response open for the life of the
    // recording (~5 min).  Reading to EOF therefore blocks until the request
    // times out.  Stop as soon as the first complete top-level JSON object is
    // received - that already contains everything we need.
    tuneReq.stopAtJsonClose = true;
    // Use longer timeout - tune can take time before it produces the metadata.
    tuneReq.timeout = 30;

    brls::Logger::debug("tuneLiveTVChannel: POST {}", tuneUrl);
    HttpResponse tuneResp = client.request(tuneReq);

    // programMetadataKey is no longer used for playback (the live session itself
    // is the source); kept in the signature for callers. Reference it to avoid
    // an unused-parameter warning.
    (void)programMetadataKey;

    std::string sessionUuid;
    if (tuneResp.statusCode == 200 && !tuneResp.body.empty()) {
        brls::Logger::debug("tuneLiveTVChannel: Tune response ({} bytes): {}",
                            tuneResp.body.length(), tuneResp.body.substr(0, 500));

        // Server returns 200 but with {"MediaContainer":{"status":-1,"message":"Could not tune..."}}
        // when the tune genuinely fails (no tuner/antenna).
        std::string tuneStatus = extractJsonValue(tuneResp.body, "status");
        std::string tuneMessage = extractJsonValue(tuneResp.body, "message");
        if (tuneStatus == "-1" || tuneResp.body.find("Could not tune") != std::string::npos) {
            brls::Logger::warning("tuneLiveTVChannel: Server reported tune failure: {}", tuneMessage);
            return false;
        }

        // The Media uuid is the live session id used as /livetv/sessions/{uuid}.
        sessionUuid = extractJsonValue(tuneResp.body, "uuid");

        // The first numeric ratingKey in the tune response is the live-session
        // metadata item the server just created (e.g. "Added new metadata item
        // (Live Session ...) with ID 17594"). /:/timeline needs this to
        // resolve the playing item — without it the call 404s and the keep-
        // alive never resets the rolling-subscription stop-grab timer.
        // EPG/program ratingKeys are URL-encoded strings ("plex%3A%2F%2F..."),
        // so we skip non-numeric matches when scanning.
        m_lastLiveRatingKey.clear();
        {
            size_t scan = 0;
            const std::string needle = "\"ratingKey\"";
            while (true) {
                size_t at = tuneResp.body.find(needle, scan);
                if (at == std::string::npos) break;
                size_t colon = tuneResp.body.find(':', at);
                if (colon == std::string::npos) break;
                size_t vs = tuneResp.body.find_first_not_of(" \t\n\r\"", colon + 1);
                if (vs == std::string::npos) break;
                if (tuneResp.body[vs] >= '0' && tuneResp.body[vs] <= '9') {
                    size_t ve = vs;
                    while (ve < tuneResp.body.length() &&
                           tuneResp.body[ve] >= '0' && tuneResp.body[ve] <= '9') ve++;
                    m_lastLiveRatingKey = tuneResp.body.substr(vs, ve - vs);
                    break;
                }
                scan = at + needle.length();
            }
        }
        brls::Logger::debug("tuneLiveTVChannel: live ratingKey = {}",
                            m_lastLiveRatingKey.empty() ? "(none)" : m_lastLiveRatingKey);
    } else if (tuneResp.statusCode == 0 || tuneResp.statusCode == -1) {
        // Connection drop / partial read - try to recover the uuid if present.
        brls::Logger::warning("tuneLiveTVChannel: Connection dropped (status {}), body so far ({} bytes): {}",
                              tuneResp.statusCode, tuneResp.body.length(),
                              tuneResp.body.empty() ? "(empty)" : tuneResp.body.substr(0, 300));
        if (!tuneResp.body.empty()) {
            sessionUuid = extractJsonValue(tuneResp.body, "uuid");
        }
    } else if (tuneResp.statusCode == 500) {
        brls::Logger::error("tuneLiveTVChannel: Tune failed (500 - server error) for channel {}", channelKey);
        return false;
    } else {
        brls::Logger::error("tuneLiveTVChannel: Tune returned {} for channel {}, body: {}",
                            tuneResp.statusCode, channelKey,
                            tuneResp.body.empty() ? "(empty)" : redactBodyForLog(tuneResp.body.substr(0, 200)));
        return false;
    }

    if (sessionUuid.empty()) {
        brls::Logger::error("tuneLiveTVChannel: No session UUID in tune response for channel {}", channelKey);
        return false;
    }

    brls::Logger::info("tuneLiveTVChannel: Live session id = {}", sessionUuid);

    // The raw tune session is mpeg2video; route it through transcode/universal
    // (exactly as the official Plex app does) to get a playable h264 HLS URL.
    if (buildLiveSessionStreamUrl(sessionUuid, streamUrl)) {
        liveSessionUuid = sessionUuid;
        return true;
    }

    brls::Logger::error("tuneLiveTVChannel: Failed to build stream URL for channel {}", channelKey);
    return false;
}

bool PlexClient::buildLiveSessionStreamUrl(const std::string& liveSessionId, std::string& url) {
    // Mirror the working video transcode flow (getTranscodeUrl) but feed the
    // live tune session as the source path.  The official Plex app does the
    // same: GET /video/:/transcode/universal/decision?path=/livetv/sessions/{id}
    // followed by start.m3u8, then plays the universal session segments.
    AppSettings& settings = Application::getInstance().getSettings();
    const auto& vc = platform::getVideoConstraints();

    std::string encodedPath = HttpClient::urlEncode("/livetv/sessions/" + liveSessionId);

    // Unique transcode session id (the server keys the playback session on it).
    char sessionBuf[48];
    snprintf(sessionBuf, sizeof(sessionBuf), "vita-%lu", (unsigned long)time(nullptr));
    std::string sessionId = sessionBuf;
    m_lastSessionId = sessionId;

    char buf[256];
    int bitrate = settings.maxBitrate > 0 ? settings.maxBitrate : vc.defaultBitrate;

    std::string q;
    q += "path=" + encodedPath;
    q += "&mediaIndex=0&partIndex=0";
    // Live source is mpeg2video, so the video stream must be transcoded; audio
    // can be direct-streamed.
    q += "&directPlay=0&directStream=0&directStreamAudio=1";
    q += "&protocol=hls&fastSeek=1&hasMDE=1&location=lan&audioBoost=100";
    snprintf(buf, sizeof(buf), "&videoBitrate=%d", bitrate); q += buf;
    snprintf(buf, sizeof(buf), "&videoResolution=%s", vc.defaultResolution); q += buf;
    q += "&videoQuality=100";
    q += settings.showSubtitles ? "&subtitles=auto" : "&subtitles=none";
    q += "&session=" + sessionId;
    q += "&X-Plex-Token=" + m_authToken;

    // Profile augmentation matches getTranscodeUrl's video branch so the server
    // picks an h264/aac HLS target the player can handle.
    char profileBuf[512];
    snprintf(profileBuf, sizeof(profileBuf),
        "add-transcode-target(type=videoProfile&context=streaming&protocol=hls"
        "&container=mpegts&videoCodec=h264&audioCodec=aac&subtitleCodec=srt)"
        "+add-limitation(scope=videoCodec&scopeName=h264&type=upperBound&name=video.level&value=%d)"
        "+add-limitation(scope=videoCodec&scopeName=h264&type=upperBound&name=video.width&value=%d)"
        "+add-limitation(scope=videoCodec&scopeName=h264&type=upperBound&name=video.height&value=%d)",
        vc.maxVideoLevel, vc.maxVideoWidth, vc.maxVideoHeight);
    std::string profileExtra = profileBuf;

    // Step 1: /decision with X-Plex-* as headers.  X-Plex-Session-Identifier
    // must match the tune's client id so the server links this transcode to the
    // live grab's consumer (/livetv/sessions/{id}/{sessionIdentifier}/...).
    HttpClient decisionClient;
    HttpRequest dReq;
    dReq.url = m_serverUrl + "/video/:/transcode/universal/decision?" + q;
    dReq.method = "GET";
    dReq.headers["Accept"] = "application/json";
    dReq.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    dReq.headers["X-Plex-Product"] = PLEX_CLIENT_NAME;
    dReq.headers["X-Plex-Version"] = PLEX_CLIENT_VERSION;
    dReq.headers["X-Plex-Platform"] = vc.plexPlatform;
    dReq.headers["X-Plex-Device"] = vc.plexDevice;
    dReq.headers["X-Plex-Device-Name"] = vc.plexDevice;
    dReq.headers["X-Plex-Client-Profile-Name"] = "Generic";
    dReq.headers["X-Plex-Client-Profile-Extra"] = profileExtra;
    dReq.headers["X-Plex-Session-Identifier"] = PLEX_CLIENT_ID;
    dReq.timeout = 20;
    HttpResponse dResp = decisionClient.request(dReq);
    brls::Logger::info("buildLiveSessionStreamUrl: decision {} body: {}",
                       dResp.statusCode,
                       dResp.body.empty() ? "(empty)" : redactBodyForLog(dResp.body.substr(0, 300)));

    // Step 2: start.m3u8 - the player needs X-Plex-* as query params too, since
    // it fetches the playlist/segments itself.
    std::string startQuery = q;
    startQuery += "&X-Plex-Client-Identifier=" + std::string(PLEX_CLIENT_ID);
    startQuery += "&X-Plex-Product=" + std::string(PLEX_CLIENT_NAME);
    startQuery += "&X-Plex-Version=" + std::string(PLEX_CLIENT_VERSION);
    startQuery += "&X-Plex-Platform=" + HttpClient::urlEncode(vc.plexPlatform);
    startQuery += "&X-Plex-Device=" + HttpClient::urlEncode(vc.plexDevice);
    startQuery += "&X-Plex-Device-Name=" + HttpClient::urlEncode(vc.plexDevice);
    startQuery += "&X-Plex-Client-Profile-Name=Generic";
    startQuery += "&X-Plex-Client-Profile-Extra=" + HttpClient::urlEncode(profileExtra);
    startQuery += "&X-Plex-Session-Identifier=" + std::string(PLEX_CLIENT_ID);

    url = m_serverUrl + "/video/:/transcode/universal/start.m3u8?" + startQuery;
    brls::Logger::info("buildLiveSessionStreamUrl: stream session={} path=/livetv/sessions/{}",
                       sessionId, liveSessionId);
    return true;
}

std::string PlexClient::getThumbnailUrl(const std::string& thumb, int width, int height) {
    if (thumb.empty()) return "";

    std::string url = buildApiUrl("/photo/:/transcode?url=" + HttpClient::urlEncode(thumb) +
                                  "&width=" + std::to_string(width) +
                                  "&height=" + std::to_string(height) +
                                  "&minSize=1&upscale=1");
    return url;
}

// ============================================================================
// Playlist Management (Official Plex API from developer.plex.tv)
// ============================================================================

bool PlexClient::fetchMusicPlaylists(std::vector<Playlist>& playlists) {
    playlists.clear();

    // GET /playlists?playlistType=audio&smart=0
    // Returns non-smart (dumb) audio playlists
    std::string url = buildApiUrl("/playlists?playlistType=audio");

    HttpClient client;
    std::string response;

    if (!client.get(url, response, {{"Accept", "application/json"}})) {
        brls::Logger::error("fetchMusicPlaylists: Request failed");
        return false;
    }

    // Parse JSON response
    // Look for "Metadata" array
    size_t metadataPos = response.find("\"Metadata\"");
    if (metadataPos == std::string::npos) {
        brls::Logger::debug("fetchMusicPlaylists: No playlists found");
        return true;  // Empty is valid
    }

    size_t arrStart = response.find('[', metadataPos);
    if (arrStart == std::string::npos) return false;

    size_t pos = arrStart + 1;
    while (pos < response.size()) {
        size_t objStart = response.find('{', pos);
        if (objStart == std::string::npos) break;

        // Find matching closing brace (accounting for nesting)
        int depth = 1;
        size_t objEnd = objStart + 1;
        while (objEnd < response.size() && depth > 0) {
            if (response[objEnd] == '{') depth++;
            else if (response[objEnd] == '}') depth--;
            objEnd++;
        }

        std::string objStr = response.substr(objStart, objEnd - objStart);

        Playlist pl;
        pl.ratingKey = extractJsonValue(objStr, "ratingKey");
        pl.key = extractJsonValue(objStr, "key");
        pl.title = extractJsonValue(objStr, "title");
        pl.summary = extractJsonValue(objStr, "summary");
        pl.thumb = extractJsonValue(objStr, "thumb");
        pl.composite = extractJsonValue(objStr, "composite");
        pl.playlistType = extractJsonValue(objStr, "playlistType");
        pl.smart = extractJsonBool(objStr, "smart");
        pl.leafCount = extractJsonInt(objStr, "leafCount");
        pl.duration = extractJsonInt(objStr, "duration");
        pl.addedAt = extractJsonInt(objStr, "addedAt");
        pl.updatedAt = extractJsonInt(objStr, "updatedAt");

        if (!pl.ratingKey.empty()) {
            playlists.push_back(pl);
        }

        pos = objEnd;
    }

    brls::Logger::info("fetchMusicPlaylists: Found {} playlists", playlists.size());
    return true;
}

bool PlexClient::fetchPlaylistItems(const std::string& playlistId, std::vector<PlaylistItem>& items) {
    items.clear();

    // GET /playlists/{playlistId}/items
    std::string url = buildApiUrl("/playlists/" + playlistId + "/items");

    HttpClient client;
    std::string response;

    if (!client.get(url, response, {{"Accept", "application/json"}})) {
        brls::Logger::error("fetchPlaylistItems: Request failed for playlist {}", playlistId);
        return false;
    }

    // Parse JSON response
    size_t metadataPos = response.find("\"Metadata\"");
    if (metadataPos == std::string::npos) {
        brls::Logger::debug("fetchPlaylistItems: Playlist {} is empty", playlistId);
        return true;
    }

    size_t arrStart = response.find('[', metadataPos);
    if (arrStart == std::string::npos) return false;

    size_t pos = arrStart + 1;
    while (pos < response.size()) {
        size_t objStart = response.find('{', pos);
        if (objStart == std::string::npos) break;

        int depth = 1;
        size_t objEnd = objStart + 1;
        while (objEnd < response.size() && depth > 0) {
            if (response[objEnd] == '{') depth++;
            else if (response[objEnd] == '}') depth--;
            objEnd++;
        }

        std::string objStr = response.substr(objStart, objEnd - objStart);

        PlaylistItem item;
        item.playlistItemId = extractJsonValue(objStr, "playlistItemID");

        // Parse media item
        item.media.ratingKey = extractJsonValue(objStr, "ratingKey");
        item.media.key = extractJsonValue(objStr, "key");
        item.media.title = extractJsonValue(objStr, "title");
        item.media.thumb = extractJsonValue(objStr, "thumb");
        item.media.duration = extractJsonInt(objStr, "duration");
        item.media.grandparentTitle = extractJsonValue(objStr, "grandparentTitle");  // Artist
        item.media.parentTitle = extractJsonValue(objStr, "parentTitle");            // Album
        item.media.parentThumb = extractJsonValue(objStr, "parentThumb");
        item.media.grandparentThumb = extractJsonValue(objStr, "grandparentThumb");
        item.media.index = extractJsonInt(objStr, "index");  // Track number
        item.media.type = extractJsonValue(objStr, "type");
        item.media.mediaType = parseMediaType(item.media.type);

        if (!item.media.ratingKey.empty()) {
            items.push_back(item);
        }

        pos = objEnd;
    }

    brls::Logger::info("fetchPlaylistItems: Playlist {} has {} items", playlistId, items.size());
    return true;
}

bool PlexClient::createPlaylist(const std::string& title, const std::string& playlistType, Playlist& result) {
    // POST /playlists?type=15&title={title}&smart=0&playlistType={type}
    // type=15 is the Plex type for playlists
    std::string url = buildApiUrl("/playlists?type=15&title=" + HttpClient::urlEncode(title) +
                                  "&smart=0&playlistType=" + playlistType);

    HttpClient client;
    std::string response;

    if (!client.post(url, "", response, {{"Accept", "application/json"}})) {
        brls::Logger::error("createPlaylist: Failed to create playlist '{}'", title);
        return false;
    }

    // Parse response to get new playlist info
    size_t metadataPos = response.find("\"Metadata\"");
    if (metadataPos == std::string::npos) {
        brls::Logger::error("createPlaylist: Invalid response");
        return false;
    }

    size_t objStart = response.find('{', metadataPos);
    if (objStart == std::string::npos) return false;

    int depth = 1;
    size_t objEnd = objStart + 1;
    while (objEnd < response.size() && depth > 0) {
        if (response[objEnd] == '{') depth++;
        else if (response[objEnd] == '}') depth--;
        objEnd++;
    }

    std::string objStr = response.substr(objStart, objEnd - objStart);

    result.ratingKey = extractJsonValue(objStr, "ratingKey");
    result.key = extractJsonValue(objStr, "key");
    result.title = extractJsonValue(objStr, "title");
    result.playlistType = extractJsonValue(objStr, "playlistType");
    result.smart = false;
    result.leafCount = 0;

    brls::Logger::info("createPlaylist: Created playlist '{}' with id {}", title, result.ratingKey);
    return !result.ratingKey.empty();
}

bool PlexClient::createPlaylistWithItems(const std::string& title, const std::vector<std::string>& ratingKeys, Playlist& result) {
    if (ratingKeys.empty()) {
        return createPlaylist(title, "audio", result);
    }

    // Build URI: server://{machineId}/com.plexapp.plugins.library/library/metadata/{key1},{key2},...
    std::string keysStr;
    for (size_t i = 0; i < ratingKeys.size(); i++) {
        if (i > 0) keysStr += ",";
        keysStr += ratingKeys[i];
    }

    std::string uri = "server://" + m_currentServer.machineIdentifier +
                      "/com.plexapp.plugins.library/library/metadata/" + keysStr;

    // POST /playlists?type=15&title={title}&smart=0&playlistType=audio&uri={uri}
    std::string url = buildApiUrl("/playlists?type=15&title=" + HttpClient::urlEncode(title) +
                                  "&smart=0&playlistType=audio&uri=" + HttpClient::urlEncode(uri));

    HttpClient client;
    std::string response;

    if (!client.post(url, "", response, {{"Accept", "application/json"}})) {
        brls::Logger::error("createPlaylistWithItems: Failed to create playlist '{}'", title);
        return false;
    }

    // Parse response
    size_t metadataPos = response.find("\"Metadata\"");
    if (metadataPos == std::string::npos) {
        brls::Logger::error("createPlaylistWithItems: Invalid response");
        return false;
    }

    size_t objStart = response.find('{', metadataPos);
    if (objStart == std::string::npos) return false;

    int depth = 1;
    size_t objEnd = objStart + 1;
    while (objEnd < response.size() && depth > 0) {
        if (response[objEnd] == '{') depth++;
        else if (response[objEnd] == '}') depth--;
        objEnd++;
    }

    std::string objStr = response.substr(objStart, objEnd - objStart);

    result.ratingKey = extractJsonValue(objStr, "ratingKey");
    result.key = extractJsonValue(objStr, "key");
    result.title = extractJsonValue(objStr, "title");
    result.playlistType = "audio";
    result.smart = false;
    result.leafCount = (int)ratingKeys.size();

    brls::Logger::info("createPlaylistWithItems: Created playlist '{}' with {} items", title, ratingKeys.size());
    return !result.ratingKey.empty();
}

bool PlexClient::deletePlaylist(const std::string& playlistId) {
    // DELETE /playlists/{playlistId}
    std::string url = buildApiUrl("/playlists/" + playlistId);

    HttpClient client;
    std::string response;

    if (!client.del(url, response)) {
        brls::Logger::error("deletePlaylist: Failed to delete playlist {}", playlistId);
        return false;
    }

    brls::Logger::info("deletePlaylist: Deleted playlist {}", playlistId);
    return true;
}

bool PlexClient::renamePlaylist(const std::string& playlistId, const std::string& newTitle) {
    // PUT /playlists/{playlistId}?title={newTitle}
    std::string url = buildApiUrl("/playlists/" + playlistId + "?title=" + HttpClient::urlEncode(newTitle));

    HttpClient client;
    std::string response;

    if (!client.put(url, "", response)) {
        brls::Logger::error("renamePlaylist: Failed to rename playlist {}", playlistId);
        return false;
    }

    brls::Logger::info("renamePlaylist: Renamed playlist {} to '{}'", playlistId, newTitle);
    return true;
}

bool PlexClient::addToPlaylist(const std::string& playlistId, const std::vector<std::string>& ratingKeys) {
    if (ratingKeys.empty()) return true;

    // Build URI
    std::string keysStr;
    for (size_t i = 0; i < ratingKeys.size(); i++) {
        if (i > 0) keysStr += ",";
        keysStr += ratingKeys[i];
    }

    std::string uri = "server://" + m_currentServer.machineIdentifier +
                      "/com.plexapp.plugins.library/library/metadata/" + keysStr;

    // PUT /playlists/{playlistId}/items?uri={uri}
    std::string url = buildApiUrl("/playlists/" + playlistId + "/items?uri=" + HttpClient::urlEncode(uri));

    HttpClient client;
    std::string response;

    if (!client.put(url, "", response)) {
        brls::Logger::error("addToPlaylist: Failed to add items to playlist {}", playlistId);
        return false;
    }

    brls::Logger::info("addToPlaylist: Added {} items to playlist {}", ratingKeys.size(), playlistId);
    return true;
}

bool PlexClient::removeFromPlaylist(const std::string& playlistId, const std::string& playlistItemId) {
    // DELETE /playlists/{playlistId}/items/{playlistItemId}
    std::string url = buildApiUrl("/playlists/" + playlistId + "/items/" + playlistItemId);

    HttpClient client;
    std::string response;

    if (!client.del(url, response)) {
        brls::Logger::error("removeFromPlaylist: Failed to remove item {} from playlist {}", playlistItemId, playlistId);
        return false;
    }

    brls::Logger::info("removeFromPlaylist: Removed item {} from playlist {}", playlistItemId, playlistId);
    return true;
}

bool PlexClient::clearPlaylist(const std::string& playlistId) {
    // DELETE /playlists/{playlistId}/items
    std::string url = buildApiUrl("/playlists/" + playlistId + "/items");

    HttpClient client;
    std::string response;

    if (!client.del(url, response)) {
        brls::Logger::error("clearPlaylist: Failed to clear playlist {}", playlistId);
        return false;
    }

    brls::Logger::info("clearPlaylist: Cleared playlist {}", playlistId);
    return true;
}

bool PlexClient::movePlaylistItem(const std::string& playlistId, const std::string& playlistItemId, const std::string& afterItemId) {
    // PUT /playlists/{playlistId}/items/{playlistItemId}/move?after={afterItemId}
    std::string url = buildApiUrl("/playlists/" + playlistId + "/items/" + playlistItemId + "/move");
    if (!afterItemId.empty()) {
        url += "&after=" + afterItemId;
    }

    HttpClient client;
    std::string response;

    if (!client.put(url, "", response)) {
        brls::Logger::error("movePlaylistItem: Failed to move item {} in playlist {}", playlistItemId, playlistId);
        return false;
    }

    brls::Logger::info("movePlaylistItem: Moved item {} after {} in playlist {}", playlistItemId, afterItemId, playlistId);
    return true;
}

// ============================================================================
// Play Queue API
// ============================================================================

static void parsePlayQueueItems(const std::string& json, PlexClient& client,
                                 PlexClient::PlayQueueContainer& result) {
    // Parse container-level fields
    result.playQueueID = client.extractJsonIntPublic(json, "playQueueID");
    result.playQueueSelectedItemID = client.extractJsonIntPublic(json, "playQueueSelectedItemID");
    result.playQueueSelectedItemOffset = client.extractJsonIntPublic(json, "playQueueSelectedItemOffset");
    result.playQueueSelectedMetadataItemID = client.extractJsonIntPublic(json, "playQueueSelectedMetadataItemID");
    result.playQueueTotalCount = client.extractJsonIntPublic(json, "playQueueTotalCount");
    result.playQueueVersion = client.extractJsonIntPublic(json, "playQueueVersion");

    // playQueueShuffled is a boolean in JSON
    std::string shuffledStr = client.extractJsonValuePublic(json, "playQueueShuffled");
    result.playQueueShuffled = (shuffledStr == "true" || shuffledStr == "1");

    result.playQueueSourceURI = client.extractJsonValuePublic(json, "playQueueSourceURI");

    // Parse Metadata array items
    result.items.clear();
    size_t metaPos = json.find("\"Metadata\"");
    if (metaPos == std::string::npos) return;

    // Find the array start
    size_t arrStart = json.find('[', metaPos);
    if (arrStart == std::string::npos) return;

    // Parse each object in the Metadata array
    size_t pos = arrStart;
    while ((pos = json.find('{', pos)) != std::string::npos) {
        // Find matching closing brace
        int depth = 1;
        size_t objEnd = pos + 1;
        while (objEnd < json.size() && depth > 0) {
            if (json[objEnd] == '{') depth++;
            else if (json[objEnd] == '}') depth--;
            objEnd++;
        }

        // Check if we've gone past the Metadata array
        size_t arrEnd = json.find(']', arrStart);
        if (arrEnd != std::string::npos && pos > arrEnd) break;

        std::string obj = json.substr(pos, objEnd - pos);

        // Only parse if it has playQueueItemID (skip nested objects)
        std::string pqItemId = client.extractJsonValuePublic(obj, "playQueueItemID");
        if (!pqItemId.empty()) {
            PlexClient::PlayQueueItem item;
            item.playQueueItemID = std::stoi(pqItemId);
            item.ratingKey = client.extractJsonValuePublic(obj, "ratingKey");
            item.title = client.extractJsonValuePublic(obj, "title");
            item.grandparentTitle = client.extractJsonValuePublic(obj, "grandparentTitle");
            item.parentTitle = client.extractJsonValuePublic(obj, "parentTitle");
            item.thumb = client.extractJsonValuePublic(obj, "thumb");
            item.parentThumb = client.extractJsonValuePublic(obj, "parentThumb");
            item.grandparentThumb = client.extractJsonValuePublic(obj, "grandparentThumb");
            item.duration = client.extractJsonIntPublic(obj, "duration");
            item.index = client.extractJsonIntPublic(obj, "index");
            item.type = client.extractJsonValuePublic(obj, "type");

            if (!item.ratingKey.empty()) {
                result.items.push_back(item);
            }
        }

        pos = objEnd;
    }
}

std::string PlexClient::buildPlayQueueURI(const std::string& ratingKey) {
    // library://{machineId}/item/%2Flibrary%2Fmetadata%2F{ratingKey}
    return "library://" + m_currentServer.machineIdentifier +
           "/item/%2Flibrary%2Fmetadata%2F" + ratingKey;
}

std::string PlexClient::buildPlayQueueDirectoryURI(const std::string& ratingKey) {
    // library://{machineId}/directory/%2Flibrary%2Fmetadata%2F{ratingKey}%2Fchildren
    return "library://" + m_currentServer.machineIdentifier +
           "/directory/%2Flibrary%2Fmetadata%2F" + ratingKey + "%2Fchildren";
}

bool PlexClient::createPlayQueue(const std::string& uri, const std::string& type,
                                  PlayQueueContainer& result,
                                  const std::string& key,
                                  int shuffle, int repeat, int continuous) {
    std::string params = "?type=" + type + "&uri=" + HttpClient::urlEncode(uri);
    if (!key.empty()) {
        params += "&key=" + HttpClient::urlEncode("/library/metadata/" + key);
    }
    if (shuffle) params += "&shuffle=1";
    if (repeat) params += "&repeat=1";
    if (continuous) params += "&continuous=1";

    std::string url = buildApiUrl("/playQueues" + params);

    HttpClient client;
    HttpRequest req;
    req.url = url;
    req.method = "POST";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    if (resp.statusCode != 200) {
        brls::Logger::error("createPlayQueue: Failed ({})", resp.statusCode);
        if (isAuthError(resp.statusCode)) handleUnauthorized();
        return false;
    }

    parsePlayQueueItems(resp.body, *this, result);
    brls::Logger::info("createPlayQueue: Created PQ {} with {} items (type={})",
                       result.playQueueID, result.playQueueTotalCount, type);
    return result.playQueueID > 0;
}

bool PlexClient::createPlayQueueFromPlaylist(int playlistID, const std::string& type,
                                              PlayQueueContainer& result, int shuffle) {
    std::string params = "?type=" + type + "&playlistID=" + std::to_string(playlistID);
    if (shuffle) params += "&shuffle=1";

    std::string url = buildApiUrl("/playQueues" + params);

    HttpClient client;
    HttpRequest req;
    req.url = url;
    req.method = "POST";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    if (resp.statusCode != 200) {
        brls::Logger::error("createPlayQueueFromPlaylist: Failed ({})", resp.statusCode);
        if (isAuthError(resp.statusCode)) handleUnauthorized();
        return false;
    }

    parsePlayQueueItems(resp.body, *this, result);
    brls::Logger::info("createPlayQueueFromPlaylist: Created PQ {} from playlist {} ({} items)",
                       result.playQueueID, playlistID, result.playQueueTotalCount);
    return result.playQueueID > 0;
}

bool PlexClient::getPlayQueue(int playQueueID, PlayQueueContainer& result) {
    std::string url = buildApiUrl("/playQueues/" + std::to_string(playQueueID));

    HttpClient client;
    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    if (resp.statusCode != 200) {
        brls::Logger::error("getPlayQueue: Failed to get PQ {} ({})", playQueueID, resp.statusCode);
        if (isAuthError(resp.statusCode)) handleUnauthorized();
        return false;
    }

    parsePlayQueueItems(resp.body, *this, result);
    return result.playQueueID > 0;
}

bool PlexClient::addToPlayQueue(int playQueueID, const std::string& uri, bool playNext) {
    std::string params = "?uri=" + HttpClient::urlEncode(uri);
    if (playNext) params += "&next=1";

    std::string url = buildApiUrl("/playQueues/" + std::to_string(playQueueID) + params);

    HttpClient client;
    std::string response;
    if (!client.put(url, "", response)) {
        brls::Logger::error("addToPlayQueue: Failed to add to PQ {}", playQueueID);
        return false;
    }

    brls::Logger::info("addToPlayQueue: Added to PQ {} (next={})", playQueueID, playNext);
    return true;
}

bool PlexClient::clearPlayQueue(int playQueueID) {
    std::string url = buildApiUrl("/playQueues/" + std::to_string(playQueueID) + "/items");

    HttpClient client;
    std::string response;
    if (!client.del(url, response)) {
        brls::Logger::error("clearPlayQueue: Failed to clear PQ {}", playQueueID);
        return false;
    }

    brls::Logger::info("clearPlayQueue: Cleared PQ {}", playQueueID);
    return true;
}

bool PlexClient::removeFromPlayQueue(int playQueueID, int playQueueItemID) {
    std::string url = buildApiUrl("/playQueues/" + std::to_string(playQueueID) +
                                  "/items/" + std::to_string(playQueueItemID));

    HttpClient client;
    std::string response;
    if (!client.del(url, response)) {
        brls::Logger::error("removeFromPlayQueue: Failed to remove item {} from PQ {}",
                           playQueueItemID, playQueueID);
        return false;
    }

    brls::Logger::info("removeFromPlayQueue: Removed item {} from PQ {}",
                       playQueueItemID, playQueueID);
    return true;
}

bool PlexClient::movePlayQueueItem(int playQueueID, int playQueueItemID, int afterItemID) {
    std::string url = buildApiUrl("/playQueues/" + std::to_string(playQueueID) +
                                  "/items/" + std::to_string(playQueueItemID) + "/move");
    if (afterItemID > 0) {
        url += "?after=" + std::to_string(afterItemID);
    }

    HttpClient client;
    std::string response;
    if (!client.put(url, "", response)) {
        brls::Logger::error("movePlayQueueItem: Failed to move item {} in PQ {}",
                           playQueueItemID, playQueueID);
        return false;
    }

    brls::Logger::info("movePlayQueueItem: Moved item {} after {} in PQ {}",
                       playQueueItemID, afterItemID, playQueueID);
    return true;
}

bool PlexClient::shufflePlayQueue(int playQueueID, PlayQueueContainer& result) {
    std::string url = buildApiUrl("/playQueues/" + std::to_string(playQueueID) + "/shuffle");

    HttpClient client;
    HttpRequest req;
    req.url = url;
    req.method = "PUT";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    if (resp.statusCode != 200) {
        brls::Logger::error("shufflePlayQueue: Failed ({})", resp.statusCode);
        return false;
    }

    parsePlayQueueItems(resp.body, *this, result);
    brls::Logger::info("shufflePlayQueue: Shuffled PQ {} ({} items)", playQueueID, result.playQueueTotalCount);
    return true;
}

bool PlexClient::unshufflePlayQueue(int playQueueID, PlayQueueContainer& result) {
    std::string url = buildApiUrl("/playQueues/" + std::to_string(playQueueID) + "/unshuffle");

    HttpClient client;
    HttpRequest req;
    req.url = url;
    req.method = "PUT";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    if (resp.statusCode != 200) {
        brls::Logger::error("unshufflePlayQueue: Failed ({})", resp.statusCode);
        return false;
    }

    parsePlayQueueItems(resp.body, *this, result);
    brls::Logger::info("unshufflePlayQueue: Unshuffled PQ {} ({} items)", playQueueID, result.playQueueTotalCount);
    return true;
}

} // namespace vitaplex
