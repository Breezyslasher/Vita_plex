/**
 * VitaPlex - shell integration, Android backend (see the header).
 *
 * Thin JNI forwarding to org.VitaPlex.app.DownloadNotification, which is where
 * the actual notification-shade work happens. Everything here is best-effort:
 * a missing class or a pending exception is cleared and dropped, because none
 * of this is worth failing a download over.
 */

#include "utils/shell_integration.hpp"

#if defined(__ANDROID__)

#include <borealis.hpp>
#include <jni.h>
#include <SDL2/SDL.h>

namespace vitaplex {
namespace shell {

namespace {

constexpr const char* kClass = "org/VitaPlex/app/DownloadNotification";

// SDL attaches its own main thread to the JVM, and every call site here is
// marshalled onto that thread by brls::sync — so there is no
// AttachCurrentThread to do, and no detach to get wrong. Note this is SDL's
// thread, not Android's UI thread; NotificationManager is thread-safe, so
// that is fine, and it is how the media notification already works.
JNIEnv* env() { return (JNIEnv*)SDL_AndroidGetJNIEnv(); }

// Look the class up per call rather than caching it. A cached jclass is a
// local reference and would go stale the moment the frame it came from popped;
// making it global to avoid that buys nothing at this call rate.
jclass findClass(JNIEnv* e) {
    jclass cls = e->FindClass(kClass);
    if (!cls) {
        if (e->ExceptionCheck()) e->ExceptionClear();
        return nullptr;
    }
    return cls;
}

} // namespace

void notify(const std::string& summary, const std::string& body) {
    JNIEnv* e = env();
    if (!e) return;
    jclass cls = findClass(e);
    if (!cls) return;

    jmethodID mid = e->GetStaticMethodID(
        cls, "showComplete", "(Ljava/lang/String;Ljava/lang/String;)V");
    if (!mid) {
        if (e->ExceptionCheck()) e->ExceptionClear();
        e->DeleteLocalRef(cls);
        return;
    }

    jstring jSummary = e->NewStringUTF(summary.c_str());
    jstring jBody    = e->NewStringUTF(body.c_str());
    e->CallStaticVoidMethod(cls, mid, jSummary, jBody);
    if (e->ExceptionCheck()) e->ExceptionClear();

    e->DeleteLocalRef(jBody);
    e->DeleteLocalRef(jSummary);
    e->DeleteLocalRef(cls);
}

void setProgress(double fraction, bool visible) {
    JNIEnv* e = env();
    if (!e) return;
    jclass cls = findClass(e);
    if (!cls) return;

    jmethodID mid = e->GetStaticMethodID(cls, "setProgress", "(FZ)V");
    if (!mid) {
        if (e->ExceptionCheck()) e->ExceptionClear();
        e->DeleteLocalRef(cls);
        return;
    }

    // Negative is meaningful on the Java side (indeterminate), so only the
    // upper end is clamped.
    if (fraction > 1.0) fraction = 1.0;
    e->CallStaticVoidMethod(cls, mid, (jfloat)fraction, (jboolean)(visible ? JNI_TRUE : JNI_FALSE));
    if (e->ExceptionCheck()) e->ExceptionClear();

    e->DeleteLocalRef(cls);
}

} // namespace shell
} // namespace vitaplex

#endif // __ANDROID__
