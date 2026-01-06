/**
 * VitaPlex - Plex API Client implementation
 */

#include "app/plex_client.hpp"
#include "app/application.hpp"
#include "utils/http_client.hpp"

#include <borealis.hpp>
#include <cstring>
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

    // Use longer timeout for potentially slow connections (relay, remote)
    bool isRelay = (url.find(".plex.direct") != std::string::npos);
    req.timeout = isRelay ? 60 : 30;

    HttpResponse resp = client.request(req);

    if (resp.statusCode == 200) {
        m_currentServer.name = extractJsonValue(resp.body, "friendlyName");
        m_currentServer.machineIdentifier = extractJsonValue(resp.body, "machineIdentifier");
        m_currentServer.address = url;

        brls::Logger::info("Connected to: {}", m_currentServer.name);
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

bool PlexClient::getPlaybackUrl(const std::string& ratingKey, std::string& url) {
    // Build direct play URL
    url = buildApiUrl("/library/metadata/" + ratingKey + "/file");
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

bool PlexClient::fetchLiveTVChannels(std::vector<LiveTVChannel>& channels) {
    HttpClient client;
    std::string url = buildApiUrl("/livetv/sessions");
    HttpResponse resp = client.get(url);

    if (resp.statusCode != 200) {
        m_hasLiveTV = false;
        return false;
    }

    m_hasLiveTV = true;
    channels.clear();
    // Parse channels

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

} // namespace vitaplex
