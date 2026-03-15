/**
 * VitaPlex - Plex API Client implementation
 */

#include "app/plex_client.hpp"
#include "app/application.hpp"
#include "utils/http_client.hpp"

#include <borealis.hpp>
#include <cstring>
#include <ctime>
#include <algorithm>

namespace vitaplex {

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
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    req.headers["X-Plex-Product"] = PLEX_CLIENT_NAME;
    req.headers["X-Plex-Version"] = PLEX_CLIENT_VERSION;
    req.headers["X-Plex-Platform"] = PLEX_PLATFORM;
    req.headers["X-Plex-Device"] = PLEX_DEVICE;

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
    req.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    req.headers["X-Plex-Product"] = PLEX_CLIENT_NAME;
    req.headers["X-Plex-Version"] = PLEX_CLIENT_VERSION;
    req.headers["X-Plex-Platform"] = PLEX_PLATFORM;
    req.headers["X-Plex-Device"] = PLEX_DEVICE;

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
            server.name = extractJsonValue(obj, "name");
            server.machineIdentifier = extractJsonValue(obj, "clientIdentifier");

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

    HttpClient client;
    std::string url = buildApiUrl("/library/sections");
    brls::Logger::debug("Fetching: {}", url);

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
            brls::Logger::debug("Body: {}", resp.body.substr(0, 500));
        }
        return false;
    }

    // Log first part of response for debugging
    brls::Logger::debug("Response body: {}", resp.body.substr(0, std::min((size_t)500, resp.body.length())));

    sections.clear();

    // Find all Directory entries - in JSON arrays
    size_t pos = 0;
    while ((pos = resp.body.find("\"key\"", pos)) != std::string::npos) {
        // Go back to find the start of this object
        size_t objStart = resp.body.rfind('{', pos);
        if (objStart == std::string::npos) {
            pos++;
            continue;
        }

        // Check if we've already processed this object
        std::string beforeObj = resp.body.substr(objStart, pos - objStart);
        if (beforeObj.find("\"key\"") != std::string::npos) {
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

bool PlexClient::fetchLibraryContent(const std::string& sectionKey, std::vector<MediaItem>& items, int metadataType) {
    brls::Logger::debug("fetchLibraryContent: section={} type={}", sectionKey, metadataType);

    HttpClient client;
    std::string url = buildApiUrl("/library/sections/" + sectionKey + "/all");
    if (metadataType > 0) {
        url += "&type=" + std::to_string(metadataType);
    }

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

    // Parse items by looking for objects with "ratingKey" (media items have this)
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
        item.contentRating = extractJsonValue(obj, "contentRating");
        item.subtype = extractJsonValue(obj, "subtype");

        if (!item.ratingKey.empty() && !item.title.empty()) {
            items.push_back(item);
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

bool PlexClient::fetchHubs(std::vector<Hub>& hubs) {
    brls::Logger::debug("fetchHubs: serverUrl={}", m_serverUrl);

    HttpClient client;
    std::string url = buildApiUrl("/hubs");

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

    hubs.clear();

    // Parse hubs - look for objects with "hubIdentifier" field
    size_t pos = 0;
    while ((pos = resp.body.find("\"hubIdentifier\"", pos)) != std::string::npos) {
        // Go back to find start of this hub object
        size_t hubStart = resp.body.rfind('{', pos);
        if (hubStart == std::string::npos) {
            pos++;
            continue;
        }

        // Find end of hub object
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
    std::string url = buildApiUrl("/hubs/search?query=" + HttpClient::urlEncode(query));

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
        item.grandparentTitle = extractJsonValue(obj, "grandparentTitle");
        item.parentTitle = extractJsonValue(obj, "parentTitle");
        item.parentThumb = extractJsonValue(obj, "parentThumb");
        item.grandparentThumb = extractJsonValue(obj, "grandparentThumb");

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

bool PlexClient::fetchGenreItems(const std::string& sectionKey, std::vector<GenreItem>& genres) {
    brls::Logger::debug("Fetching genre items for section: {}", sectionKey);

    HttpClient client;
    // Plex API: /library/sections/{key}/genre returns genres with keys for filtering
    std::string url = buildApiUrl("/library/sections/" + sectionKey + "/genre");

    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    if (resp.statusCode != 200) {
        brls::Logger::error("Failed to fetch genre items: {}", resp.statusCode);
        return false;
    }

    genres.clear();

    // Parse genres - look for Directory entries with "title", "key", and "fastKey"
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

        // Skip container/meta entries
        if (!item.title.empty() && !item.key.empty() &&
            item.title != "genre" && item.title.find("MediaContainer") == std::string::npos) {
            // Avoid duplicates
            bool found = false;
            for (const auto& g : genres) {
                if (g.title == item.title) { found = true; break; }
            }
            if (!found) {
                genres.push_back(item);
            }
        }

        pos = objEnd;
    }

    brls::Logger::info("Found {} genre items for section {}", genres.size(), sectionKey);
    return true;
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

        int bitrate = settings.maxBitrate > 0 ? settings.maxBitrate : 2000;

        const char* resolution = "960x544";  // Vita native
        if (bitrate >= 8000) {
            resolution = "1280x720";
        } else if (bitrate < 2000) {
            resolution = "640x360";
        }

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
        // Matches switchfin's Vita config (proven to work on Vita's MPV):
        // - HLS: segmented streaming, no moov atom issues
        // - TS container: works with HLS protocol
        // - AAC only: MP3 in MPEG-TS causes "unspecified frame size" probe
        //   failure with Vita's mp3_vita decoder, leading to A/V desync
        // - H.264 level 4.0 max, 720p max (Vita hardware limits)
        profileExtra = "add-transcode-target(type=videoProfile"
                       "&context=streaming&protocol=hls"
                       "&container=mpegts&videoCodec=h264"
                       "&audioCodec=aac"
                       "&subtitleCodec=srt)"
                       "+add-limitation(scope=videoCodec&scopeName=h264"
                       "&type=upperBound&name=video.level&value=40)"
                       "+add-limitation(scope=videoCodec&scopeName=h264"
                       "&type=upperBound&name=video.width&value=1280)"
                       "+add-limitation(scope=videoCodec&scopeName=h264"
                       "&type=upperBound&name=video.height&value=720)";
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
    decisionReq.headers["X-Plex-Client-Identifier"] = "VitaPlex";
    decisionReq.headers["X-Plex-Product"] = "VitaPlex";
    decisionReq.headers["X-Plex-Version"] = "1.0.0";
    decisionReq.headers["X-Plex-Platform"] = "PlayStation Vita";
    decisionReq.headers["X-Plex-Device"] = "PS Vita";
    decisionReq.headers["X-Plex-Device-Name"] = "PS Vita";
    decisionReq.headers["X-Plex-Client-Profile-Name"] = "Generic";
    decisionReq.headers["X-Plex-Client-Profile-Extra"] = profileExtra;
    HttpResponse decisionResp = decisionClient.request(decisionReq);

    brls::Logger::info("getTranscodeUrl: Decision response: {} body: {}",
                      decisionResp.statusCode, decisionResp.body.substr(0, 500));

    if (decisionResp.statusCode != 200) {
        brls::Logger::warning("getTranscodeUrl: Decision returned {}, trying start anyway",
                             decisionResp.statusCode);
    }

    // Step 2: Build the /start URL for MPV to stream.
    // Include X-Plex-* as query params AND MPV sends them as headers too.
    std::string startQuery = queryParams;
    startQuery += "&X-Plex-Client-Identifier=VitaPlex";
    startQuery += "&X-Plex-Product=VitaPlex";
    startQuery += "&X-Plex-Version=1.0.0";
    startQuery += "&X-Plex-Platform=PlayStation%20Vita";
    startQuery += "&X-Plex-Device=PS%20Vita";
    startQuery += "&X-Plex-Device-Name=PS%20Vita";
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

        HttpResponse resp = client.request(req);
        if (resp.statusCode == 200 && !resp.body.empty()) {
            brls::Logger::debug("fetchLiveTVChannels: EPG channels response ({} bytes, first 500): {}",
                                resp.body.length(), resp.body.substr(0, 500));

            // Parse Channel array from response
            // Per openapi.json schema: Channel objects have callSign, identifier, channelVcn, thumb, title, key
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

                // Cross-reference with ChannelMapping to get deviceIdentifier for tuning
                for (const auto& mapping : m_channelMappings) {
                    if (!channel.key.empty() && channel.key == mapping.channelKey) {
                        channel.channelIdentifier = mapping.deviceIdentifier;
                        break;
                    }
                    if (!identifier.empty() && identifier == mapping.lineupIdentifier) {
                        channel.channelIdentifier = mapping.deviceIdentifier;
                        if (channel.key.empty()) {
                            channel.key = mapping.channelKey;
                        }
                        break;
                    }
                }

                if (!channel.callSign.empty() || !channel.title.empty()) {
                    channels.push_back(channel);
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

bool PlexClient::tuneLiveTVChannelByKey(const std::string& channelVcn, const std::string& epgChannelKey, std::string& streamUrl, const std::string& programMetadataKey) {
    brls::Logger::info("tuneLiveTVChannelByKey: vcn={}, epgKey={}, metadataKey={}", channelVcn, epgChannelKey, programMetadataKey);

    // Try with VCN first (official API uses deviceIdentifier like "2.1")
    if (!channelVcn.empty()) {
        if (tuneLiveTVChannel(channelVcn, streamUrl, programMetadataKey)) {
            return true;
        }
        brls::Logger::info("tuneLiveTVChannelByKey: VCN failed, trying EPG key...");
    }

    // Fall back to EPG channel key
    if (!epgChannelKey.empty() && epgChannelKey != channelVcn) {
        return tuneLiveTVChannel(epgChannelKey, streamUrl, programMetadataKey);
    }

    return false;
}

bool PlexClient::tuneLiveTVChannel(const std::string& channelKey, std::string& streamUrl, const std::string& programMetadataKey) {
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
    // Per openapi.json: channel param is a string like "2.1" (the deviceIdentifier)
    // Returns Media with uuid (session ID) which can be used for HLS streaming
    std::string tuneUrl = buildApiUrl("/livetv/dvrs/" + m_dvrId + "/channels/" + channelKey + "/tune");

    HttpRequest tuneReq;
    tuneReq.url = tuneUrl;
    tuneReq.method = "POST";
    tuneReq.headers["Accept"] = "application/json";
    tuneReq.headers["X-Plex-Client-Identifier"] = PLEX_CLIENT_ID;
    tuneReq.headers["X-Plex-Product"] = PLEX_CLIENT_NAME;
    // Use longer timeout - tune can take time, especially with EPG channel keys
    // that may create MediaSubscription responses with chunked data
    tuneReq.timeout = 30;

    brls::Logger::debug("tuneLiveTVChannel: POST {}", tuneUrl);
    HttpResponse tuneResp = client.request(tuneReq);

    if (tuneResp.statusCode == 200 && !tuneResp.body.empty()) {
        brls::Logger::debug("tuneLiveTVChannel: Tune response ({} bytes): {}",
                            tuneResp.body.length(), tuneResp.body.substr(0, 500));

        // Check for "Could not tune" error (status -1)
        // Server returns 200 but with {"MediaContainer":{"status":-1,"message":"Could not tune..."}}
        std::string tuneStatus = extractJsonValue(tuneResp.body, "status");
        std::string tuneMessage = extractJsonValue(tuneResp.body, "message");
        if (tuneStatus == "-1" || tuneResp.body.find("Could not tune") != std::string::npos) {
            brls::Logger::warning("tuneLiveTVChannel: Server reported tune failure: {}", tuneMessage);
            // Don't return - fall through to transcode fallback
        } else {
            // Extract session UUID from Media object in response
            // Per openapi.json: Media[].uuid is the session ID
            std::string sessionUuid = extractJsonValue(tuneResp.body, "uuid");

            if (!sessionUuid.empty()) {
                // Build HLS stream URL using session endpoint
                // Official API: GET /livetv/sessions/{sessionId}/{consumerId}/index.m3u8
                // Use client identifier as consumerId
                streamUrl = buildApiUrl("/livetv/sessions/" + sessionUuid + "/" +
                                        std::string(PLEX_CLIENT_ID) + "/index.m3u8");
                brls::Logger::info("tuneLiveTVChannel: Stream URL (session HLS) = {}", streamUrl);
                return true;
            }

            brls::Logger::warning("tuneLiveTVChannel: No session UUID in tune response");
        }
    } else if (tuneResp.statusCode == 0 || tuneResp.statusCode == -1) {
        // Connection drop / timeout - may happen with EPG key tunes that return chunked data
        brls::Logger::warning("tuneLiveTVChannel: Connection dropped (status {}), body so far ({} bytes): {}",
                              tuneResp.statusCode, tuneResp.body.length(),
                              tuneResp.body.empty() ? "(empty)" : tuneResp.body.substr(0, 300));
        // Try to extract UUID from partial response if we got any data
        if (!tuneResp.body.empty()) {
            std::string sessionUuid = extractJsonValue(tuneResp.body, "uuid");
            if (!sessionUuid.empty()) {
                streamUrl = buildApiUrl("/livetv/sessions/" + sessionUuid + "/" +
                                        std::string(PLEX_CLIENT_ID) + "/index.m3u8");
                brls::Logger::info("tuneLiveTVChannel: Stream URL from partial response = {}", streamUrl);
                return true;
            }
        }
    } else if (tuneResp.statusCode == 500) {
        brls::Logger::error("tuneLiveTVChannel: Tune failed (500 - server error) for channel {}", channelKey);
    } else {
        brls::Logger::error("tuneLiveTVChannel: Tune returned {} for channel {}, body: {}",
                            tuneResp.statusCode, channelKey,
                            tuneResp.body.empty() ? "(empty)" : tuneResp.body.substr(0, 200));
    }

    // Fallback: try transcode/universal with program metadata key
    // This provides transcoding for devices that can't handle the native stream format
    if (!programMetadataKey.empty()) {
        brls::Logger::info("tuneLiveTVChannel: Falling back to transcode with program metadata key...");

        // programMetadataKey from EPG grid is already URL-encoded (e.g. plex%3A%2F%2Fepisode%2F...)
        // Do NOT re-encode it or we get double-encoding (plex%253A%252F%252F...) causing 400 errors
        std::string params = "path=" + programMetadataKey +
            "&mediaIndex=0&partIndex=0"
            "&directPlay=0&directStream=1&directStreamAudio=1"
            "&hasMDE=1&location=lan"
            "&audioBoost=100&audioChannelCount=2"
            "&protocol=hls&videoBitrate=2000&videoResolution=960x544&videoQuality=100"
            "&subtitles=none"
            "&X-Plex-Client-Identifier=" + std::string(PLEX_CLIENT_ID) +
            "&X-Plex-Product=" + std::string(PLEX_CLIENT_NAME) +
            "&X-Plex-Version=1.0.0"
            "&X-Plex-Platform=PlayStation%20Vita"
            "&X-Plex-Device=PS%20Vita"
            "&X-Plex-Device-Name=PS%20Vita";

        std::string decisionUrl = buildApiUrl("/video/:/transcode/universal/decision?" + params);

        HttpRequest decReq;
        decReq.url = decisionUrl;
        decReq.method = "GET";
        decReq.headers["Accept"] = "application/json";
        decReq.timeout = 15;

        brls::Logger::debug("tuneLiveTVChannel: Decision URL: {}", decisionUrl);
        HttpResponse decResp = client.request(decReq);

        if (decResp.statusCode == 200) {
            streamUrl = buildApiUrl("/video/:/transcode/universal/start.m3u8?" + params);
            brls::Logger::info("tuneLiveTVChannel: Stream URL (transcode) = {}", streamUrl);
            return true;
        }

        brls::Logger::error("tuneLiveTVChannel: Transcode decision failed: {} body: {}",
                            decResp.statusCode,
                            decResp.body.empty() ? "(empty)" : decResp.body.substr(0, 300));
    }

    brls::Logger::error("tuneLiveTVChannel: All tuning strategies failed for channel {}", channelKey);
    return false;
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
