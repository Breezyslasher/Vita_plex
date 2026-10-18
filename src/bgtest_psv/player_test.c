/**
 * VPLXBGT10: music during a game the second way, tried from the background.
 *
 * An app's own BGM output goes quiet while an app with extended memory is in
 * front, and the shell's music service does not, but the shell plays files,
 * not links (DESIGN_NOTES.md). So a player would download each song to the
 * memory card and hand it to the shell, while a game is in front. This tries
 * both halves once the app has been sent away with PS:
 *
 *   1. the first song that fits, downloaded as the server has it when the
 *      shell decodes that (MP3, AAC), and handed to the shell at once;
 *   2. the next one, downloaded as the server's MP3 of it while the first
 *      plays, and handed over when the first ends.
 *
 * Plex makes the MP3 as it sends it and may hold back once it is well ahead,
 * which the second download's speed shows; an original file has no such
 * limit.
 *
 * Requests go over https as VitaPlex makes them, with curl, mbedtls and
 * Mozilla's CA bundle (packaged beside the eboot). The sign-in token comes
 * from VitaPlex's settings.json and goes to the server in the requests, and
 * no URL is logged past its path.
 */

#include "bgtest.h"

#include "ShellAudio.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <curl/curl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int sceAppMgrAcquireBgmPortWithPriority(int priority);
int sceAppMgrReleaseBgmPort(void);

// What ElevenMPV-A asks for while SceShell decodes, and what VitaPlex's
// system player tests hold.
#define SHELL_PORT_PRIORITY 0x80

#define MB (1024 * 1024)

typedef struct Track {
    char ratingKey[16];
    char title[96];
    char artist[96];
    char partKey[192];
    char ext[8];        // of the server's file: "mp3", "m4a", ...
    int durationMs;
    long long size;
    int transcode;      // 1: fetch the server's MP3 of it, not the file
    char path[64];      // where it is saved
} Track;

static char g_server[256];
static char g_token[160];
static volatile int g_shellUp;
static volatile int g_portHeld;
static Track g_tracks[2];
static volatile int g_secondReady;   // 1 downloaded, -1 failed
static volatile int g_handedOver;    // a song has been given to the shell

static double now(void) { return sceKernelGetSystemTimeWide() / 1e6; }

static void mmss(char* buf, size_t size, unsigned ms) {
    snprintf(buf, size, "%u:%02u", ms / 60000, ms / 1000 % 60);
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

static int readSettings(void) {
    FILE* f = fopen("ux0:data/VitaPlex/settings.json", "r");
    if (!f) {
        logLine("player: no VitaPlex settings file");
        return -1;
    }
    static char buf[32 * 1024];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (jsonString(buf, "serverUrl", g_server, sizeof(g_server)) <= 0) {
        logLine("player: no server in VitaPlex's settings");
        return -1;
    }
    if (jsonString(buf, "authToken", g_token, sizeof(g_token)) <= 0) {
        logLine("player: VitaPlex is not signed in");
        return -1;
    }
    size_t len = strlen(g_server);
    while (len > 0 && g_server[len - 1] == '/') g_server[--len] = '\0';
    logLine("player: server %s, signed in", g_server);
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
// client's names in headers, https checked against the bundled CA list.
static CURL* request(const char* path, struct curl_slist** headers) {
    char url[1024], token[200];
    snprintf(url, sizeof(url), "%s%s", g_server, path);
    snprintf(token, sizeof(token), "X-Plex-Token: %s", g_token);
    struct curl_slist* h = curl_slist_append(NULL, token);
    h = curl_slist_append(h, "X-Plex-Client-Identifier: vitaplex-bgtest10");
    h = curl_slist_append(h, "X-Plex-Product: VitaPlex");
    h = curl_slist_append(h, "X-Plex-Platform: PlayStation Vita");
    h = curl_slist_append(h, "X-Plex-Device: PS Vita");
    h = curl_slist_append(h, "X-Plex-Device-Name: PS Vita");
    *headers = h;

    CURL* c = curl_easy_init();
    if (!c) return NULL;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "VitaPlex BG Test 10");
    curl_easy_setopt(c, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_CAINFO, "app0:cacert.pem");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    return c;
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
        logLine("player: GET %s failed: %s", logPath(path, p, sizeof(p)), curl_easy_strerror(rc));
        return -1;
    }
    return (int)status;
}

// ── Picking two songs ────────────────────────────────────────────────────

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

// Two songs of 1 to 4 minutes from the first music library, the first one the
// shell decodes as it is (else any), the second to fetch as the server's MP3.
static int pickTracks(void) {
    Body b = {0};
    int status = getBody("/library/sections", NULL, &b);
    char key[16] = "";
    for (const char* d = b.data; d && (d = strstr(d, "<Directory ")) != NULL; d++) {
        char type[16];
        if (xmlAttr(d, "type", type, sizeof(type)) == 0 && strcmp(type, "artist") == 0) {
            xmlAttr(d, "key", key, sizeof(key));
            break;
        }
    }
    free(b.data);
    if (!key[0]) {
        logLine("player: no music library (sections: HTTP %d)", status);
        return 0;
    }

    char path[160];
    snprintf(path, sizeof(path),
             "/library/sections/%s/all?type=10&X-Plex-Container-Start=0&X-Plex-Container-Size=300",
             key);
    memset(&b, 0, sizeof(b));
    status = getBody(path, NULL, &b);
    logLine("player: music library %s: HTTP %d, %u bytes of tracks", key, status,
            (unsigned)b.len);

    // The first song the shell decodes as it is, and the first two of all.
    Track t, plays, any[2];
    int havePlays = 0, haveAny = 0, seen = 0;
    for (const char* p = b.data; p && (p = strstr(p, "<Track ")) != NULL; p++, seen++) {
        memset(&t, 0, sizeof(t));
        char num[24];
        xmlAttr(p, "ratingKey", t.ratingKey, sizeof(t.ratingKey));
        xmlAttr(p, "title", t.title, sizeof(t.title));
        xmlAttr(p, "grandparentTitle", t.artist, sizeof(t.artist));
        if (xmlAttr(p, "duration", num, sizeof(num)) == 0) t.durationMs = atoi(num);
        const char* next = strstr(p + 1, "<Track ");
        const char* part = strstr(p, "<Part ");
        if (!part || (next && part > next)) continue;
        xmlAttr(part, "key", t.partKey, sizeof(t.partKey));
        if (xmlAttr(part, "size", num, sizeof(num)) == 0) t.size = atoll(num);
        const char* dot = strrchr(t.partKey, '.');
        snprintf(t.ext, sizeof(t.ext), "%s", dot ? dot + 1 : "");
        for (char* e = t.ext; *e; e++)
            if (*e >= 'A' && *e <= 'Z') *e += 'a' - 'A';
        if (strcmp(t.ext, "mp4") == 0) strcpy(t.ext, "m4a");
        if (!t.ratingKey[0] || !t.partKey[0]) continue;
        if (t.durationMs < 60000 || t.durationMs > 240000) continue;
        if (!havePlays && shellDecodes(t.ext)) {
            plays = t;
            havePlays = 1;
        }
        if (haveAny < 2) any[haveAny++] = t;
        if (havePlays && haveAny == 2) break;
    }
    free(b.data);

    // Song 1 as it is when one plays so, song 2 always as the server's MP3.
    int n = 0;
    if (havePlays) {
        g_tracks[n++] = plays;
        for (int i = 0; i < haveAny && n < 2; i++)
            if (strcmp(any[i].ratingKey, plays.ratingKey) != 0) {
                g_tracks[n] = any[i];
                g_tracks[n++].transcode = 1;
            }
    } else {
        for (int i = 0; i < haveAny; i++) {
            g_tracks[n] = any[i];
            g_tracks[n++].transcode = 1;
        }
    }
    for (int i = 0; i < n; i++) {
        Track* s = &g_tracks[i];
        snprintf(s->path, sizeof(s->path), "ux0:data/VitaPlex/bgtest10-%d.%s", i + 1,
                 s->transcode ? "mp3" : s->ext);
        char d[16];
        mmss(d, sizeof(d), (unsigned)s->durationMs);
        logLine("player: song %d: \"%s\" by %s, %s, %s file of %.1f MB; fetched %s", i + 1,
                s->title, s->artist, d, s->ext, s->size / (double)MB,
                s->transcode ? "as the server's MP3" : "as it is");
    }
    if (n == 0) logLine("player: no song of 1 to 4 minutes among %d tracks", seen);
    return n;
}

// ── Downloading ──────────────────────────────────────────────────────────

typedef struct Progress {
    int song;
    double lastLog;
} Progress;

static int onProgress(void* user, curl_off_t total, curl_off_t got, curl_off_t ut, curl_off_t un) {
    (void)total;
    (void)ut;
    (void)un;
    Progress* p = (Progress*)user;
    const double t = now();
    if (t - p->lastLog >= 10.0) {
        p->lastLog = t;
        logLine("player: song %d: %.1f MB so far%s", p->song, got / (double)MB,
                g_away ? ", away" : "");
    }
    return 0;
}

// The server's MP3 of a song, as VitaPlex asks for it: a decision first, then
// start.mp3 with the same session (plex_client.cpp, resolveTranscodeUrl).
static void transcodePath(const Track* t, int song, char* out, size_t size, int start) {
    char metadata[48];
    snprintf(metadata, sizeof(metadata), "/library/metadata/%s", t->ratingKey);
    char* path = curl_easy_escape(NULL, metadata, 0);
    char* extra = curl_easy_escape(NULL,
                                   "add-transcode-target(type=musicProfile&context=streaming"
                                   "&protocol=http&container=mp3&audioCodec=mp3)",
                                   0);
    char* token = curl_easy_escape(NULL, g_token, 0);
    snprintf(out, size,
             "/music/:/transcode/universal/%s?path=%s&mediaIndex=0&partIndex=0&directPlay=0"
             "&directStream=1&directStreamAudio=1&hasMDE=1&location=lan&audioBoost=100"
             "&audioChannelCount=2&protocol=http&musicBitrate=320&session=bgtest10-%lu-%d"
             "&X-Plex-Token=%s&X-Plex-Client-Profile-Name=Generic&X-Plex-Client-Profile-Extra=%s",
             start ? "start.mp3" : "decision", path ? path : "", (unsigned long)time(NULL),
             song, token ? token : "", extra ? extra : "");
    curl_free(path);
    curl_free(extra);
    curl_free(token);
}

static int download(int song) {
    Track* t = &g_tracks[song - 1];
    char path[1536];
    if (t->transcode) {
        char decision[1536];
        transcodePath(t, song, decision, sizeof(decision), 0);
        Body b = {0};
        const int status = getBody(decision, "application/json", &b);
        char text[96] = "";
        if (b.data) jsonString(b.data, "generalDecisionText", text, sizeof(text));
        logLine("player: song %d: transcode decision HTTP %d: %s", song, status, text);
        free(b.data);
        transcodePath(t, song, path, sizeof(path), 1);
    } else {
        snprintf(path, sizeof(path), "%s?download=1", t->partKey);
    }

    FILE* f = fopen(t->path, "wb");
    if (!f) {
        logLine("player: song %d: cannot write %s", song, t->path);
        return -1;
    }
    static char fbuf[2][64 * 1024];
    setvbuf(f, fbuf[song - 1], _IOFBF, sizeof(fbuf[0]));

    struct curl_slist* h = NULL;
    CURL* c = request(path, &h);
    if (!c) {
        fclose(f);
        return -1;
    }
    Progress prog = {song, now()};
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);   // curl fwrites to it by default
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &prog);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1000L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 64L * 1024);

    char lp[96];
    const int awayAtStart = g_away;
    logLine("player: song %d: downloading %s%s", song, logPath(path, lp, sizeof(lp)),
            awayAtStart ? ", away" : "");
    const double t0 = now();
    const CURLcode rc = curl_easy_perform(c);
    const double took = now() - t0;
    long status = 0;
    curl_off_t bytes = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(c, CURLINFO_SIZE_DOWNLOAD_T, &bytes);
    curl_easy_cleanup(c);
    curl_slist_free_all(h);
    fclose(f);

    logLine("player: song %d: %s, HTTP %ld, %.2f MB in %.1fs (%.0f KB/s), away at start %s, "
            "at end %s",
            song, rc == CURLE_OK ? "downloaded" : curl_easy_strerror(rc), status,
            bytes / (double)MB, took, took > 0 ? bytes / 1024.0 / took : 0.0,
            awayAtStart ? "yes" : "no", g_away ? "yes" : "no");
    return rc == CURLE_OK && status == 200 && bytes > 0 ? 0 : -1;
}

// ── The system player ────────────────────────────────────────────────────

// The first hand-over goes one step every 5 s. Done all at once from the
// background, it was answered within 250 ms by the system asking the app to
// quit (log 7a912d67), and the watcher reads events four times a second, so
// only spacing the steps out says which one the system answers.
static void step(int first) {
    if (first) sceKernelDelayThread(5 * 1000 * 1000);
}

static void handOver(int song) {
    char r[3][24];
    const int first = !g_portHeld && !g_shellUp;
    if (!g_portHeld) {
        logLine("player: step 1 of 4%s: acquire the BGM port at 0x%X", g_away ? ", away" : "",
                SHELL_PORT_PRIORITY);
        const int rc = sceAppMgrAcquireBgmPortWithPriority(SHELL_PORT_PRIORITY);
        logLine("player: acquire BGM port: %s", result(r[0], 24, rc));
        g_portHeld = rc >= 0;
        step(first);
    }
    if (!g_shellUp) {
        logLine("player: step 2 of 4%s: start the system player (music player, type 0)",
                g_away ? ", away" : "");
        const int rc = sceMusicPlayerServiceInitialize(0);
        logLine("player: system player initialize %s", result(r[0], 24, rc));
        if (rc < 0) return;
        g_shellUp = 1;
        step(first);
    }
    char path[64];
    snprintf(path, sizeof(path), "%s", g_tracks[song - 1].path);
    if (first) logLine("player: step 3 of 4%s: stop, open %s", g_away ? ", away" : "", path);
    const int rcStop = sceMusicPlayerServiceSendEvent(SCE_MUSIC_EVENTID_STOP, 0);
    const int rcOpen = sceMusicPlayerServiceOpen(path, NULL);
    if (first) {
        logLine("player: stop %s, open %s", result(r[0], 24, rcStop), result(r[1], 24, rcOpen));
        step(first);
        logLine("player: step 4 of 4%s: play", g_away ? ", away" : "");
    }
    const int rcPlay = sceMusicPlayerServiceSendEvent(SCE_MUSIC_EVENTID_PLAY, 0);
    g_handedOver = 1;
    logLine("player: song %d handed to the system player%s (%s): stop %s, open %s, play %s",
            song, g_away ? ", away" : "", path, result(r[0], 24, rcStop),
            result(r[1], 24, rcOpen), result(r[2], 24, rcPlay));
}

// Watches song play until it ends: its clock moving with the length the
// server gives it loaded, then either going back after the last 10 s or the
// state leaving 1 (playing) there. The length is what tells it from the file
// played before, which the shell goes on with when an open does not take and
// whose clock the first readings after an open can show. 1 when it ended.
static int watchSong(int song) {
    const unsigned durMs = (unsigned)g_tracks[song - 1].durationMs;
    char dur[16], at[16], r[24];
    mmss(dur, sizeof(dur), durMs);
    int lastState = -100, started = 0, otherLogged = 0, havePrev = 0;
    unsigned maxTime = 0, prevTime = 0;
    const double t0 = now();
    double lastLine = t0;
    for (;;) {
        sceKernelDelayThread(500 * 1000);
        SceMusicPlayerServicePlayStatusExtension st;
        memset(&st, 0, sizeof(st));
        const int rc = sceMusicPlayerServiceGetPlayStatusExtension(&st);
        const int state = st.currentState;
        const unsigned time = st.currentTime;
        const double t = now();
        mmss(at, sizeof(at), time);
        if (state != lastState) {
            logLine("player: song %d: system player state %d at %s (%s)%s", song, state, at,
                    result(r, sizeof(r), rc), g_away ? ", away" : "");
            lastState = state;
        }
        if (!started && state == 1 && havePrev && time > prevTime) {
            static SceMusicPlayerServiceTrackInfo info;
            memset(&info, 0, sizeof(info));
            const int rcInfo = sceMusicPlayerServiceGetTrackInfo(&info);
            const int diff = info.duration - (int)durMs;
            char len[16];
            mmss(len, sizeof(len), (unsigned)(info.duration > 0 ? info.duration : 0));
            if (rcInfo >= 0 && diff > -3000 && diff < 3000) {
                started = 1;
                maxTime = time;
                logLine("player: song %d plays: the system player has it as %s long, the server "
                        "%s, and is at %s",
                        song, len, dur, at);
            } else if (!otherLogged) {
                logLine("player: song %d: the system player is playing something %s long, not "
                        "this song of %s (%s)",
                        song, len, dur, result(r, sizeof(r), rcInfo));
                otherLogged = 1;
            }
        }
        prevTime = time;
        havePrev = 1;
        if (t - lastLine >= 5.0) {
            logLine("player: song %d at %s of %s, state %d%s", song, at, dur, state,
                    g_away ? ", away" : "");
            lastLine = t;
        }
        if (started) {
            if (time > maxTime) maxTime = time;
            if (maxTime + 10000 >= durMs && (time + 5000 < maxTime || state != 1)) {
                mmss(at, sizeof(at), maxTime);
                logLine("player: song %d ended (it got to %s of %s)", song, at, dur);
                return 1;
            }
        } else if (t - t0 > 15.0) {
            logLine("player: song %d: the system player never started it", song);
            return 0;
        }
        if (t - t0 > durMs / 1000.0 + 60.0) {
            mmss(at, sizeof(at), maxTime);
            logLine("player: song %d did not end in time (it got to %s of %s)", song, at, dur);
            return 0;
        }
    }
}

static void stopShell(const char* why) {
    char a[24], b[24];
    if (g_shellUp) {
        g_shellUp = 0;
        const int rcStop = sceMusicPlayerServiceSendEvent(SCE_MUSIC_EVENTID_STOP, 0);
        const int rcTerm = sceMusicPlayerServiceTerminate();
        logLine("player: system player stop %s, terminate %s (%s)", result(a, 24, rcStop),
                result(b, 24, rcTerm), why);
    }
    if (g_portHeld) {
        g_portHeld = 0;
        logLine("player: release BGM port: %s", result(a, 24, sceAppMgrReleaseBgmPort()));
    }
}

// Asked to quit once a song is with the shell, it is left playing, to see
// whether the shell plays it on with this app gone; it stops at the song's
// end. Before that, everything is put back.
void playerQuit(void) {
    if (g_handedOver) {
        logLine("player: asked to quit with a song handed over; leaving the system player to "
                "play it");
        return;
    }
    stopShell("asked to quit");
}

// ── The run ──────────────────────────────────────────────────────────────

static int secondMain(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    g_secondReady = download(2) == 0 ? 1 : -1;
    return 0;
}

static int playerMain(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    if (readSettings() < 0) return 0;
    for (int i = 0; i < 300 && !g_netReady; i++) sceKernelDelayThread(100 * 1000);
    if (!g_netReady) {
        logLine("player: the network did not start");
        return 0;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const int songs = pickTracks();
    if (songs == 0) return 0;

    // Time to press PS and start a game: 15 s after being sent away, or after
    // five minutes in front.
    logLine("player: waiting for PS, then 15 s for a game to start");
    const double t0 = now();
    double awaySince = 0;
    for (;;) {
        const double t = now();
        if (g_away) {
            if (awaySince == 0) awaySince = t;
            if (t - awaySince >= 15.0) break;
        } else {
            awaySince = 0;
        }
        if (t - t0 >= 300.0) break;
        sceKernelDelayThread(250 * 1000);
    }
    logLine("player: starting%s", g_away ? ", away" : " in front (never sent away)");

    if (download(1) < 0) return 0;
    handOver(1);
    SceUID second = -1;
    if (songs > 1) {
        second = sceKernelCreateThread("BgTestSecond", secondMain, 0x10000100 + 20, 256 * 1024, 0,
                                       0, NULL);
        if (second >= 0) sceKernelStartThread(second, 0, NULL);
    }
    watchSong(1);
    if (second >= 0) {
        sceKernelWaitThreadEnd(second, NULL, NULL);
        if (g_secondReady == 1) {
            handOver(2);
            watchSong(2);
        }
    }
    stopShell("test over");
    logLine("player: done");
    return 0;
}

void playerStart(void) {
    const SceUID t =
        sceKernelCreateThread("BgTestPlayer", playerMain, 0x10000100 + 20, 256 * 1024, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}
