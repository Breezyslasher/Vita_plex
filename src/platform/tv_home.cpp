/**
 * VitaPlex - Android TV home-screen rows (implementation)
 *
 * See include/platform/tv_home.hpp. The Android path marshals the item lists
 * into parallel Java arrays for org.VitaPlex.app.TvChannels, which owns the
 * content-provider work; every other platform links the no-ops at the bottom.
 */

#include "platform/tv_home.hpp"

#include <borealis.hpp>

#ifdef __ANDROID__
#include <SDL2/SDL.h>
#include <jni.h>

#include <string>
#endif

namespace vitaplex {
namespace tvhome {

#ifdef __ANDROID__

namespace {

// How many items each surface gets. Both cross a content provider and are only
// ever seen a few tiles at a time, so publishing the whole library would be
// work nobody looks at.
constexpr size_t kMaxWatchNext = 12;
constexpr size_t kMaxChannel = 20;

// Cover art size for home-screen tiles. The launcher renders them small, and
// these are fetched by the system rather than by us, so keep the server's job
// cheap.
constexpr int kPosterW = 400, kPosterH = 600;
constexpr int kStillW = 640, kStillH = 360;

struct ScopedFrame {
    JNIEnv* env;
    bool ok;
    explicit ScopedFrame(JNIEnv* e, jint capacity) : env(e), ok(false) {
        // Each item contributes several strings; a local frame keeps them from
        // accumulating in the caller's reference table.
        ok = env && env->PushLocalFrame(capacity) == 0;
    }
    ~ScopedFrame() { if (ok) env->PopLocalFrame(nullptr); }
};

jobjectArray makeStringArray(JNIEnv* env, jclass strCls,
                             const std::vector<std::string>& values) {
    jobjectArray arr = env->NewObjectArray((jsize)values.size(), strCls, nullptr);
    if (!arr) return nullptr;
    for (jsize i = 0; i < (jsize)values.size(); i++) {
        jstring s = env->NewStringUTF(values[(size_t)i].c_str());
        env->SetObjectArrayElement(arr, i, s);
        env->DeleteLocalRef(s);   // released immediately: the array holds it now
    }
    return arr;
}

// Episodes read better as a 16:9 still, everything else as a poster.
std::string artFor(const MediaItem& item) {
    PlexClient& client = PlexClient::getInstance();
    if (item.mediaType == MediaType::EPISODE) {
        if (!item.thumb.empty()) return client.getThumbnailUrl(item.thumb, kStillW, kStillH);
        if (!item.grandparentThumb.empty())
            return client.getThumbnailUrl(item.grandparentThumb, kPosterW, kPosterH);
        return {};
    }
    if (!item.thumb.empty()) return client.getThumbnailUrl(item.thumb, kPosterW, kPosterH);
    return {};
}

// An episode's row title reads "Show" with the episode as the subtitle; a movie
// is just its own title.
std::string titleFor(const MediaItem& item) {
    if (item.mediaType == MediaType::EPISODE && !item.grandparentTitle.empty())
        return item.grandparentTitle;
    return item.title;
}

std::string subtitleFor(const MediaItem& item) {
    if (item.mediaType == MediaType::EPISODE) return item.title;
    return item.summary;
}

}  // namespace

void publishContinueWatching(const std::vector<MediaItem>& items) {
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    if (!env) return;

    const size_t n = items.size() < kMaxWatchNext ? items.size() : kMaxWatchNext;
    std::vector<std::string> ids, titles, subtitles, arts;
    std::vector<jlong> positions, durations;
    std::vector<jint> episodeInfo;   // season, episode per item
    ids.reserve(n); titles.reserve(n); subtitles.reserve(n); arts.reserve(n);
    positions.reserve(n); durations.reserve(n); episodeInfo.reserve(n * 2);

    for (size_t i = 0; i < n; i++) {
        const MediaItem& it = items[i];
        if (it.ratingKey.empty()) continue;
        ids.push_back(it.ratingKey);
        titles.push_back(titleFor(it));
        subtitles.push_back(subtitleFor(it));
        arts.push_back(artFor(it));
        positions.push_back((jlong)it.viewOffset);           // Plex reports ms
        durations.push_back((jlong)it.duration * 1000LL);    // MediaItem is seconds
        episodeInfo.push_back((jint)it.parentIndex);
        episodeInfo.push_back((jint)it.index);
    }
    if (ids.empty()) return;

    ScopedFrame frame(env, (jint)(ids.size() * 4 + 16));
    if (!frame.ok) return;

    jclass cls = env->FindClass("org/VitaPlex/app/TvChannels");
    if (!cls) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jmethodID mid = env->GetStaticMethodID(
        cls, "publishWatchNext",
        "([Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;[J[J[I)V");
    if (!mid) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }

    jclass strCls = env->FindClass("java/lang/String");
    jobjectArray jIds = makeStringArray(env, strCls, ids);
    jobjectArray jTitles = makeStringArray(env, strCls, titles);
    jobjectArray jSubs = makeStringArray(env, strCls, subtitles);
    jobjectArray jArts = makeStringArray(env, strCls, arts);
    jlongArray jPos = env->NewLongArray((jsize)positions.size());
    jlongArray jDur = env->NewLongArray((jsize)durations.size());
    jintArray jEp = env->NewIntArray((jsize)episodeInfo.size());
    if (!jIds || !jTitles || !jSubs || !jArts || !jPos || !jDur || !jEp) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    env->SetLongArrayRegion(jPos, 0, (jsize)positions.size(), positions.data());
    env->SetLongArrayRegion(jDur, 0, (jsize)durations.size(), durations.data());
    env->SetIntArrayRegion(jEp, 0, (jsize)episodeInfo.size(), episodeInfo.data());

    env->CallStaticVoidMethod(cls, mid, jIds, jTitles, jSubs, jArts, jPos, jDur, jEp);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

void publishRecentlyAdded(const std::vector<MediaItem>& items) {
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    if (!env) return;

    const size_t n = items.size() < kMaxChannel ? items.size() : kMaxChannel;
    std::vector<std::string> ids, titles, subtitles, arts;
    ids.reserve(n); titles.reserve(n); subtitles.reserve(n); arts.reserve(n);
    for (size_t i = 0; i < n; i++) {
        const MediaItem& it = items[i];
        if (it.ratingKey.empty()) continue;
        ids.push_back(it.ratingKey);
        titles.push_back(titleFor(it));
        subtitles.push_back(subtitleFor(it));
        arts.push_back(artFor(it));
    }
    if (ids.empty()) return;

    ScopedFrame frame(env, (jint)(ids.size() * 4 + 16));
    if (!frame.ok) return;

    jclass cls = env->FindClass("org/VitaPlex/app/TvChannels");
    if (!cls) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jmethodID mid = env->GetStaticMethodID(
        cls, "publishRecentlyAdded",
        "([Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)V");
    if (!mid) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }

    jclass strCls = env->FindClass("java/lang/String");
    jobjectArray jIds = makeStringArray(env, strCls, ids);
    jobjectArray jTitles = makeStringArray(env, strCls, titles);
    jobjectArray jSubs = makeStringArray(env, strCls, subtitles);
    jobjectArray jArts = makeStringArray(env, strCls, arts);
    if (!jIds || !jTitles || !jSubs || !jArts) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }

    env->CallStaticVoidMethod(cls, mid, jIds, jTitles, jSubs, jArts);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

#else   // ---- every other port: no TV home screen to publish to ----

void publishContinueWatching(const std::vector<MediaItem>&) {}
void publishRecentlyAdded(const std::vector<MediaItem>&) {}

#endif

} // namespace tvhome
} // namespace vitaplex
