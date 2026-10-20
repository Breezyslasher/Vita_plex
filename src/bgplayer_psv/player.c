/**
 * VitaPlex BG's background player: the playback.
 *
 * Plays the queue the app handed over (bgplayer.h) through the shell's music
 * service. The shell plays files, not links, and plays on beside games and
 * apps with extended memory (docs/DESIGN_NOTES.md). So each song is put on the
 * memory card first: the server's own file when the shell decodes it (MP3, AAC,
 * WAV), else the server's MP3 of it with a length header in front, which the
 * server's lacks; a song the app had downloaded plays from where it is. The
 * next song downloads while one plays, and goes over as it ends.
 *
 * This is the background test's player (src/bgtest_psv/player_test.c) with a
 * queue in place of two songs from the library. What the console showed about
 * each step is told there and in the design notes.
 *
 * Requests go over https as VitaPlex makes them, with curl, mbedtls and the CA
 * bundle the app copies to ux0:data. The sign-in token comes from VitaPlex's
 * settings.json and goes to the server in the requests' headers, and no URL is
 * logged past its path.
 */

#include "bgplayer.h"
#include "mp3_info.h"
#include "program.h"
#include "qm_link.h"

#include "ShellAudio.h"

#include <psp2/appmgr.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <curl/curl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sceAppMgrAcquireBgmPortWithPriority(int priority);
int sceAppMgrAcquireBgmPort(void);
int sceAppMgrReleaseBgmPort(void);
// sceAppMgrAcquireBgmPortForMusicPlayer, by NID (src/utils/appmgr_bgm_stub.S),
// whose arguments nobody has written down; one 0 is passed.
int vitaplexAcquireBgmPortForMusicPlayer(int unknown);

// What ElevenMPV-A asks for while SceShell decodes.
#define SHELL_PORT_PRIORITY 0x80

// How the queue says to take the BGM port (bgplayer.h); 0x80 unless it says.
static char g_portHow[16] = BGPLAYER_PORT_PRIORITY;

#define MB (1024 * 1024)

// A song as the queue names it.
typedef struct Entry {
    char key[16];
    int durationMs;
    char local[112];   // a download of it, or ""
    char title[64];
    char artist[48];
    char album[48];    // for the quick menu's row
} Entry;

// A song made ready for the shell.
typedef struct Song {
    int index;          // its place in the queue
    char key[16];
    char title[96];
    char artist[96];
    char partKey[192];
    char ext[8];        // of the server's file: "mp3", "m4a", ...
    int durationMs;
    long long size;
    int transcode;      // fetched as the server's MP3 of it
    char path[128];     // what the shell gets
    int own;            // in BGPLAYER_SONGS, to be removed once done with
} Song;

static char g_server[256];
static char g_token[160];
static char g_caPath[96] = "app0:resources/cacert.pem";

// The queue, as the app wrote it (loadQueue).
static Entry* g_queue;
static int g_count, g_index, g_repeat;
static unsigned g_startMs;       // where the song at g_index goes on from
static double g_queueWritten;
static char g_readyFile[128];    // the app's file of the song at g_index

// The song with the shell, for the status and for a pause: set by the player
// thread, read by the watcher's, under g_statusLock.
static SceUID g_statusLock = -1;
static struct {
    int index;
    char key[16];
    int durationMs;
    unsigned startMs;   // where it was to go on from
    int started;        // seen playing, so the shell's clock is its
} g_now;

static volatile int g_shellUp, g_portHeld;
static volatile int g_handedOver;   // a song is with the shell
// A queue is playing, or getting its next song ready (playQueue).
static volatile int g_active;
// For good: asked to quit.
static volatile int g_stopped;
// Its app has the music (playerPause): the song and where it had got to, and
// whether the queue was playing then; and whether the app asks for the music
// again (playerPlay).
static volatile int g_paused, g_playAsked;
static volatile int g_pausedIndex, g_pauseWasPlaying;
static volatile unsigned g_pausedAtMs;
// The download running is not wanted any more.
static volatile int g_cancel;

// The quick menu's Music row through VitaPlex's plugin (qm_link.h), when
// the queue says so: the player's while a song of the queue is with the
// shell, and the system's again as the music stops (stopShell).
static int g_qmEnabled;           // the queue's qm=1
static volatile int g_qmActive;   // the plugin has the row for this player
// A press on the row, from main.c's watcher, for watchSong to act on; and
// whether the row's play/pause stopped the shell, which then stays stopped.
static volatile int g_menuPress;
static volatile int g_menuPaused;

// The next song, downloading on a thread of its own while one plays.
static Song g_next;
static volatile int g_nextIndex = -1;
static volatile int g_nextState;   // 1 downloading, 2 ready, -1 failed
static SceUID g_nextThread = -1;

static double now(void) { return sceKernelGetSystemTimeWide() / 1e6; }

static void mmss(char* buf, size_t size, unsigned ms) {
    snprintf(buf, size, "%u:%02u", ms / 60000, ms / 1000 % 60);
}

static void lockStatus(void) {
    if (g_statusLock >= 0) sceKernelLockMutex(g_statusLock, 1, NULL);
}
static void unlockStatus(void) {
    if (g_statusLock >= 0) sceKernelUnlockMutex(g_statusLock, 1);
}

// A file in place of path, written beside it: rename does not replace a file
// on every target (platform_common.cpp).
static int moveIn(const char* from, const char* to) {
    remove(to);
    return rename(from, to);
}

static int readable(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

// ── The status ───────────────────────────────────────────────────────────

static void writeStatus(const char* state, unsigned positionMs, int wasPlaying) {
    lockStatus();
    // The player's thread, a moment behind a pause the watcher has just
    // written down, would otherwise say it plays.
    if (strcmp(state, BGPLAYER_STATE_PLAYING) == 0 && (g_paused || g_stopped)) {
        unlockStatus();
        return;
    }
    FILE* f = fopen(BGPLAYER_STATUS ".part", "w");
    if (f) {
        fprintf(f,
                "written=%.6f\nqueue=%.6f\nindex=%d\nratingKey=%s\npositionMs=%u\ndurationMs=%d\n"
                "state=%s\nwasPlaying=%d\nend\n",
                systemSecs(), g_queueWritten, g_now.index, g_now.key, positionMs, g_now.durationMs,
                state, wasPlaying);
        if (fclose(f) == 0) moveIn(BGPLAYER_STATUS ".part", BGPLAYER_STATUS);
    }
    unlockStatus();
}

// ── VitaPlex's settings ──────────────────────────────────────────────────

// The string value of "key" in settings.json, which VitaPlex writes as a flat
// object with \ and " escaped. Its length, or -1.
static int jsonString(const char* json, const char* key, char* out, size_t size) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* v = strstr(json, pat);
    if (!v) return -1;
    v = strchr(v + strlen(pat), ':');
    if (v) v = strchr(v, '"');
    if (!v) return -1;
    v++;
    size_t i = 0;
    while (*v && *v != '"' && i + 1 < size) {
        if (*v == '\\' && v[1]) v++;
        out[i++] = *v++;
    }
    out[i] = '\0';
    return *v == '"' ? (int)i : -1;
}

// The server and the token the app uses. A local server may admit VitaPlex
// with no token. 0 when there is a server to ask.
static int readSettings(void) {
    FILE* f = fopen(BGPLAYER_DIR "settings.json", "r");
    if (!f) {
        logLine("no VitaPlex settings file: only songs downloaded to the card can play");
        return -1;
    }
    static char buf[32 * 1024];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (jsonString(buf, "serverUrl", g_server, sizeof(g_server)) <= 0) {
        logLine("no server in VitaPlex's settings: only songs downloaded to the card can play");
        return -1;
    }
    if (jsonString(buf, "authToken", g_token, sizeof(g_token)) < 0) g_token[0] = '\0';
    size_t len = strlen(g_server);
    while (len > 0 && g_server[len - 1] == '/') g_server[--len] = '\0';
    logLine("server %s, %s", g_server, g_token[0] ? "signed in" : "with no token");
    return 0;
}

// ── Requests ─────────────────────────────────────────────────────────────

typedef struct Body {
    char* data;
    size_t len, cap;
} Body;

static size_t toBody(char* p, size_t size, size_t n, void* user) {
    Body* b = (Body*)user;
    const size_t add = size * n;
    if (b->len + add + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64 * 1024;
        while (cap < b->len + add + 1) cap *= 2;
        if (cap > 2 * MB) return 0;   // ends the transfer
        char* d = (char*)realloc(b->data, cap);
        if (!d) return 0;
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, add);
    b->len += add;
    b->data[b->len] = '\0';
    return add;
}

// A request to path on the server, as VitaPlex makes it: the token and the
// client's names in headers, https checked against the CA bundle.
static CURL* request(const char* path, struct curl_slist** headers) {
    char url[1536], token[200];
    snprintf(url, sizeof(url), "%s%s", g_server, path);
    struct curl_slist* h = NULL;
    if (g_token[0]) {
        snprintf(token, sizeof(token), "X-Plex-Token: %s", g_token);
        h = curl_slist_append(h, token);
    }
    h = curl_slist_append(h, "X-Plex-Client-Identifier: vitaplex-bgplayer");
    h = curl_slist_append(h, "X-Plex-Product: VitaPlex");
    h = curl_slist_append(h, "X-Plex-Platform: PlayStation Vita");
    h = curl_slist_append(h, "X-Plex-Device: PS Vita");
    h = curl_slist_append(h, "X-Plex-Device-Name: PS Vita");
    *headers = h;

    CURL* c = curl_easy_init();
    if (!c) return NULL;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "VitaPlex BG Player");
    curl_easy_setopt(c, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_CAINFO, g_caPath);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    return c;
}

// The CA bundle, without which no request works: the copy the app leaves in
// ux0:data as it hands over, since app0: goes from under a program whose app
// goes after it has started; else the app's own, while app0: is its.
static void findCaBundle(void) {
    if (readable(BGPLAYER_CA_BUNDLE)) {
        snprintf(g_caPath, sizeof(g_caPath), "%s", BGPLAYER_CA_BUNDLE);
    } else if (!readable(g_caPath)) {
        snprintf(g_caPath, sizeof(g_caPath), "ux0:app/%s/resources/cacert.pem",
                 BGPLAYER_APP_TITLEID);
        if (!readable(g_caPath)) {
            logLine("no CA bundle at %s, app0: or the app's directory: https will fail",
                    BGPLAYER_CA_BUNDLE);
            return;
        }
    }
    logLine("CA bundle: %s", g_caPath);
}

// The part of a path before its query, for the log.
static const char* logPath(const char* path, char* buf, size_t size) {
    const size_t n = strcspn(path, "?");
    snprintf(buf, size, "%.*s", (int)(n < size - 1 ? n : size - 1), path);
    return buf;
}

// GETs path into *body. The HTTP status, or -1 when the request failed.
static int getBody(const char* path, const char* accept, Body* body) {
    struct curl_slist* h = NULL;
    CURL* c = request(path, &h);
    if (!c) return -1;
    if (accept) {
        char a[64];
        snprintf(a, sizeof(a), "Accept: %s", accept);
        h = curl_slist_append(h, a);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    }
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, toBody);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    const CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    curl_slist_free_all(h);
    if (rc != CURLE_OK) {
        char p[96];
        logLine("GET %s failed: %s", logPath(path, p, sizeof(p)), curl_easy_strerror(rc));
        return -1;
    }
    return (int)status;
}

// ── A song's details ─────────────────────────────────────────────────────

// A character reference (&#8217; or &#x2019;) at p, as UTF-8 into out (which
// has room for 4 bytes). The bytes written, with *len its length; 0 if none.
static size_t charRef(const char* p, char* out, size_t* len) {
    if (p[0] != '&' || p[1] != '#') return 0;
    const int hex = p[2] == 'x' || p[2] == 'X';
    char* end;
    const unsigned long c = strtoul(p + (hex ? 3 : 2), &end, hex ? 16 : 10);
    if (*end != ';' || end == p + (hex ? 3 : 2) || c == 0 || c > 0x10FFFF) return 0;
    *len = (size_t)(end - p) + 1;
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (c >> 18));
    out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

// The value of attribute `name` in the XML element that starts at el and ends
// at the next '>', with the predefined entities and character references
// decoded. 0 when found.
static int xmlAttr(const char* el, const char* name, char* out, size_t size) {
    static const char* const entities[][2] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}};
    const char* end = strchr(el, '>');
    char pat[48];
    snprintf(pat, sizeof(pat), " %s=\"", name);
    const char* p = strstr(el, pat);
    if (!end || !p || p > end) return -1;
    p += strlen(pat);
    size_t i = 0;
    while (p < end && *p != '"' && i + 1 < size) {
        size_t skip = 0;
        char utf8[4];
        const size_t n = i + 5 <= size ? charRef(p, utf8, &skip) : 0;
        if (n) {
            memcpy(out + i, utf8, n);
            i += n;
        } else if (*p == '&') {
            for (size_t e = 0; e < sizeof(entities) / sizeof(entities[0]); e++)
                if (strncmp(p, entities[e][0], strlen(entities[e][0])) == 0) {
                    out[i++] = entities[e][1][0];
                    skip = strlen(entities[e][0]);
                    break;
                }
        }
        if (skip) {
            p += skip;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return 0;
}

// The shell decodes MP3, AAC (M4A) and WAV; the rest goes as the server's MP3.
static int shellDecodes(const char* ext) {
    return strcmp(ext, "mp3") == 0 || strcmp(ext, "m4a") == 0 || strcmp(ext, "aac") == 0 ||
           strcmp(ext, "wav") == 0;
}

// The extension of path, in lower case, with mp4 as m4a.
static void extOf(const char* path, char* out, size_t size) {
    const char* dot = strrchr(path, '.');
    const char* slash = strrchr(path, '/');
    snprintf(out, size, "%s", dot && (!slash || dot > slash) ? dot + 1 : "");
    for (char* e = out; *e; e++)
        if (*e >= 'A' && *e <= 'Z') *e += 'a' - 'A';
    if (strcmp(out, "mp4") == 0) snprintf(out, size, "m4a");
}

// The Track element at p: its title, artist, length and first part. 0 when it
// has a part to fetch.
static int trackAt(const char* p, Song* s) {
    char num[24];
    xmlAttr(p, "title", s->title, sizeof(s->title));
    xmlAttr(p, "grandparentTitle", s->artist, sizeof(s->artist));
    if (xmlAttr(p, "duration", num, sizeof(num)) == 0) s->durationMs = atoi(num);
    const char* next = strstr(p + 1, "<Track ");
    const char* part = strstr(p, "<Part ");
    if (!part || (next && part > next)) return -1;
    xmlAttr(part, "key", s->partKey, sizeof(s->partKey));
    if (xmlAttr(part, "size", num, sizeof(num)) == 0) s->size = atoll(num);
    extOf(s->partKey, s->ext, sizeof(s->ext));
    return s->partKey[0] ? 0 : -1;
}

// The song's details from the server, as the library gives them. 0, or -1
// when the server did not answer, and -2 when it has nothing to play there.
static int fetchDetails(Song* s, const char* label) {
    char path[48];
    snprintf(path, sizeof(path), "/library/metadata/%s", s->key);
    Body b = {0};
    const int status = getBody(path, NULL, &b);
    const char* p = b.data ? strstr(b.data, "<Track ") : NULL;
    const int ok = p && trackAt(p, s) == 0;
    free(b.data);
    if (ok) return 0;
    if (status < 0) {
        logLine("%s: track %s: no answer from the server", label, s->key);
        return -1;
    }
    logLine("%s: track %s: HTTP %d, %s", label, s->key, status,
            status == 200 ? "no part to play in it" : "nothing to play");
    return -2;
}

// ── Downloading ──────────────────────────────────────────────────────────

typedef struct Progress {
    const char* label;
    double lastLog;
} Progress;

// Stops the transfer when the player is paused, stopped, or done with it.
static int onProgress(void* user, curl_off_t total, curl_off_t got, curl_off_t ut, curl_off_t un) {
    (void)total;
    (void)ut;
    (void)un;
    Progress* p = (Progress*)user;
    if (g_cancel || g_paused || g_stopped) return 1;
    const double t = now();
    if (t - p->lastLog >= 10.0) {
        p->lastLog = t;
        logLine("%s: %.1f MB so far", p->label, got / (double)MB);
    }
    return 0;
}

// The server's MP3 of a song, as VitaPlex asks for it: a decision first, then
// start.mp3 with the same session (plex_client.cpp, resolveTranscodeUrl).
static void transcodePath(const Song* s, char* out, size_t size, int start) {
    static unsigned session;
    char metadata[48];
    snprintf(metadata, sizeof(metadata), "/library/metadata/%s", s->key);
    char* path = curl_easy_escape(NULL, metadata, 0);
    char* extra = curl_easy_escape(NULL,
                                   "add-transcode-target(type=musicProfile&context=streaming"
                                   "&protocol=http&container=mp3&audioCodec=mp3)",
                                   0);
    char* token = curl_easy_escape(NULL, g_token, 0);
    if (!start) session++;
    snprintf(out, size,
             "/music/:/transcode/universal/%s?path=%s&mediaIndex=0&partIndex=0&directPlay=0"
             "&directStream=1&directStreamAudio=1&hasMDE=1&location=lan&audioBoost=100"
             "&audioChannelCount=2&protocol=http&musicBitrate=320&session=bgplayer-%s-%u"
             "%s%s&X-Plex-Client-Profile-Name=Generic&X-Plex-Client-Profile-Extra=%s",
             start ? "start.mp3" : "decision", path ? path : "", s->key, session,
             token && token[0] ? "&X-Plex-Token=" : "", token ? token : "", extra ? extra : "");
    curl_free(path);
    curl_free(extra);
    curl_free(token);
}

// A server's MP3 with no length header has the shell work its length out
// from the first frame's bitrate: 19:17 for a 2:51 song (log 1a1cef7f). The
// same frames go to `to` behind a Xing frame giving their count, which gave
// the shell the right length (log e822493b). 0 when `to` is written.
static int giveHeader(const char* label, const char* from, const char* to) {
    Mp3Info m;
    if (mp3Inspect(from, &m) != 0) {
        logLine("%s: no MP3 frame found in what the server sent", label);
        return -1;
    }
    char len[16], guess[16];
    mmss(len, sizeof(len), (unsigned)(m.seconds * 1000));
    mmss(guess, sizeof(guess), (unsigned)(mp3FirstFrameGuess(&m) * 1000));
    if (m.header[0]) {
        logLine("%s: the server's MP3 has a %s header already: %s", label, m.header, len);
        return moveIn(from, to) == 0 ? 0 : -1;
    }
    char part[140];
    snprintf(part, sizeof(part), "%s.part", to);
    if (mp3WriteWithHeader(from, &m, part) != 0 || moveIn(part, to) != 0) {
        remove(part);
        logLine("%s: could not write the header; the shell gets the MP3 as the server sent it",
                label);
        return moveIn(from, to) == 0 ? 0 : -1;
    }
    remove(from);
    logLine("%s: %ld frames make %s; the first frame's %d kbps alone would make it %s, so it "
            "goes to the shell behind a header",
            label, m.frames, len, m.firstKbps, guess);
    return 0;
}

// The song into s->path, through a file beside it that is moved in once the
// whole song has come. 0 when it is there; -1 when it failed, -2 when the
// download was stopped (paused, stopped, or not wanted any more).
static int download(Song* s, const char* label) {
    char path[1536];
    if (s->transcode) {
        char decision[1536];
        transcodePath(s, decision, sizeof(decision), 0);
        Body b = {0};
        const int status = getBody(decision, "application/json", &b);
        char text[96] = "";
        if (b.data) jsonString(b.data, "generalDecisionText", text, sizeof(text));
        logLine("%s: transcode decision HTTP %d: %s", label, status, text);
        free(b.data);
        transcodePath(s, path, sizeof(path), 1);
    } else {
        snprintf(path, sizeof(path), "%s?download=1", s->partKey);
    }

    char part[140];
    snprintf(part, sizeof(part), "%s.part", s->path);
    FILE* f = fopen(part, "wb");
    if (!f) {
        logLine("%s: cannot write %s", label, part);
        return -1;
    }
    static char fbuf[64 * 1024];   // one download at a time
    setvbuf(f, fbuf, _IOFBF, sizeof(fbuf));

    struct curl_slist* h = NULL;
    CURL* c = request(path, &h);
    if (!c) {
        fclose(f);
        remove(part);
        return -1;
    }
    Progress prog = {label, now()};
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);   // curl fwrites to it by default
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &prog);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1000L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 64L * 1024);

    char lp[96];
    logLine("%s: downloading %s", label, logPath(path, lp, sizeof(lp)));
    const double t0 = now();
    const CURLcode rc = curl_easy_perform(c);
    const double took = now() - t0;
    long status = 0;
    curl_off_t bytes = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(c, CURLINFO_SIZE_DOWNLOAD_T, &bytes);
    curl_easy_cleanup(c);
    curl_slist_free_all(h);
    const int closed = fclose(f) == 0;

    if (rc == CURLE_ABORTED_BY_CALLBACK) {
        remove(part);
        logLine("%s: download stopped after %.2f MB, %s", label, bytes / (double)MB,
                g_stopped ? "the player stopping" : g_paused ? "its app taking the music back"
                                                  : "the song not wanted now");
        return -2;
    }
    logLine("%s: %s, HTTP %ld, %.2f MB in %.1fs (%.0f KB/s)", label,
            rc == CURLE_OK ? "downloaded" : curl_easy_strerror(rc), status, bytes / (double)MB,
            took, took > 0 ? bytes / 1024.0 / took : 0.0);
    if (rc != CURLE_OK || status != 200 || bytes <= 0 || !closed) {
        remove(part);
        return -1;
    }
    if (s->transcode) {
        char sent[140];
        snprintf(sent, sizeof(sent), "%s.sent", s->path);
        if (moveIn(part, sent) != 0 || giveHeader(label, sent, s->path) != 0) {
            remove(sent);
            return -1;
        }
        return 0;
    }
    return moveIn(part, s->path) == 0 ? 0 : -1;
}

// The song at index made ready for the shell: a download of it the app made
// (or the file the app made ready), else one already in BGPLAYER_SONGS, else
// fetched now. 0 when it is; -1 when it failed and may be tried again, -2
// when it cannot play, -3 when stopped part-way.
static int prepareSong(int index, Song* s, const char* readyFile) {
    const Entry* e = &g_queue[index];
    memset(s, 0, sizeof(*s));
    s->index = index;
    snprintf(s->key, sizeof(s->key), "%s", e->key);
    snprintf(s->title, sizeof(s->title), "%s", e->title);
    snprintf(s->artist, sizeof(s->artist), "%s", e->artist);
    s->durationMs = e->durationMs;
    char label[40];
    snprintf(label, sizeof(label), "song %d of %d", index + 1, g_count);

    char ext[8];
    extOf(e->local, ext, sizeof(ext));
    if (e->local[0] && shellDecodes(ext) && readable(e->local)) {
        snprintf(s->path, sizeof(s->path), "%s", e->local);
        logLine("%s: \"%s\" by %s, the app's download (%s)", label, s->title, s->artist, ext);
        return 0;
    }
    if (readyFile && readyFile[0] && readable(readyFile)) {
        snprintf(s->path, sizeof(s->path), "%s", readyFile);
        s->own = 1;
        logLine("%s: \"%s\" by %s, the file the app made ready", label, s->title, s->artist);
        return 0;
    }
    if (!g_server[0]) {
        logLine("%s: \"%s\": not downloaded, and no server to fetch it from", label, s->title);
        return -2;
    }
    const int details = fetchDetails(s, label);
    if (details < 0) return details;
    s->transcode = !shellDecodes(s->ext);
    snprintf(s->path, sizeof(s->path), "%s/%s.%s", BGPLAYER_SONGS, s->key,
             s->transcode ? "mp3" : s->ext);
    s->own = 1;
    char d[16];
    mmss(d, sizeof(d), (unsigned)s->durationMs);
    if (readable(s->path)) {
        logLine("%s: \"%s\" by %s, %s, already on the card", label, s->title, s->artist, d);
        return 0;
    }
    logLine("%s: \"%s\" by %s, %s, %s file of %.1f MB; fetched %s", label, s->title, s->artist,
            d, s->ext, s->size / (double)MB, s->transcode ? "as the server's MP3" : "as it is");
    const int rc = download(s, label);
    return rc == -2 ? -3 : rc;
}

static void removeSong(const Song* s) {
    if (s->own && s->path[0]) remove(s->path);
}

// Every file in BGPLAYER_SONGS but keep: songs a player before this one left
// when the system ended it.
static void cleanSongs(const char* keep) {
    const SceUID d = sceIoDopen(BGPLAYER_SONGS);
    if (d < 0) return;
    SceIoDirent e;
    int removed = 0;
    for (;;) {
        memset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0) break;
        if (e.d_name[0] == '.') continue;
        char path[sizeof(BGPLAYER_SONGS) + sizeof(e.d_name)];
        snprintf(path, sizeof(path), "%s/%s", BGPLAYER_SONGS, e.d_name);
        if (keep && strcmp(path, keep) == 0) continue;
        if (remove(path) == 0) removed++;
    }
    sceIoDclose(d);
    if (removed) logLine("removed %d file(s) left in %s", removed, BGPLAYER_SONGS);
}

// Tells the server the song was played, as a player that reached its end.
static void scrobble(const Song* s) {
    if (!g_server[0]) return;
    char path[96];
    snprintf(path, sizeof(path), "/:/scrobble?key=%s&identifier=com.plexapp.plugins.library",
             s->key);
    Body b = {0};
    const int status = getBody(path, NULL, &b);
    free(b.data);
    if (status != 200) logLine("song %d of %d: marking it played: HTTP %d", s->index + 1, g_count,
                               status);
}

// ── The queue ────────────────────────────────────────────────────────────

// Up to the next '|' of *p into out, and past it.
static void field(const char** p, char* out, size_t size) {
    const size_t n = strcspn(*p, "|");
    snprintf(out, size, "%.*s", (int)(n < size - 1 ? n : size - 1), *p);
    *p += n + ((*p)[n] == '|');
}

static void parseTrack(const char* p, Entry* e) {
    char num[16], skip[8];
    memset(e, 0, sizeof(*e));
    field(&p, e->key, sizeof(e->key));
    field(&p, num, sizeof(num));
    e->durationMs = atoi(num);
    field(&p, e->local, sizeof(e->local));
    field(&p, skip, sizeof(skip));   // the rating
    field(&p, skip, sizeof(skip));   // the thumb
    field(&p, e->title, sizeof(e->title));
    field(&p, e->artist, sizeof(e->artist));
    field(&p, e->album, sizeof(e->album));
}

static const char* repeatName(int r) {
    return r == BGPLAYER_REPEAT_ONE ? "the one song" : r == BGPLAYER_REPEAT_ALL ? "all" : "off";
}

// The queue the app wrote. 0 when there is a song to play in it; with
// sameAsBefore set when it is the queue played before (no new one written).
static int loadQueue(int* sameAsBefore) {
    FILE* f = fopen(BGPLAYER_QUEUE, "r");
    if (!f) {
        logLine("no queue at %s: nothing to play", BGPLAYER_QUEUE);
        return -1;
    }
    if (!g_queue) g_queue = (Entry*)malloc(BGPLAYER_MAX_TRACKS * sizeof(Entry));
    if (!g_queue) {
        fclose(f);
        logLine("no memory for a queue");
        return -1;
    }
    static char line[2048];
    int count = 0, index = 0, repeat = BGPLAYER_REPEAT_OFF, whole = 0;
    unsigned position = 0;
    double written = 0;
    char file[128] = "", port[16] = BGPLAYER_PORT_PRIORITY;
    int qm = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "track=", 6) == 0) {
            if (count < BGPLAYER_MAX_TRACKS) parseTrack(line + 6, &g_queue[count++]);
        } else if (strncmp(line, "written=", 8) == 0) {
            written = strtod(line + 8, NULL);
        } else if (strncmp(line, "index=", 6) == 0) {
            index = atoi(line + 6);
        } else if (strncmp(line, "positionMs=", 11) == 0) {
            position = (unsigned)strtoul(line + 11, NULL, 10);
        } else if (strncmp(line, "repeat=", 7) == 0) {
            repeat = atoi(line + 7);
        } else if (strncmp(line, "file=", 5) == 0) {
            snprintf(file, sizeof(file), "%s", line + 5);
        } else if (strncmp(line, "port=", 5) == 0) {
            snprintf(port, sizeof(port), "%s", line + 5);
        } else if (strncmp(line, "qm=", 3) == 0) {
            qm = atoi(line + 3) == 1;
        } else if (strcmp(line, "end") == 0) {
            whole = 1;
        }
    }
    fclose(f);
    if (!whole || count == 0) {
        logLine("the queue at %s is %s: nothing to play", BGPLAYER_QUEUE,
                whole ? "empty" : "not whole");
        return -1;
    }
    *sameAsBefore = written == g_queueWritten;
    g_count = count;
    g_index = index >= 0 && index < count ? index : 0;
    g_startMs = position;
    g_repeat = repeat;
    g_queueWritten = written;
    snprintf(g_readyFile, sizeof(g_readyFile), "%s", file);
    snprintf(g_portHow, sizeof(g_portHow), "%s", port);
    g_qmEnabled = qm;
    char at[16];
    mmss(at, sizeof(at), position);
    const Entry* e = &g_queue[g_index];
    logLine("queue: %d song(s), from song %d (\"%s\" by %s) at %s, repeat %s%s, BGM port %s, "
            "quick menu %s; written %.1f s before this",
            count, g_index + 1, e->title, e->artist, at, repeatName(repeat),
            file[0] ? ", the app having made that song ready" : "", g_portHow,
            qm ? "on" : "off", systemSecs() - written);
    return 0;
}

// The song after index as the queue plays: the same one again for repeat
// one; -1 at the end.
static int nextIndex(int index) {
    if (g_repeat == BGPLAYER_REPEAT_ONE) return index;
    if (index + 1 < g_count) return index + 1;
    return g_repeat == BGPLAYER_REPEAT_ALL ? 0 : -1;
}

// ── The system player ────────────────────────────────────────────────────

// Hands s to the shell's music player: the BGM port and the service the first
// time, then stop, open and play, all at once (log 468b487f answered none of
// them). 0 when it took the file.
static int handOver(const Song* s) {
    char r[3][24];
    // The way the queue says, which VitaPlex BG's port test sets (bgplayer.h).
    // Not taken at all is tried once a time the shell starts.
    if (!g_portHeld && !(g_shellUp && strcmp(g_portHow, BGPLAYER_PORT_NONE) == 0)) {
        int rc = 0;
        if (strcmp(g_portHow, BGPLAYER_PORT_NONE) == 0) {
            logLine("the BGM port: not taken, as the queue says");
        } else {
            if (strcmp(g_portHow, BGPLAYER_PORT_PLAIN) == 0)
                rc = sceAppMgrAcquireBgmPort();
            else if (strcmp(g_portHow, BGPLAYER_PORT_MUSICPLAYER) == 0)
                rc = vitaplexAcquireBgmPortForMusicPlayer(0);
            else
                rc = sceAppMgrAcquireBgmPortWithPriority(SHELL_PORT_PRIORITY);
            logLine("acquire the BGM port %s%s: %s",
                    strcmp(g_portHow, BGPLAYER_PORT_PRIORITY) == 0 ? "at " : "", g_portHow,
                    result(r[0], 24, rc));
            g_portHeld = rc >= 0;
        }
    }
    if (!g_shellUp) {
        const int rc = sceMusicPlayerServiceInitialize(0);
        logLine("start the system player (music player, type 0): %s", result(r[0], 24, rc));
        if (rc < 0) return -1;
        g_shellUp = 1;
    }
    char path[128];
    snprintf(path, sizeof(path), "%s", s->path);
    const int rcStop = sceMusicPlayerServiceSendEvent(SCE_MUSIC_EVENTID_STOP, 0);
    const int rcOpen = sceMusicPlayerServiceOpen(path, NULL);
    const int rcPlay = sceMusicPlayerServiceSendEvent(SCE_MUSIC_EVENTID_PLAY, 0);
    g_handedOver = 1;
    logLine("song %d of %d handed to the system player (%s): stop %s, open %s, play %s",
            s->index + 1, g_count, path, result(r[0], 24, rcStop), result(r[1], 24, rcOpen),
            result(r[2], 24, rcPlay));
    return rcOpen < 0 ? -1 : 0;
}

// The length the shell gave the last song it played; 0 before the first.
static int g_lastShellLenMs;

// Whether the shell has the song just opened, by the length it gives: the
// server's, or after an earlier song any length but that one's. An open that
// does not take leaves the shell on the file before, whose length is known,
// while its own length for an MP3 with no length header is a guess.
static int shellHasNewSong(int shellLenMs, int serverLenMs) {
    const int d = shellLenMs - serverLenMs;
    if (serverLenMs > 0 && d > -3000 && d < 3000) return 1;
    const int e = shellLenMs - g_lastShellLenMs;
    return g_lastShellLenMs > 0 && shellLenMs > 0 && (e <= -3000 || e >= 3000);
}

static void shellClock(unsigned* timeMs, int* state) {
    SceMusicPlayerServicePlayStatusExtension st;
    memset(&st, 0, sizeof(st));
    sceMusicPlayerServiceGetPlayStatusExtension(&st);
    *timeMs = st.currentTime;
    *state = st.currentState;
}

// Whether `how` took the clock to ms within `wait` seconds: there or a little
// past it, and over 3 s from where playing alone would have taken it from
// `from`, where it was when asked, forward or back. 1 when it did.
static int seekLanded(const Song* s, unsigned ms, unsigned from, const char* how, int* state,
                      double wait) {
    const double t0 = now();
    unsigned time = from;
    char at[16];
    while (now() - t0 < wait && !g_paused && !g_stopped) {
        sceKernelDelayThread(250 * 1000);
        shellClock(&time, state);
        const double off = (double)time - from - (now() - t0) * 1000;
        if (time + 2000 >= ms && time <= ms + 6000 && (off > 3000 || off < -3000)) {
            mmss(at, sizeof(at), time);
            logLine("song %d of %d: %s took the clock to %s in %.1fs, state %d", s->index + 1,
                    g_count, how, at, now() - t0, *state);
            return 1;
        }
    }
    mmss(at, sizeof(at), time);
    logLine("song %d of %d: %s left the clock at %s, state %d", s->index + 1, g_count, how, at,
            *state);
    return 0;
}

// Waits for s to play, so that the open cannot undo a seek: the clock moving
// with this song's length (shellHasNewSong), for up to 10 s. The clock then,
// and 1 in *playing when it was seen to.
static unsigned untilPlaying(const Song* s, int* playing) {
    unsigned time = 0, prev = 0;
    int state = 0;
    *playing = 0;
    const double t0 = now();
    for (int havePrev = 0; !*playing && now() - t0 < 10.0 && !g_paused && !g_stopped;
         havePrev = 1) {
        sceKernelDelayThread(250 * 1000);
        prev = time;
        shellClock(&time, &state);
        if (state == 1 && havePrev && time > prev) {
            static SceMusicPlayerServiceTrackInfo info;
            memset(&info, 0, sizeof(info));
            *playing = sceMusicPlayerServiceGetTrackInfo(&info) >= 0 &&
                       shellHasNewSong(info.duration, s->durationMs);
        }
    }
    return time;
}

// Takes s to ms from the clock at time: the time and then the seek event, as
// ShellAudio.h has it, which worked on the console (log 0bbfcb94), and failing
// that the call that seeks by itself.
static void seekFrom(const Song* s, unsigned ms, unsigned time) {
    char want[16], r[2][24];
    mmss(want, sizeof(want), ms);
    int state = 1;
    const int rcTime = sceMusicPlayerServiceSetSeekTime(ms);
    const int rcEvent = sceMusicPlayerServiceSendEvent(SCE_MUSIC_EVENTID_SEEK, 0);
    logLine("song %d of %d: set seek time %s, seek event %s", s->index + 1, g_count,
            result(r[0], 24, rcTime), result(r[1], 24, rcEvent));
    int landed = seekLanded(s, ms, time, "the seek event", &state, 8.0);
    if (!landed && !g_paused && !g_stopped) {
        shellClock(&time, &state);
        logLine("song %d of %d: seek call %s", s->index + 1, g_count,
                result(r[0], 24, sceMusicPlayerServiceSeek(ms)));
        landed = seekLanded(s, ms, time, "the seek call", &state, 4.0);
    }
    if (g_paused || g_stopped) return;
    if (!landed)
        logLine("song %d of %d: neither way took it to %s; it goes on from where it is",
                s->index + 1, g_count, want);
    if (state != 1)
        logLine("song %d of %d: not playing after the seek; play %s", s->index + 1, g_count,
                result(r[0], 24, sceMusicPlayerServiceSendEvent(SCE_MUSIC_EVENTID_PLAY, 0)));
}

// Takes s on to ms, where the app or a pause left it.
static void seekTo(const Song* s, unsigned ms) {
    char want[16], at[16];
    mmss(want, sizeof(want), ms);
    int playing = 0;
    const double t0 = now();
    const unsigned time = untilPlaying(s, &playing);
    if (g_paused || g_stopped) return;
    mmss(at, sizeof(at), time);
    // The shell keeps its place in a file through a stop (log 7d5ae5e7), so
    // a song paused and handed over again may be there already.
    if (playing && time + 3000 >= ms && time <= ms + 3000) {
        logLine("song %d of %d: playing after %.1fs, at %s, where it is to go on from",
                s->index + 1, g_count, now() - t0, at);
        return;
    }
    logLine("song %d of %d: %s after %.1fs, at %s; seeking to %s", s->index + 1, g_count,
            playing ? "playing" : "not seen playing", now() - t0, at, want);
    seekFrom(s, ms, time);
}

// The shell can start a file it has had before where it left it (log
// 2aefa839), so a song that should start at 0:00 and starts more than 5 s in
// is taken back there.
static void fromTheStart(const Song* s) {
    int playing = 0;
    const unsigned time = untilPlaying(s, &playing);
    if (!playing || time < 5000 || g_paused || g_stopped) return;
    char at[16];
    mmss(at, sizeof(at), time);
    logLine("song %d of %d: the system player started it at %s, not 0:00; seeking back",
            s->index + 1, g_count, at);
    seekFrom(s, 0, time);
}

// The power button: the system stops the shell's music as it suspends apps
// (0x00400000, log 91252932). ElevenMPV-A sends play again from its power
// callback at that event, and so does this, from main.c's; the music then went
// on through the screen going off and on (log d942eac0).
void playerPowerEvent(int powerInfo) {
    if (!(powerInfo & 0x00400000) || !g_handedOver || !g_shellUp || g_paused || g_stopped) return;
    char r[24];
    logLine("apps suspending (power button?); play again, as ElevenMPV-A does: %s",
            result(r, sizeof(r), sceMusicPlayerServiceSendEvent(SCE_MUSIC_EVENTID_PLAY, 0)));
}

enum {
    WATCH_FAILED = 0,
    WATCH_ENDED = 1,
    WATCH_NEXT = 2,       // the quick menu's next
    WATCH_PREVIOUS = 3,   // its previous, in the first seconds of a song
    WATCH_PAUSED = -1,
    WATCH_STOPPED = -2
};

// Watches s play until it ends: its clock moving with a new song loaded
// (shellHasNewSong), then either going back after the last 10 s or the state
// leaving 1 (playing) there, against the server's length. The status is
// written every 2 s on the way.
static int watchSong(const Song* s) {
    const unsigned durMs = (unsigned)(s->durationMs > 0 ? s->durationMs : 0);
    char dur[16], at[16], r[24];
    mmss(dur, sizeof(dur), durMs);
    int lastState = -100, started = 0, otherLogged = 0, havePrev = 0, beat = 0;
    unsigned maxTime = 0, prevTime = 0;
    const double t0 = now();
    double lastLine = t0;
    // A song stopped part-way: since when, and for how long in all, which the
    // time it is given leaves out; and whether the quick menu was open as it
    // stopped (byMenu), which leaves it stopped.
    double stoppedSince = 0, stoppedFor = 0, lastPlay = 0;
    int byMenu = 0;
    for (;;) {
        sceKernelDelayThread(500 * 1000);
        if (g_stopped) return WATCH_STOPPED;
        if (g_paused) return WATCH_PAUSED;
        // As ElevenMPV-A does while it plays: the Vita does not go to sleep
        // by itself in the middle of a song.
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        SceMusicPlayerServicePlayStatusExtension st;
        memset(&st, 0, sizeof(st));
        const int rc = sceMusicPlayerServiceGetPlayStatusExtension(&st);
        const int state = st.currentState;
        const unsigned time = st.currentTime;
        const double t = now();
        mmss(at, sizeof(at), time);
        if (state != lastState) {
            logLine("song %d of %d: system player state %d at %s (%s)", s->index + 1, g_count,
                    state, at, result(r, sizeof(r), rc));
            lastState = state;
        }
        // A press on the quick menu's row (playerQuickMenuTick). Play/pause
        // stops the shell and plays it again, as ElevenMPV-A pauses a song the
        // shell plays: the shell keeps its place through a stop (log
        // 7d5ae5e7). Previous goes back to the start past a song's first
        // 3 s, and to the song before within them.
        const int press = g_menuPress;
        if (press != QM_NONE) {
            g_menuPress = QM_NONE;
            if (press == QM_NEXT) {
                logLine("song %d of %d: next, from the quick menu", s->index + 1, g_count);
                return WATCH_NEXT;
            }
            if (press == QM_PREVIOUS && !(started && time >= 3000)) {
                logLine("song %d of %d: previous, from the quick menu", s->index + 1, g_count);
                return WATCH_PREVIOUS;
            }
            if (press == QM_PREVIOUS) {
                logLine("song %d of %d: back to its start from %s, from the quick menu",
                        s->index + 1, g_count, at);
                g_menuPaused = 0;
                seekFrom(s, 0, time);
                maxTime = 0;
                havePrev = 0;
                continue;
            }
            if (g_menuPaused || state != 1) {
                g_menuPaused = 0;
                logLine("song %d of %d: play at %s, from the quick menu: %s", s->index + 1, g_count,
                        at, result(r, sizeof(r), sceMusicPlayerServiceSendEvent(SCE_MUSIC_EVENTID_PLAY, 0)));
            } else {
                g_menuPaused = 1;
                logLine("song %d of %d: paused at %s, from the quick menu: %s", s->index + 1,
                        g_count, at,
                        result(r, sizeof(r), sceMusicPlayerServiceSendEvent(SCE_MUSIC_EVENTID_STOP, 0)));
            }
            continue;
        }
        if (!started && state == 1 && havePrev && time > prevTime) {
            static SceMusicPlayerServiceTrackInfo info;
            memset(&info, 0, sizeof(info));
            const int rcInfo = sceMusicPlayerServiceGetTrackInfo(&info);
            char len[16];
            mmss(len, sizeof(len), (unsigned)(info.duration > 0 ? info.duration : 0));
            if (rcInfo >= 0 && shellHasNewSong(info.duration, s->durationMs)) {
                started = 1;
                maxTime = time;
                lockStatus();
                g_now.started = 1;
                unlockStatus();
                logLine("song %d of %d plays: the system player has it as %s long, the server "
                        "%s, and is at %s",
                        s->index + 1, g_count, len, dur, at);
                g_lastShellLenMs = info.duration;
            } else if (!otherLogged) {
                logLine("song %d of %d: the system player is playing something %s long, not "
                        "this song of %s (%s)",
                        s->index + 1, g_count, len, dur, result(r, sizeof(r), rcInfo));
                otherLogged = 1;
            }
        }
        // A jump in the clock, which the quick menu's buttons may make.
        if (started && havePrev && (time + 3000 < prevTime || time > prevTime + 5000)) {
            char from[16];
            mmss(from, sizeof(from), prevTime);
            logLine("song %d of %d: the clock went from %s to %s, state %d%s", s->index + 1,
                    g_count, from, at, state,
                    quickMenuSince() < 60.0 ? ", with the quick menu open" : "");
        }
        prevTime = time;
        havePrev = 1;
        if (started && ++beat % 4 == 0) writeStatus(BGPLAYER_STATE_PLAYING, time, 1);
        if (t - lastLine >= 30.0) {
            logLine("song %d of %d at %s of %s, state %d", s->index + 1, g_count, at, dur, state);
            lastLine = t;
        }
        // Its length: the server's, else the shell's.
        const unsigned lenMs =
            durMs > 0 ? durMs : (unsigned)(g_lastShellLenMs > 0 ? g_lastShellLenMs : 0);
        if (started) {
            if (time > maxTime) maxTime = time;
            if (maxTime + 10000 >= lenMs && (time + 5000 < maxTime || state != 1) &&
                !g_menuPaused) {
                mmss(at, sizeof(at), maxTime);
                logLine("song %d of %d ended (it got to %s of %s)", s->index + 1, g_count, at, dur);
                return WATCH_ENDED;
            }
            // Stopped before its end, as the power button stopped it in log
            // 91252932, where it never went on: play again every 5 s while it
            // stays so, and move on after two minutes of it. Not when it
            // stopped with the quick menu open, where it may be the Music
            // row's pause: then it stays stopped until the shell plays again.
            if (state != 1) {
                if (stoppedSince == 0) {
                    stoppedSince = t;
                    const double menu = quickMenuSince();
                    byMenu = g_menuPaused || menu < 60.0;
                    if (byMenu && !g_menuPaused)
                        logLine("song %d of %d stopped at %s (state %d), %.0fs after PS was held: "
                                "taken as the quick menu's pause; not played again",
                                s->index + 1, g_count, at, state, menu);
                }
                if (!byMenu && t - stoppedSince > 120.0) {
                    logLine("song %d of %d stayed stopped at %s for two minutes; moving on",
                            s->index + 1, g_count, at);
                    return WATCH_FAILED;
                }
                if (!byMenu && t - lastPlay >= 5.0) {
                    logLine("song %d of %d stopped at %s, before its end (state %d); play again: "
                            "%s",
                            s->index + 1, g_count, at, state,
                            result(r, sizeof(r),
                                   sceMusicPlayerServiceSendEvent(SCE_MUSIC_EVENTID_PLAY, 0)));
                    lastPlay = t;
                }
            } else if (stoppedSince > 0) {
                logLine("song %d of %d plays again at %s, after %.1fs stopped%s", s->index + 1,
                        g_count, at, t - stoppedSince,
                        byMenu ? ", from a stop with the quick menu open" : "");
                stoppedFor += t - stoppedSince;
                stoppedSince = 0;
                byMenu = 0;
                g_menuPaused = 0;
            }
        } else if (t - t0 > 15.0) {
            logLine("song %d of %d: the system player never started it", s->index + 1, g_count);
            return WATCH_FAILED;
        }
        const double stopped = stoppedFor + (stoppedSince > 0 ? t - stoppedSince : 0);
        if (lenMs > 0 && t - t0 - stopped > lenMs / 1000.0 + 60.0) {
            mmss(at, sizeof(at), maxTime);
            logLine("song %d of %d did not end in time (it got to %s of %s)", s->index + 1,
                    g_count, at, dur);
            return WATCH_FAILED;
        }
    }
}

static void stopShell(const char* why) {
    char a[24], b[24];
    if (g_shellUp) {
        g_shellUp = 0;
        const int rcStop = sceMusicPlayerServiceSendEvent(SCE_MUSIC_EVENTID_STOP, 0);
        const int rcTerm = sceMusicPlayerServiceTerminate();
        logLine("system player stop %s, terminate %s (%s)", result(a, 24, rcStop),
                result(b, 24, rcTerm), why);
    }
    if (g_portHeld) {
        g_portHeld = 0;
        logLine("release the BGM port: %s", result(a, 24, sceAppMgrReleaseBgmPort()));
    }
    g_handedOver = 0;
    // The row goes back to the system with the music: left on, the plugin
    // would put presses into a pipe nobody reads, and SceShell waits once
    // that is full.
    if (__atomic_exchange_n(&g_qmActive, 0, __ATOMIC_SEQ_CST))
        logLine("quick menu: row given back (%s): %s", why, result(a, 24, qmDeactivate()));
    g_menuPress = QM_NONE;
    g_menuPaused = 0;
}

// The row, from main.c's watcher: the plugin opened the first time a song is
// with the shell (and again at most every 30 s while that fails), the row
// taken while the queue plays and the song sent as it changes, and presses
// passed to watchSong.
void playerQuickMenuTick(void) {
    static double lastTry, lastBeat;
    static int sentIndex = -2, sentPlaying = -1;
    char r[24];
    if (!(g_qmEnabled && g_handedOver && g_shellUp && !g_paused && !g_stopped)) {
        if (__atomic_exchange_n(&g_qmActive, 0, __ATOMIC_SEQ_CST))
            logLine("quick menu: row given back, nothing playing: %s",
                    result(r, sizeof(r), qmDeactivate()));
        sentIndex = -2;
        return;
    }
    if (!qmIsOpen()) {
        if (lastTry != 0 && now() - lastTry < 30.0) return;
        lastTry = now();
        char what[320];
        const int rc = qmOpen(what, sizeof(what));
        logLine("quick menu: %s (%s)", what, result(r, sizeof(r), rc));
        if (rc < 0) return;
    }
    if (!g_qmActive) {
        // A press left from whoever had the row before is not one for this.
        for (int stale; (stale = qmPoll()) != QM_NONE;)
            logLine("quick menu: an earlier press (%d) dropped", stale);
        const int rc = qmActivate(QM_OWNER_PLAYER);
        logLine("quick menu: row taken: %s", result(r, sizeof(r), rc));
        if (rc < 0) return;
        g_qmActive = 1;
        sentIndex = -2;
        sentPlaying = -1;
        lastBeat = now();
    }
    // VitaPlex's plugin gives the row back by itself once these stop, as when
    // the system ends this process.
    if (now() - lastBeat >= 1.0) {
        lastBeat = now();
        qmHeartbeat(QM_OWNER_PLAYER);
    }
    lockStatus();
    const int index = g_now.index;
    unlockStatus();
    if (index != sentIndex && index >= 0 && index < g_count) {
        const Entry* e = &g_queue[index];
        const int rc = qmSetSong(e->title, e->artist, e->album);
        logLine("quick menu: \"%s\" by %s: %s", e->title, e->artist, result(r, sizeof(r), rc));
        if (rc >= 0) sentIndex = index;
    }
    const int playing = !g_menuPaused;
    if (playing != sentPlaying && qmSetPlaying(playing) >= 0) sentPlaying = playing;
    for (int press; (press = qmPoll()) != QM_NONE;) {
        logLine("quick menu: %s pressed",
                press == QM_NEXT ? "next" : press == QM_PREVIOUS ? "previous" : "play/pause");
        g_menuPress = press;
    }
}

// ── Requests from the app ────────────────────────────────────────────────

// From the watcher, as the app takes the music back. The place is read before
// the stop, and the flag set before it, so that the player's thread does not
// take the stop for one to play through (watchSong). The status says where,
// before the request counts as taken (main.c removes it after this returns).
void playerPause(const char* why) {
    (void)why;
    if (g_stopped) return;
    if (!g_paused) {
        lockStatus();
        unsigned at = g_now.startMs;
        const int started = g_now.started, index = g_now.index;
        unlockStatus();
        // Before the song is seen playing, the clock may still be the file
        // before's; where it was to start is the place then.
        int state = 0;
        if (started && g_shellUp) shellClock(&at, &state);
        g_pausedIndex = index;
        g_pausedAtMs = at;
        g_pauseWasPlaying = g_active;
        g_paused = 1;
    }
    g_playAsked = 0;
    stopShell("paused by its app");
    writeStatus(BGPLAYER_STATE_PAUSED, g_pausedAtMs, g_pauseWasPlaying);
    char a[16];
    mmss(a, sizeof(a), g_pausedAtMs);
    logLine("paused at %s of song %d%s; it plays again when its app asks and has gone", a,
            g_pausedIndex + 1, g_pauseWasPlaying ? "" : ", where no queue was playing");
}

// The app writes the queue to play before it asks. A player that was not
// paused is stopped where it is first, as a pause would, so that nothing
// plays while its app goes.
void playerPlay(const char* why) {
    if (g_stopped) return;
    if (!g_paused) playerPause(why);
    g_playAsked = 1;
}

void playerStop(const char* why) {
    lockStatus();
    unsigned at = g_now.startMs;
    const int started = g_now.started;
    unlockStatus();
    int state = 0;
    if (started && g_shellUp && !g_paused) shellClock(&at, &state);
    if (g_paused) at = g_pausedAtMs;
    g_stopped = 1;
    stopShell(why);
    writeStatus(BGPLAYER_STATE_STOPPED, at, 0);
}

// ── The run ──────────────────────────────────────────────────────────────

// Nothing starts while its app is still there: one with extended memory was
// listed for 10.3 s after it had ended itself while the background test's
// player played, and the console crashed as it went (log cf918ac6). Up to 30 s.
static void waitForAppGone(void) {
    if (!appRunning()) return;
    logLine("its app %s is still there; waiting for it to go first", BGPLAYER_APP_TITLEID);
    const double t0 = now();
    while (appRunning() && now() - t0 < 30.0 && !g_stopped) sceKernelDelayThread(250 * 1000);
    if (!appRunning())
        logLine("its app has gone, after %.2f s", now() - t0);
    else if (!g_stopped)
        logLine("its app is still there after 30 s; going on all the same");
}

// With nothing playing (paused, or the queue over): waits while its app is
// open for it to ask for the music again, then for it to go. 1 when it asked;
// 0 when it went without asking, or the player is to stop.
static int waitForPlay(void) {
    int told = 0;
    for (;;) {
        if (g_stopped) return 0;
        if (g_playAsked) {
            g_playAsked = 0;
            logLine("its app asks for the music again");
            waitForAppGone();
            return !g_stopped;
        }
        if (!appRunning()) {
            // The request comes before the app goes; one more look, for one
            // written as it went.
            sceKernelDelayThread(500 * 1000);
            if (g_playAsked) continue;
            return 0;
        }
        if (!told) {
            logLine("waiting for its app to ask for the music again, or to go");
            told = 1;
        }
        sceKernelDelayThread(250 * 1000);
    }
}

static int prefetchMain(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    g_nextState = prepareSong(g_nextIndex, &g_next, NULL) == 0 ? 2 : -1;
    return 0;
}

static void startPrefetch(int index) {
    if (index < 0 || g_nextThread >= 0) return;
    g_nextIndex = index;
    g_nextState = 1;
    g_cancel = 0;
    g_nextThread =
        sceKernelCreateThread("BgPlayerNext", prefetchMain, 0x10000100 + 20, 256 * 1024, 0, 0, NULL);
    if (g_nextThread < 0 || sceKernelStartThread(g_nextThread, 0, NULL) < 0) {
        g_nextState = -1;
        if (g_nextThread >= 0) sceKernelDeleteThread(g_nextThread);
        g_nextThread = -1;
    }
}

// The next song's download, waited for, and stopped first when it is not
// wanted (index -1). 1 with the song at index in *out when it is ready.
static int takePrefetch(int index, Song* out) {
    if (g_nextThread < 0) return 0;
    if (index < 0 || index != g_nextIndex) g_cancel = 1;
    sceKernelWaitThreadEnd(g_nextThread, NULL, NULL);
    sceKernelDeleteThread(g_nextThread);
    g_nextThread = -1;
    g_cancel = 0;
    const int ready = g_nextState == 2 && index >= 0 && g_nextIndex == index;
    if (ready)
        *out = g_next;
    else if (g_nextState == 2 && (!out || strcmp(g_next.path, out->path) != 0))
        removeSong(&g_next);
    g_nextState = 0;
    g_nextIndex = -1;
    return ready;
}

enum { QUEUE_OVER, QUEUE_STOPPED, QUEUE_AGAIN, QUEUE_LEFT, QUEUE_FAILED };

// The song at g_index is the one the player is on: a pause now is at its
// start (startMs) until it is seen playing.
static void nowOn(unsigned startMs) {
    const Entry* e = &g_queue[g_index];
    lockStatus();
    g_now.index = g_index;
    snprintf(g_now.key, sizeof(g_now.key), "%s", e->key);
    g_now.durationMs = e->durationMs;
    g_now.startMs = startMs;
    g_now.started = 0;
    unlockStatus();
}

// Plays the queue from g_index at g_startMs to its end, or until the app asks
// for something else.
static int playQueue(void) {
    Song cur, prev;
    memset(&cur, 0, sizeof(cur));
    memset(&prev, 0, sizeof(prev));
    // The song that played to its end, to tell the server once the next one
    // is with the shell, rather than in the gap between them.
    int played = 0;
    int failedInARow = 0, tries = 0;
    const char* ready = g_readyFile;
    for (;;) {
        if (g_stopped) return QUEUE_STOPPED;
        if (g_paused) return QUEUE_LEFT;
        // Started at its last seconds, the song is as good as over.
        const Entry* e = &g_queue[g_index];
        if (g_startMs > 0 && e->durationMs > 0 && g_startMs + 5000 >= (unsigned)e->durationMs) {
            g_startMs = 0;
            const int next = nextIndex(g_index);
            logLine("song %d of %d was at its last seconds; %s", g_index + 1, g_count,
                    next < 0 ? "the queue is over" : "going on to the next");
            if (next < 0) return QUEUE_OVER;
            if (next != g_index) ready = NULL;
            g_index = next;
            continue;
        }
        nowOn(g_startMs);

        Song got;
        memset(&got, 0, sizeof(got));
        const int rc = takePrefetch(g_index, &cur) ? 0 : prepareSong(g_index, &got, ready);
        if (rc == 0 && got.path[0]) cur = got;
        ready = NULL;
        if (g_stopped) return QUEUE_STOPPED;
        // Kept on the card if it came whole: the same song, played on after
        // the pause, needs no download.
        if (g_paused) return QUEUE_LEFT;
        if (rc == -1 && ++tries < 3) {
            // The network gone for a moment: the console woke from sleep, say.
            logLine("song %d of %d: trying again in 10 s", g_index + 1, g_count);
            for (int i = 0; i < 40 && !g_paused && !g_stopped; i++) sceKernelDelayThread(250 * 1000);
            continue;
        }
        tries = 0;
        if (rc != 0) {
            if (++failedInARow >= 3) {
                logLine("three songs in a row would not play; stopping");
                return QUEUE_FAILED;
            }
            const int next = nextIndex(g_index);
            if (next < 0 || next == g_index) return QUEUE_OVER;
            g_index = next;
            g_startMs = 0;
            continue;
        }

        if (handOver(&cur) != 0 && !g_shellUp) {
            logLine("the system player would not start; stopping");
            return QUEUE_FAILED;
        }
        if (played) scrobble(&prev);
        played = 0;
        // The song before is with the shell no more.
        if (prev.own && strcmp(prev.path, cur.path) != 0) removeSong(&prev);
        prev = cur;
        if (g_startMs >= 5000)
            seekTo(&cur, g_startMs);
        else
            fromTheStart(&cur);
        g_startMs = 0;
        writeStatus(BGPLAYER_STATE_PLAYING, 0, 1);
        const int next = nextIndex(g_index);
        if (next >= 0 && next != g_index && !g_paused && !g_stopped) startPrefetch(next);

        const int watched = watchSong(&cur);
        if (watched == WATCH_STOPPED || watched == WATCH_PAUSED) {
            takePrefetch(-1, NULL);
            return watched == WATCH_STOPPED ? QUEUE_STOPPED : QUEUE_LEFT;
        }
        if (watched == WATCH_NEXT || watched == WATCH_PREVIOUS) {
            // The quick menu's next or previous: the song stops at once, not
            // once the other is ready, and is not marked played. Next goes on
            // past repeat one; previous stays on the first song unless the
            // queue repeats.
            char r[24];
            logLine("system player stop: %s",
                    result(r, sizeof(r), sceMusicPlayerServiceSendEvent(SCE_MUSIC_EVENTID_STOP, 0)));
            failedInARow = 0;
            const int to = watched == WATCH_NEXT
                ? (g_index + 1 < g_count ? g_index + 1 : g_repeat == BGPLAYER_REPEAT_ALL ? 0 : -1)
                : (g_index > 0 ? g_index - 1 : g_repeat == BGPLAYER_REPEAT_ALL ? g_count - 1 : 0);
            if (to < 0) {
                takePrefetch(-1, NULL);
                logLine("next on the last song: the queue is over");
                return QUEUE_OVER;
            }
            g_index = to;
            g_startMs = 0;
            continue;
        }
        if (watched == WATCH_ENDED) {
            failedInARow = 0;
            played = 1;
        } else if (++failedInARow >= 3) {
            takePrefetch(-1, NULL);
            logLine("three songs in a row would not play; stopping");
            return QUEUE_FAILED;
        }
        if (next < 0) {
            if (played) scrobble(&cur);
            return QUEUE_OVER;
        }
        g_index = next;
    }
}

static int playerMain(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    waitForAppGone();
    readSettings();
    for (int i = 0; i < 300 && !g_netReady; i++) sceKernelDelayThread(100 * 1000);
    if (!g_netReady) logLine("the network did not start; only songs on the card can play");
    curl_global_init(CURL_GLOBAL_DEFAULT);
    findCaBundle();
    sceIoMkdir(BGPLAYER_SONGS, 0777);

    int same = 0, cleaned = 0, how = QUEUE_STOPPED;
    while (!g_stopped && loadQueue(&same) == 0) {
        if (!cleaned) {
            cleanSongs(g_readyFile);
            cleaned = 1;
        }
        if (same && g_paused && g_pauseWasPlaying && g_pausedIndex >= 0 &&
            g_pausedIndex < g_count) {
            // Asked for the music again with no new queue: where it was
            // paused.
            g_index = g_pausedIndex;
            g_startMs = g_pausedAtMs;
            logLine("no new queue: song %d goes on where it was paused", g_index + 1);
        }
        g_paused = 0;
        g_active = 1;
        how = playQueue();
        g_active = how == QUEUE_LEFT;
        if (how == QUEUE_LEFT) {
            // Paused in the middle of a hand-over, the shell may have been
            // started again behind the pause.
            if (g_shellUp || g_portHeld) stopShell("paused during a hand-over");
            // Its app has the music: wait for it to ask for it again, with
            // the queue it writes, or to go without asking.
            if (waitForPlay()) {
                how = QUEUE_AGAIN;
                continue;
            }
            break;
        }
        if (how == QUEUE_OVER || how == QUEUE_FAILED) {
            stopShell(how == QUEUE_OVER ? "the queue is over" : "songs would not play");
            writeStatus(how == QUEUE_OVER ? BGPLAYER_STATE_OVER : BGPLAYER_STATE_STOPPED, 0, 0);
            // Never ends while its app is open: it may yet hand music over.
            if (appRunning() && waitForPlay()) {
                how = QUEUE_AGAIN;
                continue;
            }
        }
        break;
    }
    g_active = 0;
    if (how == QUEUE_LEFT && !g_stopped) {
        logLine("its app went without asking for the music again");
        writeStatus(BGPLAYER_STATE_STOPPED, g_pausedAtMs, 0);
    }
    stopShell("the player is done");
    cleanSongs(NULL);
    // Never ends while its app is open (bgplayer.h).
    while (appRunning() && !g_stopped) sceKernelDelayThread(250 * 1000);
    quitNow();
    return 0;
}

void playerStart(void) {
    g_statusLock = sceKernelCreateMutex("BgPlayerStatus", 0, 0, NULL);
    g_now.index = -1;
    g_pausedIndex = -1;
    // Started to play: a pause before the first song is out counts as one
    // while music played, for the app to carry it on.
    g_active = 1;
    const SceUID t =
        sceKernelCreateThread("BgPlayer", playerMain, 0x10000100 + 20, 256 * 1024, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}
