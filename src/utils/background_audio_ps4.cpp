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
 *    Music section shows it in tiles, by the setting: by album, or the user's
 *    playlists, a tile each, the queue first (the songs' album field is what
 *    the section groups by; picking a tile plays it). Its "+" rates the song
 *    on the server.
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
// The first song of the list taken back, in the library handed over: the
// shell's track numbers are the library's, VitaPlex's queue is that list.
std::atomic<int> g_takeBackFirst{0};
// The song whose stream began partway (OwnTrack::offsetMs), by its number in
// the library, and by how much: added to the shell's position in it.
std::atomic<int> g_offsetTrack{-1};
std::atomic<long long> g_offsetMs{0};

// By playlist, the other playlists' songs go with the queue, as tiles: up to
// this many, fetched in up to this long as VitaPlex closes.
constexpr size_t kMaxLibrary = 5000;
constexpr uint64_t kFetchBudgetMs = 8000;

struct OwnTrack {
    std::string key, title, artist, album, thumb;
    int duration = 0;
    // Which list of the library it is in: "q", the queue VitaPlex played;
    // "p<id>", a playlist of the Music section's. Each is contiguous.
    std::string list = "q";
    // Where the shell's stream of it begins, for the song playing as VitaPlex
    // closed: the server starts the MP3 there (offset), so the shell's
    // position in it is from here.
    long long offsetMs = 0;
};

// The shell drops a queue line of 768 bytes or more (its MAX_PATH), and every
// song after it shifts; a title's '|' splits its fields (a URL line's
// separator; Shell Companion bgm.c, addTrack and SEP).
constexpr size_t kMaxUrl = 760;
std::string noBar(std::string s) {
    for (char& c : s)
        if (c == '|') c = '/';
    return s;
}

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
                   const std::string& session, long long offsetMs = 0, bool profile = true) {
    const std::string url = server + "/music/:/transcode/universal/start.mp3?path=" +
           HttpClient::urlEncode("/library/metadata/" + key) +
           "&mediaIndex=0&partIndex=0&directPlay=0&directStream=0&directStreamAudio=0"
           "&hasMDE=1&location=lan&protocol=http&musicBitrate=320&session=" + session +
           "&X-Plex-Client-Identifier=" + HttpClient::urlEncode(PLEX_CLIENT_ID) +
           "&X-Plex-Product=VitaPlex&X-Plex-Platform=" + HttpClient::urlEncode("PlayStation 4") +
           (offsetMs >= 1000 ? "&offset=" + std::to_string(offsetMs / 1000) : std::string()) +
           (profile ? "&X-Plex-Client-Profile-Name=Generic&X-Plex-Client-Profile-Extra=" +
                          HttpClient::urlEncode("add-transcode-target(type=musicProfile"
                                                "&context=streaming&protocol=http"
                                                "&container=mp3&audioCodec=mp3)")
                    : std::string()) +
           "&X-Plex-Token=" + token;
    // Too long for the shell: without the profile (start.mp3 asks for MP3).
    if (profile && url.size() >= kMaxUrl) return mp3Url(server, token, key, session, offsetMs, false);
    return url;
}

// A square JPEG of the cover (the shell asks tiles for &w=&h= itself).
std::string coverUrl(const std::string& server, const std::string& token,
                     const std::string& thumb) {
    if (thumb.empty()) return std::string();
    return server + "/photo/:/transcode?url=" + HttpClient::urlEncode(thumb) +
           "&width=300&height=300&minSize=1&upscale=1&format=jpeg&X-Plex-Token=" + token;
}


bool writeOwnQueue(const std::vector<OwnTrack>& tracks) {
    std::ofstream out(kOwnQueue, std::ios::trunc);
    if (!out) return false;
    for (const auto& t : tracks)
        out << flat(t.key) << '\t' << flat(t.title) << '\t' << flat(t.artist) << '\t'
            << flat(t.album) << '\t' << flat(t.thumb) << '\t' << t.duration << '\t'
            << flat(t.list) << '\t' << t.offsetMs << '\n';
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
        if (f.size() > 6 && !f[6].empty()) t.list = f[6];
        if (f.size() > 7) t.offsetMs = std::atoll(f[7].c_str());
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
    // st.track is the song's number in the library handed over (the shell's
    // queue may be one of its tiles, picked there: st.count is that list's);
    // its URL names the song, to be sure the library is VitaPlex's copy.
    std::vector<OwnTrack> own = readOwnQueue();
    const bool known = !own.empty() && st.track >= 0 && st.track < (int)own.size() &&
        std::string(st.file).find(HttpClient::urlEncode("/library/metadata/" +
                                                        own[(size_t)st.track].key) + "&") !=
            std::string::npos;
    if (!known) {
        brls::Logger::warning("[ps4shell] take back: the shell plays song {} of {} ({}), not "
                              "one of VitaPlex's copy ({} songs); it plays on there",
                              st.track, st.count, st.file, own.size());
        return;
    }
    // The list the song is in becomes VitaPlex's queue: the queue handed
    // over, or the playlist picked in the Music section.
    const std::string list = own[(size_t)st.track].list;
    int first = st.track;
    while (first > 0 && own[(size_t)first - 1].list == list) first--;
    int end = st.track + 1;
    while (end < (int)own.size() && own[(size_t)end].list == list) end++;
    g_takeBackFirst = first;
    g_offsetTrack = -1;
    for (size_t i = 0; i < own.size(); i++)
        if (own[i].offsetMs > 0) {
            g_offsetTrack = (int)i;
            g_offsetMs = own[i].offsetMs;
        }
    std::vector<MediaItem> items;
    items.reserve((size_t)(end - first));
    for (int i = first; i < end; i++) {
        const OwnTrack& t = own[(size_t)i];
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
    const int index = st.track - first;
    const long long at = (long long)st.positionMs +
                         (st.track == g_offsetTrack.load() ? g_offsetMs.load() : 0);
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
    place.index = std::max(0, st.track - g_takeBackFirst.load());
    place.atMs = (long long)st.positionMs +
                 (st.track == g_offsetTrack.load() ? g_offsetMs.load() : 0);
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

    const bool byPlaylist = g_group == 1;
    const long long positionMs = (long long)(player.getPosition() * 1000.0);
    const std::string stamp = std::to_string((long long)time(nullptr));

    struct Strings {
        std::string url, title, artist, album, cover, id;
    };
    std::vector<Strings> strings;
    std::vector<OwnTrack> own;
    auto add = [&](const OwnTrack& t, const std::string& album) {
        Strings s;
        s.url = mp3Url(server, token, t.key,
                       "vp4-" + t.key + "-" + stamp + "-" + std::to_string(strings.size()),
                       t.offsetMs);
        s.title = noBar(t.title);
        s.artist = noBar(t.artist);
        s.album = noBar(album);
        s.cover = coverUrl(server, token, t.thumb);
        s.id = t.key;
        strings.push_back(std::move(s));
        own.push_back(t);
    };

    // By playlist, the Music section's tiles are the user's playlists, the
    // queue first (the player's first tile, album0), named after the playlist
    // it was played from, else "Now Playing"; the others after it.
    std::vector<Playlist> playlists;
    const std::string source = queue.getSourcePlaylistId();
    std::string queueName;
    if (byPlaylist) {
        if (!PlexClient::getInstance().fetchMusicPlaylists(playlists)) playlists.clear();
        for (const auto& p : playlists)
            if (!source.empty() && p.ratingKey == source) queueName = p.title;
        if (queueName.empty()) queueName = "Now Playing";
    }
    // The song playing: its stream starts where VitaPlex had got to, whole
    // seconds, as the shell's own #position goes past the end of what it has
    // fetched of a stream and skips the song.
    const long long startMs = std::max(0LL, positionMs) / 1000 * 1000;
    for (size_t i = 0; i < items.size(); i++) {
        const QueueItem& t = items[i];
        add({t.ratingKey, t.title, t.artist, t.album, t.thumb, t.duration, "q",
             (int)i == index ? startMs : 0},
            byPlaylist ? queueName : t.album);
    }

    int lists = 1;
    if (byPlaylist) {
        // Tiles go by name: a second playlist of one name gets a number.
        std::vector<std::string> names{queueName};
        const uint64_t until = monotonicMs() + kFetchBudgetMs;
        for (const auto& p : playlists) {
            if (p.ratingKey == source || strings.size() >= kMaxLibrary) continue;
            if (monotonicMs() > until) {
                brls::Logger::warning("[ps4shell] hand-over: out of time for the playlists; "
                                      "{} of {} in", lists - 1, playlists.size());
                break;
            }
            std::vector<PlaylistItem> entries;
            if (!PlexClient::getInstance().fetchPlaylistItems(p.ratingKey, entries)) continue;
            std::string name = p.title.empty() ? "Playlist" : p.title;
            for (int n = 2; std::find(names.begin(), names.end(), name) != names.end(); n++)
                name = (p.title.empty() ? "Playlist" : p.title) + " (" + std::to_string(n) + ")";
            size_t added = 0;
            for (const auto& e : entries) {
                const MediaItem& m = e.media;
                if (m.mediaType != MediaType::MUSIC_TRACK || m.ratingKey.empty()) continue;
                if (strings.size() >= kMaxLibrary) break;
                add({m.ratingKey, m.title, m.grandparentTitle, m.parentTitle, m.thumb,
                     m.duration / 1000, "p" + p.ratingKey, 0},
                    name);
                added++;
            }
            if (added) {
                names.push_back(name);
                lists++;
            }
        }
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
    q.positionMs = 0;  // the stream itself starts there (startMs)
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
    // By playlist the library holds every playlist; the shell's queue is the
    // first tile's (the queue handed over), picked as the Music section picks
    // one, from the song playing and, still due from the reload, its place.
    const std::string pick = "stop\nreload\npick=album0:" + std::to_string(index) + ":" +
                             std::to_string(index) + "\n";
    if ((byPlaylist ? scCommand(pick.c_str()) : scMusicPlay()) != 0) {
        brls::Logger::error("[ps4shell] hand-over: the shell's player could not be told to play");
        return;
    }
    const RepeatMode repeat = queue.getRepeatMode();
    scCommand(repeat == RepeatMode::ONE ? "repeat=2" : repeat == RepeatMode::ALL ? "repeat=1"
                                                                                  : "repeat=0");
    if (queue.isShuffleEnabled()) scCommand("shuffle=1");
    if (byPlaylist)
        brls::Logger::info("[ps4shell] hand-over: a queue of {} (\"{}\") and {} more playlists, "
                           "{} songs, to the shell, from \"{}\" at {} s",
                           items.size(), queueName, lists - 1, strings.size(),
                           items[(size_t)index].title, positionMs / 1000);
    else
        brls::Logger::info("[ps4shell] hand-over: {} songs to the shell, by album, from \"{}\" "
                           "at {} s", items.size(), items[(size_t)index].title, positionMs / 1000);
}

}  // namespace bgaudio
}  // namespace vitaplex

#endif
