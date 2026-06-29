/**
 * VitaPlex - MPRIS (Linux desktop) backend for the OS "Now Playing" bridge.
 *
 * Implements org.mpris.MediaPlayer2 + org.mpris.MediaPlayer2.Player over D-Bus so
 * the desktop environment's media controls and the multimedia keys drive VitaPlex
 * even when its window isn't focused (GNOME/KDE media widget, playerctl, headset
 * keys, etc.). Mirrors what MediaNotification does on Android.
 *
 * Threading: one private D-Bus connection lives entirely on a dedicated thread.
 * Incoming method calls are answered there; transport actions are marshalled onto
 * the borealis main loop via brls::sync (MusicQueue isn't thread-safe). update()/
 * clear() (main thread) only touch shared state under a mutex + raise a dirty
 * flag; the thread emits PropertiesChanged. No D-Bus object is ever touched from
 * two threads.
 *
 * Compiled only when VITAPLEX_MPRIS is defined (CMake, when dbus-1 is found).
 */

#if defined(VITAPLEX_MPRIS)

#include "utils/now_playing.hpp"

#include <borealis.hpp>
#include <dbus/dbus.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vitaplex {
namespace nowplaying {
namespace detail {

void mprisUpdate(const Info& info);
void mprisClear();

} // namespace detail

namespace {

constexpr const char* BUS_NAME    = "org.mpris.MediaPlayer2.vitaplex";
constexpr const char* OBJECT_PATH = "/org/mpris/MediaPlayer2";
constexpr const char* IFACE_ROOT  = "org.mpris.MediaPlayer2";
constexpr const char* IFACE_PLAYER = "org.mpris.MediaPlayer2.Player";
constexpr const char* IFACE_PROPS = "org.freedesktop.DBus.Properties";
constexpr const char* IFACE_INTRO = "org.freedesktop.DBus.Introspectable";

// --- shared state (guarded by g_mutex) -------------------------------------
std::mutex g_mutex;
Info g_info;                 // last published snapshot
bool g_active = false;       // false => PlaybackStatus "Stopped"
long long g_trackNo = 0;     // bumped on track identity change (mpris:trackid)

std::atomic<bool> g_dirty{false};   // state changed -> emit PropertiesChanged
std::atomic<bool> g_quit{false};
std::atomic<bool> g_threadStarted{false};
std::thread g_thread;

struct Snapshot {
    Info info;
    bool active;
    long long trackNo;
};

Snapshot snapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return Snapshot{g_info, g_active, g_trackNo};
}

const char* statusOf(const Snapshot& s) {
    if (!s.active) return "Stopped";
    return s.info.playing ? "Playing" : "Paused";
}

// MPRIS wants a URI for mpris:artUrl. Pass http(s) through; turn a local path
// into a file:// URI; drop anything else.
std::string artUri(const std::string& art) {
    if (art.empty()) return "";
    if (art.rfind("http://", 0) == 0 || art.rfind("https://", 0) == 0 ||
        art.rfind("file://", 0) == 0)
        return art;
    if (art[0] == '/') return "file://" + art;
    return "";
}

// --- low-level variant append helpers --------------------------------------
void vStr(DBusMessageIter* it, const std::string& s) {
    const char* c = s.c_str();
    DBusMessageIter v;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "s", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &c);
    dbus_message_iter_close_container(it, &v);
}
void vBool(DBusMessageIter* it, bool b) {
    dbus_bool_t v = b ? 1 : 0;
    DBusMessageIter var;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "b", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &v);
    dbus_message_iter_close_container(it, &var);
}
void vI64(DBusMessageIter* it, int64_t n) {
    DBusMessageIter var;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "x", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_INT64, &n);
    dbus_message_iter_close_container(it, &var);
}
void vDouble(DBusMessageIter* it, double d) {
    DBusMessageIter var;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "d", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_DOUBLE, &d);
    dbus_message_iter_close_container(it, &var);
}
void vEmptyStrArray(DBusMessageIter* it) {
    DBusMessageIter var, arr;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "as", &var);
    dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "s", &arr);
    dbus_message_iter_close_container(&var, &arr);
    dbus_message_iter_close_container(it, &var);
}

// dict-entry helpers for the Metadata a{sv}
void mStr(DBusMessageIter* arr, const char* key, const std::string& val) {
    DBusMessageIter de;
    dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, nullptr, &de);
    dbus_message_iter_append_basic(&de, DBUS_TYPE_STRING, &key);
    vStr(&de, val);
    dbus_message_iter_close_container(arr, &de);
}
void mObj(DBusMessageIter* arr, const char* key, const std::string& path) {
    const char* c = path.c_str();
    DBusMessageIter de, var;
    dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, nullptr, &de);
    dbus_message_iter_append_basic(&de, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&de, DBUS_TYPE_VARIANT, "o", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_OBJECT_PATH, &c);
    dbus_message_iter_close_container(&de, &var);
    dbus_message_iter_close_container(arr, &de);
}
void mI64(DBusMessageIter* arr, const char* key, int64_t n) {
    DBusMessageIter de;
    dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, nullptr, &de);
    dbus_message_iter_append_basic(&de, DBUS_TYPE_STRING, &key);
    vI64(&de, n);
    dbus_message_iter_close_container(arr, &de);
}
void mStrArray1(DBusMessageIter* arr, const char* key, const std::string& val) {
    const char* c = val.c_str();
    DBusMessageIter de, var, sub;
    dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, nullptr, &de);
    dbus_message_iter_append_basic(&de, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&de, DBUS_TYPE_VARIANT, "as", &var);
    dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "s", &sub);
    dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING, &c);
    dbus_message_iter_close_container(&var, &sub);
    dbus_message_iter_close_container(&de, &var);
    dbus_message_iter_close_container(arr, &de);
}

// Appends the Metadata property value (a VARIANT holding a{sv}).
void vMetadata(DBusMessageIter* it, const Snapshot& s) {
    DBusMessageIter var, arr;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "a{sv}", &var);
    dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "{sv}", &arr);

    std::string trackId = "/org/vitaplex/track/" + std::to_string(s.trackNo);
    mObj(&arr, "mpris:trackid", trackId);
    if (s.info.durationMs > 0) mI64(&arr, "mpris:length", s.info.durationMs * 1000);
    if (!s.info.title.empty())  mStr(&arr, "xesam:title", s.info.title);
    if (!s.info.artist.empty()) mStrArray1(&arr, "xesam:artist", s.info.artist);
    if (!s.info.album.empty())  mStr(&arr, "xesam:album", s.info.album);
    std::string art = artUri(s.info.artUrl);
    if (!art.empty()) mStr(&arr, "mpris:artUrl", art);

    dbus_message_iter_close_container(&var, &arr);
    dbus_message_iter_close_container(it, &var);
}

// Appends <iface>.<prop>'s value as a VARIANT into `it`. Returns false if unknown.
bool appendProp(DBusMessageIter* it, const std::string& iface,
                const std::string& prop, const Snapshot& s) {
    if (iface == IFACE_ROOT) {
        if (prop == "Identity")            { vStr(it, "VitaPlex"); return true; }
        if (prop == "DesktopEntry")        { vStr(it, "vitaplex"); return true; }
        if (prop == "CanQuit")             { vBool(it, false); return true; }
        if (prop == "CanRaise")            { vBool(it, false); return true; }
        if (prop == "HasTrackList")        { vBool(it, false); return true; }
        if (prop == "SupportedUriSchemes") { vEmptyStrArray(it); return true; }
        if (prop == "SupportedMimeTypes")  { vEmptyStrArray(it); return true; }
        return false;
    }
    if (iface == IFACE_PLAYER) {
        if (prop == "PlaybackStatus") { vStr(it, statusOf(s)); return true; }
        if (prop == "Metadata")       { vMetadata(it, s); return true; }
        if (prop == "CanGoNext")      { vBool(it, s.info.hasNext); return true; }
        if (prop == "CanGoPrevious")  { vBool(it, s.info.hasPrev); return true; }
        if (prop == "CanPlay")        { vBool(it, true); return true; }
        if (prop == "CanPause")       { vBool(it, true); return true; }
        if (prop == "CanSeek")        { vBool(it, true); return true; }
        if (prop == "CanControl")     { vBool(it, true); return true; }
        if (prop == "Position")       { vI64(it, s.info.positionMs * 1000); return true; }
        if (prop == "Rate")           { vDouble(it, 1.0); return true; }
        if (prop == "MinimumRate")    { vDouble(it, 1.0); return true; }
        if (prop == "MaximumRate")    { vDouble(it, 1.0); return true; }
        if (prop == "Volume")         { vDouble(it, 1.0); return true; }
        if (prop == "LoopStatus")     { vStr(it, "None"); return true; }
        if (prop == "Shuffle")        { vBool(it, false); return true; }
        return false;
    }
    return false;
}

void appendAllProps(DBusMessageIter* arr, const std::string& iface, const Snapshot& s) {
    static const char* rootProps[] = {
        "Identity", "DesktopEntry", "CanQuit", "CanRaise", "HasTrackList",
        "SupportedUriSchemes", "SupportedMimeTypes" };
    static const char* playerProps[] = {
        "PlaybackStatus", "Metadata", "CanGoNext", "CanGoPrevious", "CanPlay",
        "CanPause", "CanSeek", "CanControl", "Position", "Rate", "MinimumRate",
        "MaximumRate", "Volume", "LoopStatus", "Shuffle" };

    const char** props = nullptr;
    int n = 0;
    if (iface == IFACE_ROOT)        { props = rootProps;   n = (int)(sizeof(rootProps) / sizeof(*rootProps)); }
    else if (iface == IFACE_PLAYER) { props = playerProps; n = (int)(sizeof(playerProps) / sizeof(*playerProps)); }
    for (int i = 0; i < n; i++) {
        DBusMessageIter de;
        dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, nullptr, &de);
        const char* key = props[i];
        dbus_message_iter_append_basic(&de, DBUS_TYPE_STRING, &key);
        appendProp(&de, iface, props[i], s);
        dbus_message_iter_close_container(arr, &de);
    }
}

void sendEmptyReply(DBusConnection* conn, DBusMessage* msg) {
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (reply) { dbus_connection_send(conn, reply, nullptr); dbus_message_unref(reply); }
}

// Run a transport action on the borealis main loop (MusicQueue isn't thread-safe).
void post(Transport t)            { brls::sync([t]() { dispatchTransport(t); }); }
void postSeek(int64_t positionMs) { brls::sync([positionMs]() { dispatchSeek(positionMs); }); }

const char* INTROSPECT_XML =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>"
    "<interface name=\"org.freedesktop.DBus.Introspectable\">"
    "<method name=\"Introspect\"><arg name=\"xml\" type=\"s\" direction=\"out\"/></method>"
    "</interface>"
    "<interface name=\"org.freedesktop.DBus.Properties\">"
    "<method name=\"Get\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>"
    "<method name=\"GetAll\"><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"out\"/></method>"
    "<method name=\"Set\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"in\"/></method>"
    "<signal name=\"PropertiesChanged\"><arg type=\"s\"/><arg type=\"a{sv}\"/><arg type=\"as\"/></signal>"
    "</interface>"
    "<interface name=\"org.mpris.MediaPlayer2\">"
    "<method name=\"Raise\"/><method name=\"Quit\"/>"
    "<property name=\"Identity\" type=\"s\" access=\"read\"/>"
    "<property name=\"CanQuit\" type=\"b\" access=\"read\"/>"
    "<property name=\"CanRaise\" type=\"b\" access=\"read\"/>"
    "<property name=\"HasTrackList\" type=\"b\" access=\"read\"/>"
    "<property name=\"SupportedUriSchemes\" type=\"as\" access=\"read\"/>"
    "<property name=\"SupportedMimeTypes\" type=\"as\" access=\"read\"/>"
    "</interface>"
    "<interface name=\"org.mpris.MediaPlayer2.Player\">"
    "<method name=\"Next\"/><method name=\"Previous\"/><method name=\"Pause\"/>"
    "<method name=\"PlayPause\"/><method name=\"Stop\"/><method name=\"Play\"/>"
    "<method name=\"Seek\"><arg type=\"x\" direction=\"in\"/></method>"
    "<method name=\"SetPosition\"><arg type=\"o\" direction=\"in\"/><arg type=\"x\" direction=\"in\"/></method>"
    "<property name=\"PlaybackStatus\" type=\"s\" access=\"read\"/>"
    "<property name=\"Metadata\" type=\"a{sv}\" access=\"read\"/>"
    "<property name=\"CanGoNext\" type=\"b\" access=\"read\"/>"
    "<property name=\"CanGoPrevious\" type=\"b\" access=\"read\"/>"
    "<property name=\"CanPlay\" type=\"b\" access=\"read\"/>"
    "<property name=\"CanPause\" type=\"b\" access=\"read\"/>"
    "<property name=\"CanSeek\" type=\"b\" access=\"read\"/>"
    "<property name=\"CanControl\" type=\"b\" access=\"read\"/>"
    "<property name=\"Position\" type=\"x\" access=\"read\"/>"
    "<property name=\"Volume\" type=\"d\" access=\"readwrite\"/>"
    "</interface>"
    "</node>";

DBusHandlerResult onMessage(DBusConnection* conn, DBusMessage* msg, void*) {
    const char* iface  = dbus_message_get_interface(msg);
    const char* member = dbus_message_get_member(msg);
    if (!iface || !member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    // Introspect
    if (!std::strcmp(iface, IFACE_INTRO) && !std::strcmp(member, "Introspect")) {
        DBusMessage* reply = dbus_message_new_method_return(msg);
        const char* xml = INTROSPECT_XML;
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    // Properties
    if (!std::strcmp(iface, IFACE_PROPS)) {
        if (!std::strcmp(member, "Get")) {
            const char *pi = nullptr, *pp = nullptr;
            dbus_message_get_args(msg, nullptr, DBUS_TYPE_STRING, &pi,
                                  DBUS_TYPE_STRING, &pp, DBUS_TYPE_INVALID);
            Snapshot s = snapshot();
            DBusMessage* reply = dbus_message_new_method_return(msg);
            DBusMessageIter it;
            dbus_message_iter_init_append(reply, &it);
            if (pi && pp && appendProp(&it, pi, pp, s)) {
                dbus_connection_send(conn, reply, nullptr);
            } else {
                dbus_message_unref(reply);
                reply = dbus_message_new_error(msg, DBUS_ERROR_UNKNOWN_PROPERTY, "No such property");
                dbus_connection_send(conn, reply, nullptr);
            }
            dbus_message_unref(reply);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (!std::strcmp(member, "GetAll")) {
            const char* pi = nullptr;
            dbus_message_get_args(msg, nullptr, DBUS_TYPE_STRING, &pi, DBUS_TYPE_INVALID);
            Snapshot s = snapshot();
            DBusMessage* reply = dbus_message_new_method_return(msg);
            DBusMessageIter it, arr;
            dbus_message_iter_init_append(reply, &it);
            dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);
            if (pi) appendAllProps(&arr, pi, s);
            dbus_message_iter_close_container(&it, &arr);
            dbus_connection_send(conn, reply, nullptr);
            dbus_message_unref(reply);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (!std::strcmp(member, "Set")) {
            // We expose Volume as writable but don't track system volume; accept
            // and ignore so clients don't error.
            sendEmptyReply(conn, msg);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    // Root interface methods
    if (!std::strcmp(iface, IFACE_ROOT)) {
        if (!std::strcmp(member, "Raise") || !std::strcmp(member, "Quit")) {
            sendEmptyReply(conn, msg);   // we don't support these; ack anyway
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    // Player interface methods
    if (!std::strcmp(iface, IFACE_PLAYER)) {
        if (!std::strcmp(member, "Play"))        { sendEmptyReply(conn, msg); post(Transport::Play);     return DBUS_HANDLER_RESULT_HANDLED; }
        if (!std::strcmp(member, "Pause"))       { sendEmptyReply(conn, msg); post(Transport::Pause);    return DBUS_HANDLER_RESULT_HANDLED; }
        if (!std::strcmp(member, "PlayPause"))   { sendEmptyReply(conn, msg); post(Transport::Toggle);   return DBUS_HANDLER_RESULT_HANDLED; }
        if (!std::strcmp(member, "Next"))        { sendEmptyReply(conn, msg); post(Transport::Next);     return DBUS_HANDLER_RESULT_HANDLED; }
        if (!std::strcmp(member, "Previous"))    { sendEmptyReply(conn, msg); post(Transport::Previous); return DBUS_HANDLER_RESULT_HANDLED; }
        if (!std::strcmp(member, "Stop"))        { sendEmptyReply(conn, msg); post(Transport::Stop);     return DBUS_HANDLER_RESULT_HANDLED; }
        if (!std::strcmp(member, "Seek")) {
            int64_t offset = 0;  // relative, microseconds
            dbus_message_get_args(msg, nullptr, DBUS_TYPE_INT64, &offset, DBUS_TYPE_INVALID);
            Snapshot s = snapshot();
            int64_t target = s.info.positionMs + offset / 1000;
            if (target < 0) target = 0;
            sendEmptyReply(conn, msg);
            postSeek(target);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (!std::strcmp(member, "SetPosition")) {
            const char* path = nullptr;
            int64_t pos = 0;  // absolute, microseconds
            dbus_message_get_args(msg, nullptr, DBUS_TYPE_OBJECT_PATH, &path,
                                  DBUS_TYPE_INT64, &pos, DBUS_TYPE_INVALID);
            sendEmptyReply(conn, msg);
            postSeek(pos / 1000);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void emitPropertiesChanged(DBusConnection* conn) {
    Snapshot s = snapshot();
    DBusMessage* sig = dbus_message_new_signal(OBJECT_PATH, IFACE_PROPS, "PropertiesChanged");
    if (!sig) return;
    DBusMessageIter it, changed, inval;
    dbus_message_iter_init_append(sig, &it);
    const char* iface = IFACE_PLAYER;
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);

    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &changed);
    static const char* changedProps[] = { "PlaybackStatus", "Metadata", "CanGoNext", "CanGoPrevious" };
    for (const char* p : changedProps) {
        DBusMessageIter de;
        dbus_message_iter_open_container(&changed, DBUS_TYPE_DICT_ENTRY, nullptr, &de);
        dbus_message_iter_append_basic(&de, DBUS_TYPE_STRING, &p);
        appendProp(&de, IFACE_PLAYER, p, s);
        dbus_message_iter_close_container(&changed, &de);
    }
    dbus_message_iter_close_container(&it, &changed);

    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &inval);
    dbus_message_iter_close_container(&it, &inval);

    dbus_connection_send(conn, sig, nullptr);
    dbus_message_unref(sig);
}

void threadMain() {
    DBusError err;
    dbus_error_init(&err);
    DBusConnection* conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!conn) {
        brls::Logger::warning("MPRIS: no session bus ({})",
                              dbus_error_is_set(&err) ? err.message : "unknown");
        dbus_error_free(&err);
        return;
    }
    dbus_connection_set_exit_on_disconnect(conn, FALSE);

    int r = dbus_bus_request_name(conn, BUS_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (dbus_error_is_set(&err)) {
        brls::Logger::warning("MPRIS: request_name failed ({})", err.message);
        dbus_error_free(&err);
    }
    (void)r;

    DBusObjectPathVTable vtable;
    std::memset(&vtable, 0, sizeof(vtable));
    vtable.message_function = onMessage;
    if (!dbus_connection_register_object_path(conn, OBJECT_PATH, &vtable, nullptr)) {
        brls::Logger::warning("MPRIS: register_object_path failed");
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return;
    }

    brls::Logger::info("MPRIS: media controls active on {}", BUS_NAME);

    while (!g_quit.load()) {
        dbus_connection_read_write_dispatch(conn, 200);  // handles incoming, 200ms tick
        if (g_dirty.exchange(false))
            emitPropertiesChanged(conn);
    }

    dbus_bus_release_name(conn, BUS_NAME, nullptr);
    dbus_connection_close(conn);
    dbus_connection_unref(conn);
}

void ensureThread() {
    bool expected = false;
    if (g_threadStarted.compare_exchange_strong(expected, true)) {
        dbus_threads_init_default();
        g_thread = std::thread(threadMain);
        g_thread.detach();   // lives for the app's lifetime; never joined
    }
}

} // namespace

namespace detail {

void mprisUpdate(const Info& info) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        bool identityChanged = g_info.title != info.title ||
                               g_info.artist != info.artist ||
                               g_info.album != info.album;
        if (identityChanged || !g_active) g_trackNo++;
        g_info = info;
        g_active = true;
    }
    ensureThread();
    g_dirty.store(true);
}

void mprisClear() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_active = false;   // -> PlaybackStatus "Stopped"
    }
    // Keep the thread + bus name alive; just report Stopped. Avoids re-register
    // churn between tracks. The thread is torn down at process exit.
    if (g_threadStarted.load()) g_dirty.store(true);
}

} // namespace detail
} // namespace nowplaying
} // namespace vitaplex

#endif // VITAPLEX_MPRIS
