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
 *      plays, and handed over when the first ends. The server's MP3 has no
 *      length header, and the shell made a 2:51 song 19:17 long from it (log
 *      1a1cef7f), so it is handed over as a copy with one in front.
 *
 * Plex makes the MP3 as it sends it and may hold back once it is well ahead,
 * which the second download's speed shows; an original file has no such
 * limit.
 *
 * VPLXBGT11 is the same app started by VitaPlex BG from its settings, with
 * the song VitaPlex was playing, where it paused it and the next one in its
 * queue. It hands those to the shell at once, the first taken on to where it
 * was paused, and quits when they are over. What that tries is what a player
 * built this way needs: whether VitaPlex can start it and stay in front, and
 * whether the shell seeks.
 *
 * In VPLXBGT14 to 16 it is the app's background program (bgapp.c), which is
 * never in front, so it plays two songs from the library at once.
 *
 * Requests go over https as VitaPlex makes them, with curl, mbedtls and
 * Mozilla's CA bundle (packaged beside the eboot). The sign-in token comes
 * from VitaPlex's settings.json and goes to the server in the requests, and
 * no URL is logged past its path.
 */

#include "bgtest.h"
#include "mp3_info.h"

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
// VPLXBGT11: the songs are the ones VitaPlex handed over, not picked from the
// library, and the first goes on from where VitaPlex paused it.
static int g_handoff;
static unsigned g_startMs;
// VPLXBGT14 to 21: the app's background program, started at once.
static int g_background;
// playerStop: the music is over, and nothing is to start it again.
static volatile int g_stopped;
// The CA bundle packaged beside the eboot (findCaBundle).
static char g_caPath[64] = "app0:cacert.pem";

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
    curl_easy_setopt(c, CURLOPT_CAINFO, g_caPath);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    return c;
}

// Whether the background program has its app's files at app0:, as an app has
// its own: nothing says so, and without the CA bundle no request works. If
// not, it looks in the app's directory, and the log says which it got.
static void findCaBundle(void) {
    FILE* f = fopen(g_caPath, "rb");
    if (f) {
        fclose(f);
        logLine("player: CA bundle at %s, so app0: is the app's", g_caPath);
        return;
    }
    char alt[64];
    snprintf(alt, sizeof(alt), "ux0:app/%s/cacert.pem", bgtestTitle());
    f = fopen(alt, "rb");
    logLine("player: no %s; %s %s", g_caPath, f ? "using" : "nor", alt);
    if (f) {
        fclose(f);
        snprintf(g_caPath, sizeof(g_caPath), "%s", alt);
    }
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

// The Track element at p, up to the next one: its ids, title, artist, length
// and first part. 0 when it has a ratingKey and a part to fetch.
static int trackAt(const char* p, Track* t) {
    memset(t, 0, sizeof(*t));
    char num[24];
    xmlAttr(p, "ratingKey", t->ratingKey, sizeof(t->ratingKey));
    xmlAttr(p, "title", t->title, sizeof(t->title));
    xmlAttr(p, "grandparentTitle", t->artist, sizeof(t->artist));
    if (xmlAttr(p, "duration", num, sizeof(num)) == 0) t->durationMs = atoi(num);
    const char* next = strstr(p + 1, "<Track ");
    const char* part = strstr(p, "<Part ");
    if (!part || (next && part > next)) return -1;
    xmlAttr(part, "key", t->partKey, sizeof(t->partKey));
    if (xmlAttr(part, "size", num, sizeof(num)) == 0) t->size = atoll(num);
    const char* dot = strrchr(t->partKey, '.');
    snprintf(t->ext, sizeof(t->ext), "%s", dot ? dot + 1 : "");
    for (char* e = t->ext; *e; e++)
        if (*e >= 'A' && *e <= 'Z') *e += 'a' - 'A';
    if (strcmp(t->ext, "mp4") == 0) strcpy(t->ext, "m4a");
    return t->ratingKey[0] && t->partKey[0] ? 0 : -1;
}

// Where each of the n songs is saved (ux0:data/VitaPlex/<prefix>-N.ext), and
// what they are, in the log.
static void nameTracks(int n, const char* prefix) {
    for (int i = 0; i < n; i++) {
        Track* s = &g_tracks[i];
        snprintf(s->path, sizeof(s->path), "ux0:data/VitaPlex/%s-%d.%s", prefix, i + 1,
                 s->transcode ? "mp3" : s->ext);
        char d[16];
        mmss(d, sizeof(d), (unsigned)s->durationMs);
        logLine("player: song %d: \"%s\" by %s, %s, %s file of %.1f MB; fetched %s", i + 1,
                s->title, s->artist, d, s->ext, s->size / (double)MB,
                s->transcode ? "as the server's MP3" : "as it is");
    }
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
        if (trackAt(p, &t) != 0) continue;
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
    nameTracks(n, g_background ? "bgtest14" : "bgtest10");
    if (n == 0) logLine("player: no song of 1 to 4 minutes among %d tracks", seen);
    return n;
}

// ── BG Test 11: what VitaPlex hands over ─────────────────────────────────

// VitaPlex BG writes the song it was playing, where it paused it and the next
// one in its queue (background_audio_psv.cpp, runHandoffTest), one field a
// line, then starts this app. Read once and removed, so that starting this
// from the LiveArea later does not play an old one. 0 when there is a song in
// it.
static int readHandoff(char* current, char* next, size_t size, unsigned* positionMs,
                       long* age) {
    const char* path = "ux0:data/VitaPlex/bgtest11-handoff.txt";
    FILE* f = fopen(path, "r");
    if (!f) {
        logLine("player: nothing handed over (no %s); start this from VitaPlex BG's settings",
                path);
        return -1;
    }
    char line[128];
    long written = 0;
    current[0] = next[0] = '\0';
    *positionMs = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "ratingKey=", 10) == 0) snprintf(current, size, "%s", line + 10);
        else if (strncmp(line, "next=", 5) == 0) snprintf(next, size, "%s", line + 5);
        else if (strncmp(line, "positionMs=", 11) == 0)
            *positionMs = (unsigned)strtoul(line + 11, NULL, 10);
        else if (strncmp(line, "written=", 8) == 0) written = strtol(line + 8, NULL, 10);
    }
    fclose(f);
    remove(path);
    *age = written > 0 ? (long)time(NULL) - written : -1;
    return current[0] ? 0 : -1;
}

// A track's details from the server, as the library list gives them.
static int fetchTrack(const char* ratingKey, Track* t) {
    char path[48];
    snprintf(path, sizeof(path), "/library/metadata/%s", ratingKey);
    Body b = {0};
    const int status = getBody(path, NULL, &b);
    const char* p = b.data ? strstr(b.data, "<Track ") : NULL;
    const int ok = p && trackAt(p, t) == 0;
    free(b.data);
    if (!ok) logLine("player: track %s: HTTP %d, nothing to play in it", ratingKey, status);
    return ok ? 0 : -1;
}

// The songs VitaPlex handed over, each fetched as the shell decodes it when it
// can, else as the server's MP3, as a player would. The first starts where
// VitaPlex paused it.
static int handoffTracks(void) {
    char current[16], next[16];
    unsigned positionMs = 0;
    long age = -1;
    if (readHandoff(current, next, sizeof(current), &positionMs, &age) != 0) return 0;
    char at[16], when[32];
    mmss(at, sizeof(at), positionMs);
    if (age >= 0)
        snprintf(when, sizeof(when), "%lds ago", age);
    else
        snprintf(when, sizeof(when), "at a time not given");
    logLine("player: handed over %s: track %s at %s, then %s", when, current, at,
            next[0] ? next : "nothing");
    int n = 0;
    if (fetchTrack(current, &g_tracks[n]) != 0) return 0;
    n++;
    if (next[0] && fetchTrack(next, &g_tracks[n]) == 0) n++;
    for (int i = 0; i < n; i++) g_tracks[i].transcode = !shellDecodes(g_tracks[i].ext);
    nameTracks(n, "bgtest11");

    // A seek of a few seconds could not be told from the song playing, and
    // one to its last seconds leaves nothing to hear.
    if (positionMs < 10000)
        logLine("player: song 1 was paused only %s in, so it starts from the beginning", at);
    else if (positionMs + 10000 > (unsigned)g_tracks[0].durationMs)
        logLine("player: song 1 was paused at %s, near its end, so it starts from the beginning",
                at);
    else
        g_startMs = positionMs;
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

// What the shell has to go on for an MP3's length (mp3_info.h). The server's
// MP3 of a 2:51 song came out 19:17 long to it (log 1a1cef7f), which is that
// file's size at 32 kbps. 0 with *m filled in, or -1.
static int logMp3(int song, const char* path, Mp3Info* m) {
    if (mp3Inspect(path, m) != 0) {
        logLine("player: song %d: no MP3 frame found in %s", song, path);
        return -1;
    }
    char header[64], len[16], guess[16];
    if (m->header[0])
        snprintf(header, sizeof(header), "a %s header for %ld frames", m->header,
                 m->headerFrames);
    else
        snprintf(header, sizeof(header), "no Xing, Info or VBRI header");
    mmss(len, sizeof(len), (unsigned)(m->seconds * 1000));
    mmss(guess, sizeof(guess), (unsigned)(mp3FirstFrameGuess(m) * 1000));
    logLine("player: song %d: MP3 of %ld bytes: ID3 tag %ld bytes; first frame %d kbps, MPEG-%s, "
            "%d Hz; %s; %ld frames make %s at %d to %d kbps; %ld bytes after them. The first "
            "frame's bitrate alone makes it %s long",
            song, m->fileSize, m->id3Size, m->firstKbps,
            m->version == 10 ? "1" : m->version == 20 ? "2" : "2.5", m->sampleRate, header,
            m->frames, len, m->minKbps, m->maxKbps, m->trailing, guess);
    return 0;
}

// The shell gets an MP3 the server sent with no length header as a copy with
// one in front, the same frames behind a Xing frame giving their count, to see
// whether it then has the song's length. A host check had ffmpeg read 2:51
// for a 2:51 file that way, and the same audio, where it guessed 5:34 without.
static void giveHeader(int song, Track* t, const Mp3Info* m) {
    char copy[64];   // bgtest10-2.mp3 -> bgtest10-2x.mp3
    const size_t stem = strlen(t->path) - strlen(".mp3");
    snprintf(copy, sizeof(copy), "%.*sx.mp3", (int)stem, t->path);
    if (mp3WriteWithHeader(t->path, m, copy) != 0) {
        logLine("player: song %d: could not write %s; the shell gets the file as the server sent "
                "it",
                song, copy);
        return;
    }
    logLine("player: song %d: wrote %s: the same %ld frames behind a %s header%s; the shell gets "
            "that one",
            song, copy, m->frames, m->minKbps == m->maxKbps ? "Info" : "Xing",
            m->id3Size ? ", without the ID3 tag" : "");
    snprintf(t->path, sizeof(t->path), "%s", copy);
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
    const int ok = rc == CURLE_OK && status == 200 && bytes > 0;
    if (ok && (t->transcode || strcmp(t->ext, "mp3") == 0)) {
        Mp3Info m;
        if (logMp3(song, t->path, &m) == 0 && t->transcode && !m.header[0])
            giveHeader(song, t, &m);
    }
    return ok ? 0 : -1;
}

// ── The system player ────────────────────────────────────────────────────

// The first hand-over goes one step every 5 s. Done all at once from the
// background, it was answered within 250 ms by the system asking the app to
// quit (log 7a912d67), and the watcher reads events four times a second, so
// only spacing the steps out says which one the system answers. None was (log
// 468b487f), so a song VitaPlex hands over, which someone is waiting to hear,
// goes over at once, and so does the background program's.
static void step(int first) {
    if (first && !g_handoff && !g_background) sceKernelDelayThread(5 * 1000 * 1000);
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

// The length the shell gave the last song it played; 0 before the first.
static int g_lastShellLenMs;

// Whether the shell has the song just opened, by the length it gives: the
// server's, or after an earlier song any length but that one's. An open that
// does not take leaves the shell on the file before, whose length is known,
// while its own length for an MP3 with no length header is a guess: 19:17 for
// a 2:51 song in log 1a1cef7f, which was then wrongly given up on.
static int shellHasNewSong(int shellLenMs, int serverLenMs) {
    const int d = shellLenMs - serverLenMs;
    if (d > -3000 && d < 3000) return 1;
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
static int seekLanded(int song, unsigned ms, unsigned from, const char* how, int* state,
                      double wait) {
    const double t0 = now();
    unsigned time = from;
    char at[16];
    while (now() - t0 < wait) {
        sceKernelDelayThread(250 * 1000);
        shellClock(&time, state);
        const double off = (double)time - from - (now() - t0) * 1000;
        if (time + 2000 >= ms && time <= ms + 6000 && (off > 3000 || off < -3000)) {
            mmss(at, sizeof(at), time);
            logLine("player: song %d: %s took the clock to %s in %.1fs, state %d", song, how, at,
                    now() - t0, *state);
            return 1;
        }
    }
    mmss(at, sizeof(at), time);
    logLine("player: song %d: %s left the clock at %s, state %d", song, how, at, *state);
    return 0;
}

// Waits for song to play, so that the open cannot undo a seek: the clock
// moving with this song's length (shellHasNewSong), for up to 10 s. The
// clock then, and 1 in *playing when it was seen to.
static unsigned untilPlaying(int song, int* playing) {
    const int durMs = g_tracks[song - 1].durationMs;
    unsigned time = 0, prev = 0;
    int state = 0;
    *playing = 0;
    const double t0 = now();
    for (int havePrev = 0; !*playing && now() - t0 < 10.0; havePrev = 1) {
        sceKernelDelayThread(250 * 1000);
        prev = time;
        shellClock(&time, &state);
        if (state == 1 && havePrev && time > prev) {
            static SceMusicPlayerServiceTrackInfo info;
            memset(&info, 0, sizeof(info));
            *playing = sceMusicPlayerServiceGetTrackInfo(&info) >= 0 &&
                       shellHasNewSong(info.duration, durMs);
        }
    }
    return time;
}

// Takes song to ms from the clock at time: first as ShellAudio.h has it, the
// time and then the seek event, and failing that the call that seeks by
// itself.
static void seekFrom(int song, unsigned ms, unsigned time) {
    char want[16], r[2][24];
    mmss(want, sizeof(want), ms);
    int state = 1;
    const int rcTime = sceMusicPlayerServiceSetSeekTime(ms);
    const int rcEvent = sceMusicPlayerServiceSendEvent(SCE_MUSIC_EVENTID_SEEK, 0);
    logLine("player: song %d: set seek time %s, seek event %s", song, result(r[0], 24, rcTime),
            result(r[1], 24, rcEvent));
    // The event gets the longer wait: one that lands after the call is made
    // would be taken for the call's doing.
    int landed = seekLanded(song, ms, time, "the seek event", &state, 8.0);
    if (!landed) {
        shellClock(&time, &state);
        logLine("player: song %d: seek call %s", song,
                result(r[0], 24, sceMusicPlayerServiceSeek(ms)));
        landed = seekLanded(song, ms, time, "the seek call", &state, 4.0);
    }
    if (!landed)
        logLine("player: song %d: neither way took it to %s; it goes on from where it is", song,
                want);
    if (state != 1) {
        // Not playing: a seek that leaves the song paused, or a song that
        // ended while it was taken back.
        logLine("player: song %d: not playing after the seek; play %s", song,
                result(r[0], 24, sceMusicPlayerServiceSendEvent(SCE_MUSIC_EVENTID_PLAY, 0)));
    }
}

// Takes song on to ms, where VitaPlex paused it. Which way works, if either,
// is half of what VPLXBGT11 is for.
static void seekTo(int song, unsigned ms) {
    char want[16], at[16];
    mmss(want, sizeof(want), ms);
    int playing = 0;
    const double t0 = now();
    const unsigned time = untilPlaying(song, &playing);
    mmss(at, sizeof(at), time);
    logLine("player: song %d: %s after %.1fs, at %s; seeking to %s", song,
            playing ? "playing" : "not seen playing", now() - t0, at, want);
    seekFrom(song, ms, time);
}

// The shell can start a file it has had before where it left it: song 1 of
// VPLXBGT21's first run started at 3:21 and was over 2 s later (log
// 2aefa839). That was the file the player before had handed over, which the
// shell played on by itself once that player was ended. A song that should
// start at 0:00 and starts more than 5 s in is taken back there.
static void fromTheStart(int song) {
    int playing = 0;
    const unsigned time = untilPlaying(song, &playing);
    if (!playing || time < 5000) return;
    char at[16];
    mmss(at, sizeof(at), time);
    logLine("player: song %d: the system player started it at %s, not 0:00; seeking back", song,
            at);
    seekFrom(song, 0, time);
}

// The power button: the system stops the shell's music as it suspends apps
// (0x00400000, which came with the press in log 91252932, and the song was
// stopped at 1:47 half a second later). ElevenMPV-A sends play again from its
// power callback at that event (PowerCallback in its utils.cpp), and so does
// this, from main.c's.
void playerPowerEvent(int powerInfo) {
    if (!(powerInfo & 0x00400000) || !g_handedOver || !g_shellUp) return;
    char r[24];
    logLine("player: apps suspending (power button?); play again, as ElevenMPV-A does: %s",
            result(r, sizeof(r), sceMusicPlayerServiceSendEvent(SCE_MUSIC_EVENTID_PLAY, 0)));
}

// Watches song play until it ends: its clock moving with a new song loaded
// (shellHasNewSong), then either going back after the last 10 s or the state
// leaving 1 (playing) there, against the server's length. The first readings
// after an open can still show the file before's clock. 1 when it ended.
static int watchSong(int song) {
    const unsigned durMs = (unsigned)g_tracks[song - 1].durationMs;
    char dur[16], at[16], r[24];
    mmss(dur, sizeof(dur), durMs);
    int lastState = -100, started = 0, otherLogged = 0, havePrev = 0;
    unsigned maxTime = 0, prevTime = 0;
    const double t0 = now();
    double lastLine = t0;
    // A song stopped part-way: since when, and for how long in all, which the
    // time it is given leaves out.
    double stoppedSince = 0, stoppedFor = 0, lastPlay = 0;
    for (;;) {
        sceKernelDelayThread(500 * 1000);
        // Stopped from another thread, which is quitting: not played again.
        if (g_stopped) return 0;
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
            if (rcInfo >= 0 && shellHasNewSong(info.duration, (int)durMs)) {
                started = 1;
                maxTime = time;
                if (diff > -3000 && diff < 3000) {
                    logLine("player: song %d plays: the system player has it as %s long, the "
                            "server %s, and is at %s",
                            song, len, dur, at);
                } else {
                    char last[16];
                    mmss(last, sizeof(last), (unsigned)g_lastShellLenMs);
                    logLine("player: song %d plays: the system player has it as %s long, not the "
                            "server's %s, but not the last song's %s either, and is at %s",
                            song, len, dur, last, at);
                }
                g_lastShellLenMs = info.duration;
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
            // Stopped before its end, as the power button stopped it in log
            // 91252932, where it never went on: play again every 5 s while it
            // stays so, and move on after two minutes of it.
            if (state != 1) {
                if (stoppedSince == 0) stoppedSince = t;
                if (t - stoppedSince > 120.0) {
                    logLine("player: song %d stayed stopped at %s for two minutes; moving on", song,
                            at);
                    return 0;
                }
                if (t - lastPlay >= 5.0) {
                    logLine("player: song %d stopped at %s, before its end (state %d); play again: "
                            "%s",
                            song, at, state,
                            result(r, sizeof(r),
                                   sceMusicPlayerServiceSendEvent(SCE_MUSIC_EVENTID_PLAY, 0)));
                    lastPlay = t;
                }
            } else if (stoppedSince > 0) {
                logLine("player: song %d plays again at %s, after %.1fs stopped", song, at,
                        t - stoppedSince);
                stoppedFor += t - stoppedSince;
                stoppedSince = 0;
            }
        } else if (t - t0 > 15.0) {
            logLine("player: song %d: the system player never started it", song);
            return 0;
        }
        const double stopped = stoppedFor + (stoppedSince > 0 ? t - stoppedSince : 0);
        if (t - t0 - stopped > durMs / 1000.0 + 60.0) {
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
// end. Before that, everything is put back. The same when VPLXBGT12 or 13
// switches back to its other program.
void playerQuit(const char* why) {
    if (g_handedOver) {
        logLine("player: %s with a song handed over; leaving the system player to play it", why);
        return;
    }
    stopShell(why);
}

void playerStop(const char* why) {
    g_stopped = 1;
    stopShell(why);
}

// ── The run ──────────────────────────────────────────────────────────────

static int secondMain(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    g_secondReady = download(2) == 0 ? 1 : -1;
    return 0;
}

static void run(void) {
    if (readSettings() < 0) return;
    for (int i = 0; i < 300 && !g_netReady; i++) sceKernelDelayThread(100 * 1000);
    if (!g_netReady) {
        logLine("player: the network did not start");
        return;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (g_background) findCaBundle();
    const int songs = g_handoff ? handoffTracks() : pickTracks();
    if (songs == 0) return;

    if (g_handoff) {
        // VitaPlex has paused the song for this, so it goes on at once.
        logLine("player: starting at once%s", g_away ? ", away" : "");
    } else if (g_background) {
        // Never in front, so there is nothing to be sent away from.
        logLine("player: starting at once, as the app's background program");
    } else {
        // Time to press PS and start a game: 15 s after being sent away, or
        // after five minutes in front.
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
    }

    if (download(1) < 0 || g_stopped) return;
    handOver(1);
    if (g_startMs > 0)
        seekTo(1, g_startMs);
    else
        fromTheStart(1);
    SceUID second = -1;
    if (songs > 1) {
        second = sceKernelCreateThread("BgTestSecond", secondMain, 0x10000100 + 20, 256 * 1024, 0,
                                       0, NULL);
        if (second >= 0) sceKernelStartThread(second, 0, NULL);
    }
    watchSong(1);
    if (second >= 0) {
        sceKernelWaitThreadEnd(second, NULL, NULL);
        if (g_secondReady == 1 && !g_stopped) {
            handOver(2);
            fromTheStart(2);
            watchSong(2);
        }
    }
    stopShell("test over");
    logLine("player: done");
}

static int playerMain(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    run();
    // Started by VitaPlex, it quits once that is over: it reads a hand-over
    // only as it starts, so the next one needs it started afresh.
    if (g_handoff) {
        logLine("player: quitting, so that the next hand-over starts this afresh");
        sceKernelExitProcess(0);
    }
    // The background program stays a while once the songs are over, for its
    // app's LiveArea to be closed (BGFTP's README says that ends one), and
    // then goes, so that it does not outlive the test.
    if (g_background) {
        logLine("player: staying 10 minutes, then quitting");
        sceKernelDelayThread(600 * 1000 * 1000);
        logLine("player: quitting, 10 minutes after the songs");
        sceKernelExitProcess(0);
    }
    return 0;
}

void playerStart(int mode) {
    g_handoff = mode == PLAYER_HANDOFF;
    g_background = mode == PLAYER_BACKGROUND;
    const SceUID t =
        sceKernelCreateThread("BgTestPlayer", playerMain, 0x10000100 + 20, 256 * 1024, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}
