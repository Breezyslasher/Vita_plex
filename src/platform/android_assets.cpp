#if defined(__ANDROID__)

#include "platform/android_assets.hpp"

#include <jni.h>
#include <SDL2/SDL.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <borealis/core/logger.hpp>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static AAssetManager* getAssetManager() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    jclass clazz = env->GetObjectClass(activity);
    jmethodID getAssets = env->GetMethodID(clazz, "getAssets",
                                           "()Landroid/content/res/AssetManager;");
    jobject jAssetMgr = env->CallObjectMethod(activity, getAssets);
    AAssetManager* mgr = AAssetManager_fromJava(env, jAssetMgr);
    env->DeleteLocalRef(jAssetMgr);
    env->DeleteLocalRef(clazz);
    env->DeleteLocalRef(activity);
    return mgr;
}

static void mkdirRecursive(const std::string& path) {
    std::string partial;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/' && i > 0) {
            partial = path.substr(0, i);
            mkdir(partial.c_str(), 0755);
        }
    }
    mkdir(path.c_str(), 0755);
}

// Use Java AssetManager.list() to enumerate entries in an asset directory.
// AAssetDir_getNextFileName() only returns files, not subdirectories, so we
// must go through JNI to get the full listing (files + dirs).
static std::vector<std::string> listAssetDir(JNIEnv* env, jobject jAssetMgr,
                                              const std::string& path) {
    std::vector<std::string> result;

    jclass amClass = env->GetObjectClass(jAssetMgr);
    jmethodID listMethod = env->GetMethodID(amClass, "list",
                                            "(Ljava/lang/String;)[Ljava/lang/String;");
    jstring jPath = env->NewStringUTF(path.c_str());
    auto jArray = static_cast<jobjectArray>(
        env->CallObjectMethod(jAssetMgr, listMethod, jPath));
    env->DeleteLocalRef(jPath);

    if (jArray) {
        jsize count = env->GetArrayLength(jArray);
        for (jsize i = 0; i < count; ++i) {
            auto jStr = static_cast<jstring>(env->GetObjectArrayElement(jArray, i));
            const char* utf = env->GetStringUTFChars(jStr, nullptr);
            result.emplace_back(utf);
            env->ReleaseStringUTFChars(jStr, utf);
            env->DeleteLocalRef(jStr);
        }
        env->DeleteLocalRef(jArray);
    }
    env->DeleteLocalRef(amClass);
    return result;
}

// Recursively extract an asset directory to destBase.
// assetPath  – path inside the APK assets/ (e.g. "resources/xml")
// destBase   – writable root on internal storage
static void extractDirRecursive(JNIEnv* env, jobject jAssetMgr,
                                AAssetManager* nativeMgr,
                                const std::string& assetPath,
                                const std::string& destBase) {
    auto entries = listAssetDir(env, jAssetMgr, assetPath);

    for (const auto& entry : entries) {
        std::string srcPath = assetPath + "/" + entry;
        std::string dstPath = destBase + "/" + srcPath;

        // Try to open as a file first.
        AAsset* asset = AAssetManager_open(nativeMgr, srcPath.c_str(),
                                           AASSET_MODE_BUFFER);
        if (asset) {
            // It is a file — extract it.
            std::string parentDir = dstPath.substr(0, dstPath.rfind('/'));
            mkdirRecursive(parentDir);

            off_t len = AAsset_getLength(asset);
            const void* buf = AAsset_getBuffer(asset);

            FILE* fp = fopen(dstPath.c_str(), "wb");
            if (fp) {
                fwrite(buf, 1, static_cast<size_t>(len), fp);
                fclose(fp);
            }
            AAsset_close(asset);
        }

        // Whether it was a file or not, try to recurse into it as a directory.
        // listAssetDir returns empty for non-directories, so this is safe.
        extractDirRecursive(env, jAssetMgr, nativeMgr, srcPath, destBase);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void extractAndroidAssets() {
    const char* internalPath = SDL_AndroidGetInternalStoragePath();
    if (!internalPath || internalPath[0] == '\0') {
        brls::Logger::error("extractAndroidAssets: SDL_AndroidGetInternalStoragePath returned null");
        return;
    }

    brls::Logger::info("extractAndroidAssets: extracting to {}", internalPath);

    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    jclass clazz = env->GetObjectClass(activity);
    jmethodID getAssets = env->GetMethodID(clazz, "getAssets",
                                           "()Landroid/content/res/AssetManager;");
    jobject jAssetMgr = env->CallObjectMethod(activity, getAssets);
    AAssetManager* nativeMgr = AAssetManager_fromJava(env, jAssetMgr);

    // Extract the entire resources/ tree from the APK to internal storage.
    extractDirRecursive(env, jAssetMgr, nativeMgr,
                        "resources", std::string(internalPath));

    env->DeleteLocalRef(jAssetMgr);
    env->DeleteLocalRef(clazz);
    env->DeleteLocalRef(activity);

    // Change working directory so that fopen("resources/...") resolves.
    if (chdir(internalPath) != 0) {
        brls::Logger::error("extractAndroidAssets: chdir failed");
    }

    brls::Logger::info("extractAndroidAssets: done, cwd = {}", internalPath);
}

#endif // __ANDROID__
