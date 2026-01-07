/**
 * VitaPlex - Plex API Client
 * Handles all communication with Plex servers
 */

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace vitaplex {

// Media types
enum class MediaType {
    UNKNOWN,
    MOVIE,
    SHOW,
    SEASON,
    EPISODE,
    MUSIC_ARTIST,
    MUSIC_ALBUM,
    MUSIC_TRACK,
    PHOTO,
    LIVE_TV_CHANNEL,
    LIVE_TV_PROGRAM
};

// Media item info
struct MediaItem {
    std::string ratingKey;
    std::string key;
    std::string title;
    std::string summary;
    std::string thumb;
    std::string art;
    std::string type;
    MediaType mediaType = MediaType::UNKNOWN;
    int year = 0;
    int duration = 0;
    int viewOffset = 0;
    float rating = 0.0f;
    std::string contentRating;
    std::string studio;
    bool watched = false;

    // For episodes
    std::string grandparentTitle;
    std::string parentTitle;
    std::string grandparentThumb;  // Series/show poster for episodes
    std::string parentThumb;       // Season poster for episodes
    int parentIndex = 0;
    int index = 0;
    int seasonNumber = 0;
    int episodeNumber = 0;

    // For seasons/albums
    int leafCount = 0;
    int viewedLeafCount = 0;

    // Album subtype (album, single, ep, compilation, soundtrack, live, etc.)
    std::string subtype;

    // Stream info
    std::string streamUrl;
    std::string videoCodec;
    std::string audioCodec;
    int videoWidth = 0;
    int videoHeight = 0;

    // For downloads - media part path on server
    std::string partPath;
    int64_t partSize = 0;
};

// Library section info
struct LibrarySection {
    std::string key;
    std::string title;
    std::string type;
    std::string art;
    std::string thumb;
    int count = 0;
};

// Server connection info
struct ServerConnection {
    std::string uri;
    bool local = false;
    bool relay = false;
};

// Plex server info
struct PlexServer {
    std::string name;
    std::string address;  // Primary address (local preferred)
    int port = 32400;
    std::string machineIdentifier;
    std::string accessToken;
    std::vector<ServerConnection> connections;  // All available connections
};

// PIN authentication info
struct PinAuth {
    int id = 0;
    std::string code;
    std::string authToken;
    bool expired = false;
    int expiresIn = 0;
    bool useJwt = false;  // Whether this PIN uses JWT authentication
};

// Hub (for home screen)
struct Hub {
    std::string title;
    std::string type;
    std::string hubIdentifier;
    std::string key;
    std::vector<MediaItem> items;
    bool more = false;
};

// Live TV Channel
struct LiveTVChannel {
    std::string ratingKey;
    std::string key;
    std::string title;
    std::string thumb;
    std::string callSign;
    int channelNumber = 0;
    std::string currentProgram;
    std::string nextProgram;
    int64_t programStart = 0;
    int64_t programEnd = 0;
};

// Genre/Category item with key for filtering
struct GenreItem {
    std::string title;      // Display name
    std::string key;        // Filter key (ID) for API calls
    std::string fastKey;    // Fast filter URL path
};

/**
 * Plex API Client singleton
 */
class PlexClient {
public:
    static PlexClient& getInstance();

    // Authentication
    bool login(const std::string& username, const std::string& password);
    bool requestPin(PinAuth& pinAuth);
    bool checkPin(PinAuth& pinAuth);
    bool refreshToken();  // JWT token refresh (call before 7-day expiry)
    bool fetchServers(std::vector<PlexServer>& servers);  // Get user's servers from plex.tv
    bool connectToServer(const std::string& url);
    void logout();

    // Library operations
    bool fetchLibrarySections(std::vector<LibrarySection>& sections);
    bool fetchLibraryContent(const std::string& sectionKey, std::vector<MediaItem>& items);
    bool fetchSectionRecentlyAdded(const std::string& sectionKey, std::vector<MediaItem>& items);
    bool fetchChildren(const std::string& ratingKey, std::vector<MediaItem>& items);
    bool fetchMediaDetails(const std::string& ratingKey, MediaItem& item);

    // Home screen
    bool fetchHubs(std::vector<Hub>& hubs);
    bool fetchContinueWatching(std::vector<MediaItem>& items);
    bool fetchRecentlyAdded(std::vector<MediaItem>& items);
    bool fetchRecentlyAddedByType(MediaType type, std::vector<MediaItem>& items);

    // Search
    bool search(const std::string& query, std::vector<MediaItem>& results);

    // Collections, Playlists, Genres
    bool fetchCollections(const std::string& sectionKey, std::vector<MediaItem>& collections);
    bool fetchPlaylists(std::vector<MediaItem>& playlists);
    bool fetchGenres(const std::string& sectionKey, std::vector<std::string>& genres);
    bool fetchGenreItems(const std::string& sectionKey, std::vector<GenreItem>& genres);
    bool fetchByGenre(const std::string& sectionKey, const std::string& genre, std::vector<MediaItem>& items);
    bool fetchByGenreKey(const std::string& sectionKey, const std::string& genreKey, std::vector<MediaItem>& items);

    // Playback
    bool getPlaybackUrl(const std::string& ratingKey, std::string& url);
    bool getTranscodeUrl(const std::string& ratingKey, std::string& url, int offsetMs = 0);
    bool updatePlayProgress(const std::string& ratingKey, int timeMs);
    bool markAsWatched(const std::string& ratingKey);
    bool markAsUnwatched(const std::string& ratingKey);

    // Live TV
    bool fetchLiveTVChannels(std::vector<LiveTVChannel>& channels);
    bool fetchEPGGrid(std::vector<LiveTVChannel>& channelsWithPrograms, int hoursAhead = 4);
    bool hasLiveTV() const { return m_hasLiveTV; }

    // Thumbnail URL
    std::string getThumbnailUrl(const std::string& thumb, int width = 300, int height = 450);

    // Configuration
    void setAuthToken(const std::string& token) { m_authToken = token; }
    const std::string& getAuthToken() const { return m_authToken; }
    void setServerUrl(const std::string& url) { m_serverUrl = url; }
    const std::string& getServerUrl() const { return m_serverUrl; }

private:
    PlexClient() = default;
    ~PlexClient() = default;

    std::string buildApiUrl(const std::string& endpoint);
    MediaType parseMediaType(const std::string& typeStr);
    std::string extractJsonValue(const std::string& json, const std::string& key);
    int extractJsonInt(const std::string& json, const std::string& key);
    float extractJsonFloat(const std::string& json, const std::string& key);
    bool extractJsonBool(const std::string& json, const std::string& key);
    std::string base64Encode(const std::string& input);
    int extractXmlAttr(const std::string& xml, const std::string& attr);
    std::string extractXmlAttrStr(const std::string& xml, const std::string& attr);
    void checkLiveTVAvailability();

    std::string m_authToken;
    std::string m_serverUrl;
    PlexServer m_currentServer;
    bool m_hasLiveTV = false;
};

} // namespace vitaplex
