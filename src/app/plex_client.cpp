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

    req.body = "strong=true";

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

bool PlexClient::connectToServer(const std::string& url) {
    brls::Logger::info("Connecting to server: {}", url);

    m_serverUrl = url;
    Application::getInstance().setServerUrl(url);

    HttpClient client;
    HttpRequest req;
    req.url = buildApiUrl("/");
    req.method = "GET";
    req.headers["Accept"] = "application/json";

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
    HttpClient client;
    std::string url = buildApiUrl("/library/sections");
    HttpResponse resp = client.get(url);

    if (resp.statusCode != 200) return false;

    sections.clear();

    // Find all Directory entries
    size_t pos = 0;
    while ((pos = resp.body.find("\"Directory\"", pos)) != std::string::npos) {
        size_t start = resp.body.find('{', pos);
        if (start == std::string::npos) break;

        int braceCount = 1;
        size_t end = start + 1;
        while (braceCount > 0 && end < resp.body.length()) {
            if (resp.body[end] == '{') braceCount++;
            else if (resp.body[end] == '}') braceCount--;
            end++;
        }

        std::string obj = resp.body.substr(start, end - start);

        LibrarySection section;
        section.key = extractJsonValue(obj, "key");
        section.title = extractJsonValue(obj, "title");
        section.type = extractJsonValue(obj, "type");
        section.art = extractJsonValue(obj, "art");
        section.thumb = extractJsonValue(obj, "thumb");
        section.count = extractJsonInt(obj, "count");

        if (!section.key.empty()) {
            sections.push_back(section);
        }

        pos = end;
    }

    brls::Logger::info("Found {} library sections", sections.size());
    return true;
}

bool PlexClient::fetchLibraryContent(const std::string& sectionKey, std::vector<MediaItem>& items) {
    HttpClient client;
    std::string url = buildApiUrl("/library/sections/" + sectionKey + "/all");
    HttpResponse resp = client.get(url);

    if (resp.statusCode != 200) return false;

    items.clear();

    // Parse media items (simplified)
    size_t pos = 0;
    std::string markers[] = {"\"Video\"", "\"Directory\"", "\"Track\"", "\"Photo\""};

    for (const auto& marker : markers) {
        pos = 0;
        while ((pos = resp.body.find(marker, pos)) != std::string::npos) {
            size_t start = resp.body.find('{', pos);
            if (start == std::string::npos) break;

            int braceCount = 1;
            size_t end = start + 1;
            while (braceCount > 0 && end < resp.body.length()) {
                if (resp.body[end] == '{') braceCount++;
                else if (resp.body[end] == '}') braceCount--;
                end++;
            }

            std::string obj = resp.body.substr(start, end - start);

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

            if (!item.ratingKey.empty()) {
                items.push_back(item);
            }

            pos = end;
        }
    }

    brls::Logger::info("Found {} items in library", items.size());
    return true;
}

bool PlexClient::fetchChildren(const std::string& ratingKey, std::vector<MediaItem>& items) {
    HttpClient client;
    std::string url = buildApiUrl("/library/metadata/" + ratingKey + "/children");
    HttpResponse resp = client.get(url);

    if (resp.statusCode != 200) return false;

    items.clear();

    // Parse media items (same logic as fetchLibraryContent)
    size_t pos = 0;
    std::string markers[] = {"\"Video\"", "\"Directory\"", "\"Track\"", "\"Photo\""};

    for (const auto& marker : markers) {
        pos = 0;
        while ((pos = resp.body.find(marker, pos)) != std::string::npos) {
            size_t start = resp.body.find('{', pos);
            if (start == std::string::npos) break;

            int braceCount = 1;
            size_t end = start + 1;
            while (braceCount > 0 && end < resp.body.length()) {
                if (resp.body[end] == '{') braceCount++;
                else if (resp.body[end] == '}') braceCount--;
                end++;
            }

            std::string obj = resp.body.substr(start, end - start);

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

            if (!item.ratingKey.empty()) {
                items.push_back(item);
            }

            pos = end;
        }
    }

    brls::Logger::info("Found {} children", items.size());
    return true;
}

bool PlexClient::fetchMediaDetails(const std::string& ratingKey, MediaItem& item) {
    HttpClient client;
    std::string url = buildApiUrl("/library/metadata/" + ratingKey);
    HttpResponse resp = client.get(url);

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
    HttpClient client;
    std::string url = buildApiUrl("/hubs");
    HttpResponse resp = client.get(url);

    if (resp.statusCode != 200) return false;

    hubs.clear();

    // Parse hubs - look for "Hub" entries
    size_t pos = 0;
    while ((pos = resp.body.find("\"Hub\"", pos)) != std::string::npos) {
        size_t start = resp.body.find('{', pos);
        if (start == std::string::npos) break;

        int braceCount = 1;
        size_t end = start + 1;
        while (braceCount > 0 && end < resp.body.length()) {
            if (resp.body[end] == '{') braceCount++;
            else if (resp.body[end] == '}') braceCount--;
            end++;
        }

        std::string hubObj = resp.body.substr(start, end - start);

        Hub hub;
        hub.title = extractJsonValue(hubObj, "title");
        hub.type = extractJsonValue(hubObj, "type");
        hub.hubIdentifier = extractJsonValue(hubObj, "hubIdentifier");
        hub.key = extractJsonValue(hubObj, "key");
        hub.more = extractJsonBool(hubObj, "more");

        // Parse items inside the hub
        size_t itemPos = 0;
        std::string itemMarkers[] = {"\"Video\"", "\"Directory\"", "\"Track\"", "\"Metadata\""};

        for (const auto& marker : itemMarkers) {
            itemPos = 0;
            while ((itemPos = hubObj.find(marker, itemPos)) != std::string::npos) {
                size_t itemStart = hubObj.find('{', itemPos);
                if (itemStart == std::string::npos) break;

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

                if (!item.ratingKey.empty()) {
                    hub.items.push_back(item);
                }

                itemPos = itemEnd;
            }
        }

        if (!hub.title.empty()) {
            hubs.push_back(hub);
        }

        pos = end;
    }

    brls::Logger::info("Found {} hubs", hubs.size());
    return true;
}

bool PlexClient::fetchContinueWatching(std::vector<MediaItem>& items) {
    HttpClient client;
    std::string url = buildApiUrl("/hubs/continueWatching");
    HttpResponse resp = client.get(url);

    if (resp.statusCode != 200) return false;

    items.clear();

    // Parse media items from "Video" or "Metadata" entries
    size_t pos = 0;
    std::string markers[] = {"\"Video\"", "\"Metadata\""};

    for (const auto& marker : markers) {
        pos = 0;
        while ((pos = resp.body.find(marker, pos)) != std::string::npos) {
            size_t start = resp.body.find('{', pos);
            if (start == std::string::npos) break;

            int braceCount = 1;
            size_t end = start + 1;
            while (braceCount > 0 && end < resp.body.length()) {
                if (resp.body[end] == '{') braceCount++;
                else if (resp.body[end] == '}') braceCount--;
                end++;
            }

            std::string obj = resp.body.substr(start, end - start);

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

            if (!item.ratingKey.empty()) {
                items.push_back(item);
            }

            pos = end;
        }
    }

    brls::Logger::info("Found {} continue watching items", items.size());
    return true;
}

bool PlexClient::fetchRecentlyAdded(std::vector<MediaItem>& items) {
    HttpClient client;
    std::string url = buildApiUrl("/library/recentlyAdded");
    HttpResponse resp = client.get(url);

    if (resp.statusCode != 200) return false;

    items.clear();

    // Parse media items
    size_t pos = 0;
    std::string markers[] = {"\"Video\"", "\"Directory\"", "\"Track\"", "\"Photo\"", "\"Metadata\""};

    for (const auto& marker : markers) {
        pos = 0;
        while ((pos = resp.body.find(marker, pos)) != std::string::npos) {
            size_t start = resp.body.find('{', pos);
            if (start == std::string::npos) break;

            int braceCount = 1;
            size_t end = start + 1;
            while (braceCount > 0 && end < resp.body.length()) {
                if (resp.body[end] == '{') braceCount++;
                else if (resp.body[end] == '}') braceCount--;
                end++;
            }

            std::string obj = resp.body.substr(start, end - start);

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

            if (!item.ratingKey.empty()) {
                items.push_back(item);
            }

            pos = end;
        }
    }

    brls::Logger::info("Found {} recently added items", items.size());
    return true;
}

bool PlexClient::search(const std::string& query, std::vector<MediaItem>& results) {
    HttpClient client;
    std::string url = buildApiUrl("/hubs/search?query=" + HttpClient::urlEncode(query));
    HttpResponse resp = client.get(url);

    if (resp.statusCode != 200) return false;

    results.clear();

    // Parse search results - look for Video, Directory, Track, Photo, Metadata entries
    size_t pos = 0;
    std::string markers[] = {"\"Video\"", "\"Directory\"", "\"Track\"", "\"Photo\"", "\"Metadata\""};

    for (const auto& marker : markers) {
        pos = 0;
        while ((pos = resp.body.find(marker, pos)) != std::string::npos) {
            size_t start = resp.body.find('{', pos);
            if (start == std::string::npos) break;

            int braceCount = 1;
            size_t end = start + 1;
            while (braceCount > 0 && end < resp.body.length()) {
                if (resp.body[end] == '{') braceCount++;
                else if (resp.body[end] == '}') braceCount--;
                end++;
            }

            std::string obj = resp.body.substr(start, end - start);

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

            if (!item.ratingKey.empty()) {
                results.push_back(item);
            }

            pos = end;
        }
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
