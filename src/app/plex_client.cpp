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
        size_t valueEnd = json.find('"', valueStart + 1);
        if (valueEnd == std::string::npos) return "";
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
            Application::getInstance().setAuthToken(m_authToken);
            brls::Logger::info("PIN authenticated successfully");
            return true;
        }
    }

    return false;
}

bool PlexClient::refreshToken() {
    // JWT token refresh not yet implemented
    // Legacy tokens don't expire, so this is only needed for JWT auth
    brls::Logger::info("Token refresh not available (using legacy auth)");
    return false;
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
    brls::Logger::info("Connecting to server: {}", url);

    m_serverUrl = url;
    Application::getInstance().setServerUrl(url);

    HttpClient client;
    HttpRequest req;
    req.url = buildApiUrl("/");
    req.method = "GET";
    req.headers["Accept"] = "application/json";

    // Use connection timeout from settings (default 3 minutes for slow connections)
    int timeout = Application::getInstance().getSettings().connectionTimeout;
    if (timeout <= 0) timeout = 180;  // Default to 3 minutes
    req.timeout = timeout;

    brls::Logger::debug("Connection timeout: {} seconds", timeout);

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

bool PlexClient::fetchLibraryContent(const std::string& sectionKey, std::vector<MediaItem>& items) {
    brls::Logger::debug("fetchLibraryContent: section={}", sectionKey);

    HttpClient client;
    std::string url = buildApiUrl("/library/sections/" + sectionKey + "/all");

    // Request JSON format
    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    brls::Logger::debug("Response: {} - {} bytes", resp.statusCode, resp.body.length());

    if (resp.statusCode != 200) {
        brls::Logger::error("Failed to fetch content: {}", resp.statusCode);
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

    if (resp.statusCode != 200) return false;

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
        case MediaType::MUSIC_ALBUM:
        case MediaType::MUSIC_TRACK:
            typeCode = 8;  // artist
            typeName = "artist";
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

bool PlexClient::fetchByGenre(const std::string& sectionKey, const std::string& genre, std::vector<MediaItem>& items) {
    brls::Logger::debug("Fetching items by genre: {} in section {}", genre, sectionKey);

    HttpClient client;
    // Plex API: /library/sections/{key}/all?genre={genreId}
    std::string url = buildApiUrl("/library/sections/" + sectionKey + "/all?genre=" + HttpClient::urlEncode(genre));

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

bool PlexClient::fetchByGenreKey(const std::string& sectionKey, const std::string& genreKey, std::vector<MediaItem>& items) {
    brls::Logger::debug("Fetching items by genre key: {} in section {}", genreKey, sectionKey);

    HttpClient client;
    // Plex API: Use the fastKey or construct filter with genre key/ID
    // The genreKey is typically the numeric ID from the genre list
    std::string url = buildApiUrl("/library/sections/" + sectionKey + "/all?genre=" + genreKey);

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

    // Detect if this is audio (track) or video
    bool isAudio = (resp.body.find("\"type\":\"track\"") != std::string::npos);
    brls::Logger::debug("getTranscodeUrl: isAudio={}", isAudio);

    // URL-encode the path for the transcode request
    std::string encodedPath = HttpClient::urlEncode(partKey);

    // Build transcode URL
    url = m_serverUrl;

    if (isAudio) {
        // Audio transcode - convert to MP3 which the Vita can play
        url += "/music/:/transcode/universal/start.mp3?";
        url += "path=" + encodedPath;
        url += "&mediaIndex=0&partIndex=0";
        url += "&protocol=http";
        url += "&directPlay=0&directStream=0";  // Force transcode
        url += "&audioCodec=mp3&audioBitrate=320";
    } else {
        // Video transcode - convert to H.264/AAC which the Vita can decode
        url += "/video/:/transcode/universal/start.mp4?";
        url += "path=" + encodedPath;
        url += "&mediaIndex=0&partIndex=0";
        url += "&protocol=http";
        url += "&fastSeek=1";
        url += "&directPlay=0&directStream=0";  // Force transcode

        // Get quality settings
        AppSettings& settings = Application::getInstance().getSettings();
        int bitrate = settings.maxBitrate > 0 ? settings.maxBitrate : 4000;
        int maxWidth = 960;   // Vita screen width
        int maxHeight = 544;  // Vita screen height

        // Adjust resolution based on quality setting
        if (bitrate >= 8000) {
            maxWidth = 1280;
            maxHeight = 720;
        } else if (bitrate >= 2000) {
            maxWidth = 960;
            maxHeight = 544;
        } else {
            maxWidth = 640;
            maxHeight = 360;
        }

        char bitrateStr[64];
        snprintf(bitrateStr, sizeof(bitrateStr), "&videoBitrate=%d", bitrate);
        url += bitrateStr;
        url += "&videoCodec=h264";

        char resStr[64];
        snprintf(resStr, sizeof(resStr), "&maxWidth=%d&maxHeight=%d", maxWidth, maxHeight);
        url += resStr;

        // Audio settings - stereo AAC
        url += "&audioCodec=aac&audioChannels=2";
    }

    // Add resume offset if specified
    if (offsetMs > 0) {
        char offsetStr[32];
        snprintf(offsetStr, sizeof(offsetStr), "&offset=%d", offsetMs);
        url += offsetStr;
    }

    // Add authentication and client identification
    url += "&X-Plex-Token=" + m_authToken;
    url += "&X-Plex-Client-Identifier=VitaPlex";
    url += "&X-Plex-Product=VitaPlex";
    url += "&X-Plex-Version=1.0.0";
    url += "&X-Plex-Platform=PlayStation%20Vita";
    url += "&X-Plex-Device=PS%20Vita";

    // Generate a session ID for this transcode request
    char sessionId[32];
    snprintf(sessionId, sizeof(sessionId), "&session=%lu", (unsigned long)time(nullptr));
    url += sessionId;

    brls::Logger::info("getTranscodeUrl: Transcode URL = {}", url);
    return true;
}

bool PlexClient::updatePlayProgress(const std::string& ratingKey, int timeMs) {
    HttpClient client;
    std::string url = buildApiUrl("/:/progress?key=" + ratingKey + "&time=" + std::to_string(timeMs) + "&identifier=com.plexapp.plugins.library");
    HttpResponse resp = client.get(url);
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
    // Quick check if Live TV is available on this server
    HttpClient client;
    std::string url = buildApiUrl("/livetv/dvrs");
    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    req.timeout = 10;  // Short timeout for availability check

    HttpResponse resp = client.request(req);

    // If we get a 200 response (even with empty DVR list), Live TV is configured
    // A 404 or error means Live TV is not set up
    m_hasLiveTV = (resp.statusCode == 200);
    brls::Logger::info("Live TV availability check: {}", m_hasLiveTV ? "available" : "not available");
}

bool PlexClient::fetchLiveTVChannels(std::vector<LiveTVChannel>& channels) {
    HttpClient client;
    std::string url = buildApiUrl("/livetv/sessions");
    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    HttpResponse resp = client.request(req);

    if (resp.statusCode != 200) {
        brls::Logger::error("fetchLiveTVChannels failed: {}", resp.statusCode);
        return false;
    }

    channels.clear();

    // Parse channels from response
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

        LiveTVChannel channel;
        channel.ratingKey = extractJsonValue(obj, "ratingKey");
        channel.key = extractJsonValue(obj, "key");
        channel.title = extractJsonValue(obj, "title");
        channel.thumb = extractJsonValue(obj, "thumb");
        channel.callSign = extractJsonValue(obj, "callSign");
        channel.channelNumber = extractJsonInt(obj, "channelNumber");
        channel.currentProgram = extractJsonValue(obj, "title");  // Current playing title

        if (!channel.ratingKey.empty()) {
            channels.push_back(channel);
        }

        pos = objEnd;
    }

    brls::Logger::info("fetchLiveTVChannels: Found {} channels", channels.size());
    return true;
}

bool PlexClient::fetchEPGGrid(std::vector<LiveTVChannel>& channelsWithPrograms, int hoursAhead) {
    brls::Logger::debug("fetchEPGGrid: fetching {} hours of programming", hoursAhead);

    HttpClient client;

    // First get the DVR info to find the guide endpoint
    std::string dvrsUrl = buildApiUrl("/livetv/dvrs");
    HttpRequest dvrsReq;
    dvrsReq.url = dvrsUrl;
    dvrsReq.method = "GET";
    dvrsReq.headers["Accept"] = "application/json";
    HttpResponse dvrsResp = client.request(dvrsReq);

    if (dvrsResp.statusCode != 200) {
        brls::Logger::error("fetchEPGGrid: Failed to fetch DVRs: {}", dvrsResp.statusCode);
        return false;
    }

    // Get the DVR key from the response
    std::string dvrKey = extractJsonValue(dvrsResp.body, "key");
    if (dvrKey.empty()) {
        // Try to find it in the response
        size_t keyPos = dvrsResp.body.find("\"key\"");
        if (keyPos != std::string::npos) {
            dvrKey = extractJsonValue(dvrsResp.body.substr(keyPos), "key");
        }
    }

    brls::Logger::debug("fetchEPGGrid: DVR key = {}", dvrKey);

    // Fetch EPG grid - try multiple endpoints
    std::string epgUrl;
    HttpResponse epgResp;

    // Try /livetv/epg endpoint first
    epgUrl = buildApiUrl("/livetv/epg");
    HttpRequest epgReq;
    epgReq.url = epgUrl;
    epgReq.method = "GET";
    epgReq.headers["Accept"] = "application/json";
    epgReq.timeout = 30;
    epgResp = client.request(epgReq);

    if (epgResp.statusCode != 200) {
        // Try alternative: /media/subscriptions/scheduled
        brls::Logger::debug("fetchEPGGrid: /livetv/epg failed, trying grid endpoint");

        // Try with the DVR key
        if (!dvrKey.empty()) {
            epgUrl = buildApiUrl(dvrKey + "/grid?");
        } else {
            epgUrl = buildApiUrl("/livetv/grid");
        }
        epgReq.url = epgUrl;
        epgResp = client.request(epgReq);
    }

    if (epgResp.statusCode != 200) {
        brls::Logger::error("fetchEPGGrid: Failed to fetch EPG: {}", epgResp.statusCode);
        // Fall back to basic channels without program info
        return fetchLiveTVChannels(channelsWithPrograms);
    }

    brls::Logger::debug("fetchEPGGrid: Got {} bytes of EPG data", epgResp.body.length());

    channelsWithPrograms.clear();

    // Parse the EPG response - look for channel entries with program data
    // The structure varies but typically has channels with Media/Program entries
    size_t pos = 0;

    // Look for channel markers in the response
    while ((pos = epgResp.body.find("\"channelNumber\"", pos)) != std::string::npos) {
        // Find the start of this channel object
        size_t objStart = epgResp.body.rfind('{', pos);
        if (objStart == std::string::npos) {
            pos++;
            continue;
        }

        // Skip if we already parsed this object (check if there's a channelNumber before this in same object)
        std::string beforeSection = epgResp.body.substr(objStart, pos - objStart);
        if (beforeSection.find("\"channelNumber\"") != std::string::npos) {
            pos++;
            continue;
        }

        // Find end of object - need to account for nested objects
        int braceCount = 1;
        size_t objEnd = objStart + 1;
        while (braceCount > 0 && objEnd < epgResp.body.length()) {
            if (epgResp.body[objEnd] == '{') braceCount++;
            else if (epgResp.body[objEnd] == '}') braceCount--;
            objEnd++;
        }

        std::string channelObj = epgResp.body.substr(objStart, objEnd - objStart);

        LiveTVChannel channel;
        channel.ratingKey = extractJsonValue(channelObj, "ratingKey");
        channel.key = extractJsonValue(channelObj, "key");
        channel.title = extractJsonValue(channelObj, "title");
        channel.thumb = extractJsonValue(channelObj, "thumb");
        channel.callSign = extractJsonValue(channelObj, "callSign");
        channel.channelNumber = extractJsonInt(channelObj, "channelNumber");

        // Look for program info in the channel object
        // Programs may be in "Media" array or directly as program fields
        size_t programPos = channelObj.find("\"Program\"");
        if (programPos == std::string::npos) {
            programPos = channelObj.find("\"Media\"");
        }

        if (programPos != std::string::npos) {
            // Extract program title
            std::string programSection = channelObj.substr(programPos);
            channel.currentProgram = extractJsonValue(programSection, "title");

            // Try to get program times
            std::string beginsAt = extractJsonValue(programSection, "beginsAt");
            std::string endsAt = extractJsonValue(programSection, "endsAt");

            if (!beginsAt.empty()) {
                channel.programStart = atoll(beginsAt.c_str());
            }
            if (!endsAt.empty()) {
                channel.programEnd = atoll(endsAt.c_str());
            }

            // Get next program if available
            size_t nextProgramPos = programSection.find("\"title\"", 10);
            if (nextProgramPos != std::string::npos) {
                channel.nextProgram = extractJsonValue(programSection.substr(nextProgramPos), "title");
            }
        }

        if (!channel.ratingKey.empty() || channel.channelNumber > 0) {
            channelsWithPrograms.push_back(channel);
        }

        pos = objEnd;
    }

    // Sort channels by channel number
    std::sort(channelsWithPrograms.begin(), channelsWithPrograms.end(),
              [](const LiveTVChannel& a, const LiveTVChannel& b) {
                  return a.channelNumber < b.channelNumber;
              });

    brls::Logger::info("fetchEPGGrid: Got {} channels with program info", channelsWithPrograms.size());
    return !channelsWithPrograms.empty();
}

std::string PlexClient::getThumbnailUrl(const std::string& thumb, int width, int height) {
    if (thumb.empty()) return "";

    std::string url = buildApiUrl("/photo/:/transcode?url=" + HttpClient::urlEncode(thumb) +
                                  "&width=" + std::to_string(width) +
                                  "&height=" + std::to_string(height) +
                                  "&minSize=1&upscale=1");
    return url;
}

} // namespace vitaplex
