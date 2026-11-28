/**
 * VitaPlex - music after VitaPlex closes, on the PS4 (utils/background_audio.hpp)
 *
 * Through Shell Companion's PS4 module (sc_companion.prx, at the package's
 * root, loaded into SceShellUI with GoldHEN; client library fetched at build
 * time into third_party/shell_companion, docs/ps4-shell-companion.md):
 *
 *  - As VitaPlex quits with a music queue playing (Application::shutdown),
 *    the queue is written for the shell's player (scWriteQueue), each song a
 *    URL it can stream on its own (the server's MP3 of it, the token in the
 *    query, as the shell sends no headers), and the player told to play it
 *    from the song and the position VitaPlex had reached. The quick menu's
 *    Music section shows it, grouped into tiles by album or, by the setting,
 *    by the playlist it was played from (the songs' album field is what the
 *    section groups by). Its "+" rates the song on the server.
 *  - As VitaPlex opens again, the queue it handed over (its own copy,
 *    kOwnQueue) is opened in the player where the shell got to, and once
 *    that player is ready the shell's stops (takeBackNow), as on the Vita.
 */

#include "utils/background_audio.hpp"

#if defined(__PS4__) && defined(VITAPLEX_PS4_SHELL_COMPANION)

#include "activity/player_activity.hpp"
#include "app/application.hpp"
#include "app/music_queue.hpp"
#include "app/plex_client.hpp"
#include "player/mpv_player.hpp"
#include "utils/async.hpp"
#include "utils/http_client.hpp"

#include <borealis.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "shell_companion.h"

namespace vitaplex {
namespace bgaudio {

namespace {

constexpr const char* kTitleId = "VPLX00002";
// VitaPlex's own copy of the queue it handed over, to open its player on it
// again: what the shell's status says (its song's number, its position) is
// read against it.
constexpr const char* kOwnQueue = "/data/VitaPlex/ps4-shell-queue.txt";
// A status the shell's player wrote longer ago than this is of music over.
constexpr uint64_t kStaleMs = 10 * 1000;

std::atomic<bool> g_open{false};
std::atomic<bool> g_music{true};
std::atomic<int> g_group{0};

struct OwnTrack {
    std::string key, title, artist, album, thumb;
    int duration = 0;
};

uint64_t monotonicMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

// One field of kOwnQueue: tabs and line breaks would split it.
std::string flat(std::string s) {
    for (char& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

std::string serverBase() {
    std::string s = PlexClient::getInstance().getServerUrl();
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// The server's MP3 of a song, every parameter in the query (the shell's
// player sends no headers), a session of its own for each song. Always
// encoded (directStream=0): a copied stream is what the server's transcoder
// fails on for some files, and the shell's player has no fallback.
std::string mp3Url(const std::string& server, const std::string& token, const std::string& key,
                   const std::string& session) {
    return server + "/music/:/transcode/universal/start.mp3?path=" +
           HttpClient::urlEncode("/library/metadata/" + key) +
           "&mediaIndex=0&partIndex=0&directPlay=0&directStream=0&directStreamAudio=0"
           "&hasMDE=1&location=lan&protocol=http&musicBitrate=320&session=" + session +
           "&X-Plex-Client-Identifier=" + HttpClient::urlEncode(PLEX_CLIENT_ID) +
           "&X-Plex-Product=VitaPlex&X-Plex-Platform=" + HttpClient::urlEncode("PlayStation 4") +
           "&X-Plex-Client-Profile-Name=Generic&X-Plex-Client-Profile-Extra=" +
           HttpClient::urlEncode("add-transcode-target(type=musicProfile&context=streaming"
                                 "&protocol=http&container=mp3&audioCodec=mp3)") +
           "&X-Plex-Token=" + token;
}

// A square JPEG of the cover (the shell asks tiles for &w=&h= itself).
std::string coverUrl(const std::string& server, const std::string& token,
                     const std::string& thumb) {
    if (thumb.empty()) return std::string();
    return server + "/photo/:/transcode?url=" + HttpClient::urlEncode(thumb) +
           "&width=300&height=300&minSize=1&upscale=1&format=jpeg&X-Plex-Token=" + token;
}

// The title of the playlist the queue came from, for the Music section's
// tiles by playlist: "" when it cannot be had.
std::string playlistTitle(const std::string& id) {
    if (id.empty()) return std::string();
    std::vector<Playlist> playlists;
    if (!PlexClient::getInstance().fetchMusicPlaylists(playlists)) return std::string();
    for (const auto& p : playlists)
        if (p.ratingKey == id) return p.title;
    return std::string();
}

bool writeOwnQueue(const std::vector<OwnTrack>& tracks) {
    std::ofstream out(kOwnQueue, std::ios::trunc);
    if (!out) return false;
    for (const auto& t : tracks)
        out << flat(t.key) << '\t' << flat(t.title) << '\t' << flat(t.artist) << '\t'
            << flat(t.album) << '\t' << flat(t.thumb) << '\t' << t.duration << '\n';
    return (bool)out;
}

std::vector<OwnTrack> readOwnQueue() {
    std::vector<OwnTrack> tracks;
    std::ifstream in(kOwnQueue);
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string part;
        while (std::getline(ss, part, '\t')) f.push_back(part);
        if (f.size() < 6 || f[0].empty()) continue;
        OwnTrack t;
        t.key = f[0];
        t.title = f[1];
        t.artist = f[2];
        t.album = f[3];
        t.thumb = f[4];
        t.duration = std::atoi(f[5].c_str());
        tracks.push_back(std::move(t));
    }
    return tracks;
}

// At start, off the UI thread: the module in the shell, then the queue it may
// be playing for VitaPlex, opened in the player where it had got to.
void openAndTakeBack() {
    char what[256] = {0};
    const int rc = scOpen(what, sizeof(what));
    g_open = rc == 0;
    if (rc == 0)
        brls::Logger::info("[ps4shell] Shell Companion: {}", what[0] ? what : "running");
    else
        brls::Logger::warning("[ps4shell] Shell Companion not running: {}", what);

    ScQueueStatus st{};
    if (scReadQueueStatus(&st) != 0) return;
    const bool playing = std::strcmp(st.state, "playing") == 0;
    const bool paused = std::strcmp(st.state, "paused") == 0;
    if (!playing && !paused) return;
    const uint64_t now = monotonicMs();
    // The clock starts again at each boot: a status from before one reads
    // later than now (or long ago), and its player is not running.
    if (st.beatMs == 0 || st.beatMs > now + 1000 || now > st.beatMs + kStaleMs ||
        !scPlayerRunning()) {
        brls::Logger::info("[ps4shell] take back: the shell's player is not playing "
                           "(status beat {} ms, now {} ms); music over", st.beatMs, now);
        return;
    }
    std::vector<OwnTrack> own = readOwnQueue();
    if (own.empty() || (int)own.size() != st.count || st.track < 0 ||
        st.track >= (int)own.size()) {
        brls::Logger::warning("[ps4shell] take back: the shell plays a queue of {} (song {}), "
                              "VitaPlex's copy has {}; it plays on there",
                              st.count, st.track, own.size());
        return;
    }
    std::vector<MediaItem> items;
    items.reserve(own.size());
    for (const auto& t : own) {
        MediaItem item;
        item.ratingKey = t.key;
        item.title = t.title;
        item.grandparentTitle = t.artist;
        item.parentTitle = t.album;
        item.thumb = t.thumb;
        item.duration = t.duration * 1000;  // seconds in the copy
        item.type = "track";
        item.mediaType = MediaType::MUSIC_TRACK;
        items.push_back(std::move(item));
    }
    const int index = st.track;
    const long long at = (long long)st.positionMs;
    brls::Logger::info("[ps4shell] take back: the shell {} song {} of {} (\"{}\") at {} s",
                       playing ? "plays" : "has paused", index + 1, items.size(),
                       items[(size_t)index].title, at / 1000);
    brls::sync([items, index, at]() {
        MpvPlayer& p = MpvPlayer::getInstance();
        if (p.isInitialized() && (p.isPlaying() || p.isPaused())) {
            brls::Logger::info("[ps4shell] take back: VitaPlex plays something already");
            return;
        }
        brls::Application::pushActivity(
            PlayerActivity::createWithRestoredQueue(items, index, at, true));
    });
}

}  // namespace

bool ps4Available() { return true; }

void init(int) {
    const AppSettings& s = Application::getInstance().getSettings();
    g_music = s.ps4ShellMusic;
    g_group = s.ps4ShellGroup;
    scInit(kTitleId, "VitaPlex");
    asyncRun([]() { openAndTakeBack(); });
}

void setPs4ShellMusic(bool on) { g_music = on; }
void setPs4ShellGroup(int group) { g_group = group == 1 ? 1 : 0; }

ShellPlace takeBackNow() {
    ShellPlace place;
    ScQueueStatus before{};
    const bool had = scReadQueueStatus(&before) == 0;
    if (scMusicTakeBack() != 0) {
        brls::Logger::warning("[ps4shell] take back: the shell's player could not be told");
        return place;
    }
    // It stops and writes where, once more ("stopped"): up to 1.5 s.
    ScQueueStatus st{};
    bool stopped = false;
    for (int i = 0; i < 60 && !stopped; i++) {
        usleep(25 * 1000);
        stopped = scReadQueueStatus(&st) == 0 && std::strcmp(st.state, "stopped") == 0;
    }
    if (!stopped) {
        if (!had) return place;
        st = before;   // where it last said it was
    }
    place.play = true;
    place.index = st.track;
    place.atMs = (long long)st.positionMs;
    place.paused = had && std::strcmp(before.state, "paused") == 0;
    brls::Logger::info("[ps4shell] take back: the shell stopped at song {} at {} s{}",
                       place.index + 1, place.atMs / 1000, place.paused ? ", paused" : "");
    return place;
}

void handOverOnExit() {
    if (!g_music) return;
    MpvPlayer& player = MpvPlayer::getInstance();
    MusicQueue& queue = MusicQueue::getInstance();
    const auto& items = queue.getQueue();
    const int index = queue.getCurrentIndex();
    if (!player.isInitialized() || !player.isPlaying() || items.empty() || index < 0 ||
        index >= (int)items.size() || items[(size_t)index].mediaType != MediaType::MUSIC_TRACK)
        return;
    if (!g_open && !(scPlayerRunning() && scWatchRunning())) {
        brls::Logger::warning("[ps4shell] hand-over: Shell Companion is not running");
        return;
    }
    const std::string server = serverBase();
    const std::string token = PlexClient::getInstance().getAuthToken();
    if (server.empty() || token.empty()) return;

    // By playlist: one tile for the queue, named after the playlist it was
    // played from; a queue that was not a playlist keeps its albums.
    std::string group;
    if (g_group == 1) group = playlistTitle(queue.getSourcePlaylistId());

    const long long positionMs = (long long)(player.getPosition() * 1000.0);
    const std::string stamp = std::to_string((long long)time(nullptr));

    struct Strings {
        std::string url, title, artist, album, cover, id;
    };
    std::vector<Strings> strings;
    std::vector<OwnTrack> own;
    strings.reserve(items.size());
    own.reserve(items.size());
    for (size_t i = 0; i < items.size(); i++) {
        const QueueItem& t = items[i];
        Strings s;
        s.url = mp3Url(server, token, t.ratingKey, "vp4-" + t.ratingKey + "-" + stamp + "-" +
                                                       std::to_string(i));
        s.title = t.title;
        s.artist = t.artist;
        s.album = group.empty() ? t.album : group;
        s.cover = coverUrl(server, token, t.thumb);
        s.id = t.ratingKey;
        strings.push_back(std::move(s));
        own.push_back({t.ratingKey, t.title, t.artist, t.album, t.thumb, t.duration});
    }
    std::vector<ScTrack> tracks(strings.size());
    for (size_t i = 0; i < strings.size(); i++) {
        ScTrack& tr = tracks[i];
        std::memset(&tr, 0, sizeof(tr));
        tr.url = strings[i].url.c_str();
        tr.title = strings[i].title.c_str();
        tr.artist = strings[i].artist.c_str();
        tr.album = strings[i].album.c_str();
        tr.coverUrl = strings[i].cover.empty() ? nullptr : strings[i].cover.c_str();
        tr.id = strings[i].id.c_str();
    }

    // "+" (R3) rates the song on the server, VitaPlex closed or not; lit for
    // a song already rated.
    const std::string rate = server + "/:/rate?key={id}&identifier=com.plexapp.plugins.library"
                                      "&X-Plex-Token=" + token;
    const std::string saveUrl = rate + "&rating=10";
    const std::string deleteUrl = rate + "&rating=0";
    const std::string containsUrl = server + "/library/metadata/{id}?X-Plex-Token=" + token;
    ScPlus plus;
    std::memset(&plus, 0, sizeof(plus));
    plus.mode = SC_PLUS_HTTP;
    plus.saveUrl = saveUrl.c_str();
    plus.deleteUrl = deleteUrl.c_str();
    plus.method = "PUT";
    plus.containsUrl = containsUrl.c_str();
    plus.containsMatch = "userRating=\"10";

    ScQueue q;
    std::memset(&q, 0, sizeof(q));
    q.index = (uint32_t)index;
    q.positionMs = (uint32_t)std::max(0LL, positionMs);
    q.plus = &plus;
    if (scWriteQueue(&q, tracks.data(), (uint32_t)tracks.size()) != 0) {
        brls::Logger::error("[ps4shell] hand-over: the queue could not be written");
        return;
    }
    if (!writeOwnQueue(own))
        brls::Logger::warning("[ps4shell] hand-over: VitaPlex's copy of the queue could not be "
                              "written; it will not be taken back");

    // VitaPlex's player first, so the two never play at once.
    player.pause();
    if (scMusicPlay() != 0) {
        brls::Logger::error("[ps4shell] hand-over: the shell's player could not be told to play");
        return;
    }
    const RepeatMode repeat = queue.getRepeatMode();
    scCommand(repeat == RepeatMode::ONE ? "repeat=2" : repeat == RepeatMode::ALL ? "repeat=1"
                                                                                  : "repeat=0");
    if (queue.isShuffleEnabled()) scCommand("shuffle=1");
    brls::Logger::info("[ps4shell] hand-over: {} songs to the shell, from \"{}\" at {} s, "
                       "grouped by {}",
                       items.size(), items[(size_t)index].title, positionMs / 1000,
                       group.empty() ? "album" : "playlist \"" + group + "\"");
}

}  // namespace bgaudio
}  // namespace vitaplex

#endif
