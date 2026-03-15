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
    CLIP,
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
    std::string parentRatingKey;   // Season ratingKey (for auto-play-next)
    std::string grandparentRatingKey;  // Show ratingKey (for cross-season auto-play-next)
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

    // Markers (intro/credits) - times in milliseconds
    struct Marker {
        std::string type;   // "intro" or "credits"
        int startTimeMs = 0;
        int endTimeMs = 0;
    };
    std::vector<Marker> markers;

    // Trim heavy fields not needed for grid/list display.
    // Call this on items stored in bulk lists to reduce memory.
    void trimForGrid() {
        // Keep first 60 chars of summary (only used in focus tooltip)
        if (summary.length() > 60) {
            summary = summary.substr(0, 57) + "...";
        }
        // Art is only used for detail view backgrounds
        art.clear();
        art.shrink_to_fit();
        // Stream info not needed for grid cells
        streamUrl.clear();
        streamUrl.shrink_to_fit();
        videoCodec.clear();
        videoCodec.shrink_to_fit();
        audioCodec.clear();
        audioCodec.shrink_to_fit();
        // Part info not needed for grid
        partPath.clear();
        partPath.shrink_to_fit();
        // Markers not needed for grid
        markers.clear();
        markers.shrink_to_fit();
    }
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

// A single EPG program entry
struct ChannelProgram {
    std::string title;
    std::string summary;
    int64_t startTime = 0;
    int64_t endTime = 0;
    std::string ratingKey;   // EPG rating key (e.g., "plex://episode/...")
    std::string metadataKey; // EPG metadata path (e.g., "/tv.plex.providers.epg.cloud:40/metadata/...")
};

// DVR ChannelMapping entry (from official /livetv/dvrs API)
struct ChannelMapping {
    std::string channelKey;         // EPG channel key (e.g., "5cc83d73af4a72001e9b16d7-...")
    std::string deviceIdentifier;   // Device channel number (e.g., "48.1") - used for tuning
    std::string lineupIdentifier;   // Lineup channel identifier (e.g., "002")
};

// Live TV Channel
struct LiveTVChannel {
    std::string ratingKey;
    std::string key;                    // EPG channel key
    std::string title;
    std::string thumb;
    std::string callSign;
    int channelNumber = 0;
    std::string channelIdentifier;      // Device channel ID for DVR tuning (e.g., "2.1")
    std::string currentProgram;
    std::string nextProgram;
    int64_t programStart = 0;
    int64_t programEnd = 0;
    std::vector<ChannelProgram> programs;  // All programs in EPG window, sorted by start time
};

// Genre/Category item with key for filtering
struct GenreItem {
    std::string title;      // Display name
    std::string key;        // Filter key (ID) for API calls
    std::string fastKey;    // Fast filter URL path
};

// Playlist info (from Plex API)
struct Playlist {
    std::string ratingKey;      // Playlist ID
    std::string key;            // Items endpoint (e.g., /playlists/{id}/items)
    std::string title;
    std::string summary;
    std::string thumb;
    std::string composite;      // Composite thumbnail
    std::string playlistType;   // "audio", "video", "photo"
    bool smart = false;         // Smart playlist vs. dumb playlist
    int leafCount = 0;          // Number of items
    int duration = 0;           // Total duration in ms
    int64_t addedAt = 0;
    int64_t updatedAt = 0;
};

// Playlist item (track with playlist-specific info)
struct PlaylistItem {
    std::string playlistItemId;  // Used for remove/move operations
    MediaItem media;             // The actual media item (track)
};

// Stream info from Plex metadata (audio/video/subtitle streams within a Part)
struct PlexStream {
    int id = 0;              // Stream ID (for Plex API stream selection)
    int streamType = 0;      // 1=video, 2=audio, 3=subtitle
    std::string codec;       // e.g., "h264", "aac", "srt"
    std::string displayTitle; // Human-readable title (e.g., "English (AAC Stereo)")
    std::string language;     // Language name (e.g., "English")
    std::string languageCode; // Language code (e.g., "eng")
    bool selected = false;    // Currently selected stream
    int channels = 0;         // Audio channels
    std::string title;        // Track title if any
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
    bool connectToServer(const std::string& url, int timeoutSeconds);
    void logout();

    // Library operations
    bool fetchLibrarySections(std::vector<LibrarySection>& sections);
    bool fetchLibraryContent(const std::string& sectionKey, std::vector<MediaItem>& items, int metadataType = 0, int limit = 0);
    bool fetchSectionRecentlyAdded(const std::string& sectionKey, std::vector<MediaItem>& items);
    bool fetchChildren(const std::string& ratingKey, std::vector<MediaItem>& items);
    bool fetchMediaDetails(const std::string& ratingKey, MediaItem& item);

    // Music artist hubs (albums grouped by type: Albums, Singles, EPs, etc.)
    bool fetchArtistHubs(const std::string& ratingKey, std::vector<Hub>& hubs);

    // Extras (trailers, deleted scenes, featurettes, etc.)
    bool fetchExtras(const std::string& ratingKey, std::vector<MediaItem>& items);

    // Home screen
    bool fetchHubs(std::vector<Hub>& hubs);
    bool fetchContinueWatching(std::vector<MediaItem>& items);
    bool fetchRecentlyAdded(std::vector<MediaItem>& items);
    bool fetchRecentlyAddedByType(MediaType type, std::vector<MediaItem>& items);

    // Search
    bool search(const std::string& query, std::vector<MediaItem>& results);

    // Collections, Genres
    bool fetchCollections(const std::string& sectionKey, std::vector<MediaItem>& collections);
    bool fetchGenres(const std::string& sectionKey, std::vector<std::string>& genres);
    bool fetchGenreItems(const std::string& sectionKey, std::vector<GenreItem>& genres);
    bool fetchByGenre(const std::string& sectionKey, const std::string& genre, std::vector<MediaItem>& items, int metadataType = 0);
    bool fetchByGenreKey(const std::string& sectionKey, const std::string& genreKey, std::vector<MediaItem>& items, int metadataType = 0);

    // Playlists (using official Plex API from developer.plex.tv)
    bool fetchPlaylists(std::vector<MediaItem>& playlists);  // Legacy - returns as MediaItem
    bool fetchMusicPlaylists(std::vector<Playlist>& playlists);  // Get audio playlists
    bool fetchPlaylistItems(const std::string& playlistId, std::vector<PlaylistItem>& items);
    bool createPlaylist(const std::string& title, const std::string& playlistType, Playlist& result);
    bool createPlaylistWithItems(const std::string& title, const std::vector<std::string>& ratingKeys, Playlist& result);
    bool deletePlaylist(const std::string& playlistId);
    bool renamePlaylist(const std::string& playlistId, const std::string& newTitle);
    bool addToPlaylist(const std::string& playlistId, const std::vector<std::string>& ratingKeys);
    bool removeFromPlaylist(const std::string& playlistId, const std::string& playlistItemId);
    bool clearPlaylist(const std::string& playlistId);
    bool movePlaylistItem(const std::string& playlistId, const std::string& playlistItemId, const std::string& afterItemId);

    // Get machine identifier for playlist URIs
    const std::string& getMachineIdentifier() const { return m_currentServer.machineIdentifier; }

    // Playback
    bool getPlaybackUrl(const std::string& ratingKey, std::string& url);
    bool getTranscodeUrl(const std::string& ratingKey, std::string& url, int offsetMs = 0);
    void stopTranscode();  // Stop the current transcode session
    bool updatePlayProgress(const std::string& ratingKey, int timeMs);
    bool reportTimeline(const std::string& ratingKey, const std::string& key,
                        const std::string& state, int timeMs, int durationMs,
                        int playQueueItemID = 0);
    bool markAsWatched(const std::string& ratingKey);
    bool markAsUnwatched(const std::string& ratingKey);

    // Stream selection (Plex API: PUT /library/parts/{partId})
    bool fetchStreams(const std::string& ratingKey, std::vector<PlexStream>& streams, int& partId);
    bool setStreamSelection(int partId, int audioStreamID = -1, int subtitleStreamID = -1);

    // Subtitle search (Plex API: GET /library/metadata/{id}/subtitles)
    struct SubtitleResult {
        int id = 0;
        std::string key;          // URL key to select this subtitle
        std::string codec;        // e.g., "srt"
        std::string displayTitle; // Human-readable title
        std::string language;     // Language name
        std::string languageCode; // Language code (e.g., "eng")
        std::string provider;     // e.g., "opensubtitles"
    };
    bool searchSubtitles(const std::string& ratingKey, const std::string& language,
                         std::vector<SubtitleResult>& results);
    bool selectSearchedSubtitle(const std::string& ratingKey, int partId,
                                const std::string& subtitleKey);

    // Play Queues (server-side queue management for music + video)
    struct PlayQueueItem {
        int playQueueItemID = 0;       // Unique ID within the queue (for move/delete)
        std::string ratingKey;
        std::string title;
        std::string grandparentTitle;  // Artist/show name
        std::string parentTitle;       // Album/season name
        std::string thumb;
        std::string parentThumb;
        std::string grandparentThumb;
        int duration = 0;              // Duration in ms
        int index = 0;                 // Track/episode number
        std::string type;              // "track", "episode", "movie", etc.
        MediaType mediaType = MediaType::UNKNOWN;
    };

    struct PlayQueueContainer {
        int playQueueID = 0;
        int playQueueSelectedItemID = 0;
        int playQueueSelectedItemOffset = 0;
        int playQueueSelectedMetadataItemID = 0;
        bool playQueueShuffled = false;
        std::string playQueueSourceURI;
        int playQueueTotalCount = 0;
        int playQueueVersion = 0;
        std::vector<PlayQueueItem> items;
    };

    // Create a play queue from a library URI (album, show, season, playlist, single item)
    // type: "audio", "video", "photo"
    // key: ratingKey of the item to start playing (optional, defaults to first)
    // shuffle/repeat/continuous: 0 or 1
    bool createPlayQueue(const std::string& uri, const std::string& type,
                         PlayQueueContainer& result,
                         const std::string& key = "",
                         int shuffle = 0, int repeat = 0, int continuous = 0);
    // Create a play queue from a playlist ID
    bool createPlayQueueFromPlaylist(int playlistID, const std::string& type,
                                     PlayQueueContainer& result, int shuffle = 0);
    // Retrieve an existing play queue
    bool getPlayQueue(int playQueueID, PlayQueueContainer& result);
    // Add items to an existing play queue (party mode / play next)
    bool addToPlayQueue(int playQueueID, const std::string& uri, bool playNext = false);
    // Clear all items from a play queue
    bool clearPlayQueue(int playQueueID);
    // Remove a single item from a play queue
    bool removeFromPlayQueue(int playQueueID, int playQueueItemID);
    // Move an item in the play queue (after=0 means move to beginning)
    bool movePlayQueueItem(int playQueueID, int playQueueItemID, int afterItemID = 0);
    // Shuffle the play queue
    bool shufflePlayQueue(int playQueueID, PlayQueueContainer& result);
    // Unshuffle (restore natural order)
    bool unshufflePlayQueue(int playQueueID, PlayQueueContainer& result);

    // Helper: build a library URI for play queue creation
    // e.g., "library://{machineId}/item/%2Flibrary%2Fmetadata%2F{ratingKey}"
    std::string buildPlayQueueURI(const std::string& ratingKey);
    // Build a library URI for a directory (album, season, show)
    // e.g., "library://{machineId}/directory/%2Flibrary%2Fmetadata%2F{ratingKey}%2Fchildren"
    std::string buildPlayQueueDirectoryURI(const std::string& ratingKey);

    // Live TV
    bool fetchLiveTVChannels(std::vector<LiveTVChannel>& channels);
    bool fetchEPGGrid(std::vector<LiveTVChannel>& channelsWithPrograms, int hoursAhead = 4);
    bool tuneLiveTVChannel(const std::string& channelKey, std::string& streamUrl, const std::string& programMetadataKey = "");
    bool tuneLiveTVChannelByKey(const std::string& channelKey, const std::string& epgChannelKey, std::string& streamUrl, const std::string& programMetadataKey = "");
    bool hasLiveTV() const { return m_hasLiveTV; }
    std::string getEpgProviderKey() const { return m_epgProviderKey; }

    // Thumbnail URL
    std::string getThumbnailUrl(const std::string& thumb, int width = 300, int height = 450);

    // Re-authentication: check token validity with plex.tv
    bool validateToken();

    // Handle 401/unauthorized - clears auth state and triggers login redirect
    void handleUnauthorized();

    // Configuration
    void setAuthToken(const std::string& token) { m_authToken = token; }
    const std::string& getAuthToken() const { return m_authToken; }
    void setServerUrl(const std::string& url) { m_serverUrl = url; }
    const std::string& getServerUrl() const { return m_serverUrl; }

    // Public JSON helpers (used by play queue parsing helper)
    std::string extractJsonValuePublic(const std::string& json, const std::string& key) { return extractJsonValue(json, key); }
    int extractJsonIntPublic(const std::string& json, const std::string& key) { return extractJsonInt(json, key); }

    // Public API URL builder (used by Live TV tab for DVR operations)
    std::string buildApiUrlPublic(const std::string& endpoint) { return buildApiUrl(endpoint); }

private:
    PlexClient() = default;
    ~PlexClient() = default;

    std::string buildApiUrl(const std::string& endpoint);
    MediaType parseMediaType(const std::string& typeStr);
    std::string extractJsonValue(const std::string& json, const std::string& key);
    int extractJsonInt(const std::string& json, const std::string& key);
    float extractJsonFloat(const std::string& json, const std::string& key);
    bool extractJsonBool(const std::string& json, const std::string& key);

    // In-place extraction: works on a range [start, end) of a string without creating a substring.
    // Saves CPU and memory for large JSON responses.
    std::string extractJsonValueRange(const std::string& json, size_t start, size_t end, const std::string& key);
    int extractJsonIntRange(const std::string& json, size_t start, size_t end, const std::string& key);
    float extractJsonFloatRange(const std::string& json, size_t start, size_t end, const std::string& key);
    std::string base64Encode(const std::string& input);
    int extractXmlAttr(const std::string& xml, const std::string& attr);
    std::string extractXmlAttrStr(const std::string& xml, const std::string& attr);
    void checkLiveTVAvailability();

    // Returns true if status code is an auth error (401)
    bool isAuthError(int statusCode) const { return statusCode == 401; }

    // Track whether we already triggered reauth to avoid loops
    bool m_reauthTriggered = false;

    std::string m_authToken;
    std::string m_serverUrl;
    std::string m_lastSessionId;  // Last transcode session ID for stop/restart
    PlexServer m_currentServer;
    bool m_hasLiveTV = false;
    std::string m_dvrId;  // DVR ID (key) from GET /livetv/dvrs
    std::vector<std::string> m_deviceIds;  // Device UUIDs from DVR (e.g., "device://tv.plex.grabbers.hdhomerun/...")
    std::string m_lineupUri;  // Lineup URI from DVR (e.g., "lineup://tv.plex.providers.epg.onconnect/...")
    std::vector<ChannelMapping> m_channelMappings;  // Channel mappings from DVR response
    std::string m_epgProviderKey;  // EPG provider key for grid queries (extracted from lineup URI)
};

} // namespace vitaplex
