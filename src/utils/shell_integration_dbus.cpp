/**
 * VitaPlex - shell integration, D-Bus backend (see the header).
 *
 * Compiled only when VITAPLEX_MPRIS is defined — the same dbus-1 check that
 * gates the media controls, since both want a session bus.
 */

#include "utils/shell_integration.hpp"

#if defined(VITAPLEX_MPRIS)

#include <borealis.hpp>
#include <dbus/dbus.h>

#include <cstring>

namespace vitaplex {
namespace shell {

namespace {

// The desktop entry this app ships as. The launcher protocol keys everything on
// it, and the notification hint below is what gives the popup our icon.
constexpr const char* DESKTOP_ID    = "io.github.breezyslasher.VitaPlex.desktop";
constexpr const char* APP_ICON_NAME = "io.github.breezyslasher.VitaPlex";

// One connection for both, opened on first use.
//
// Private rather than shared: a shared connection is process-global and
// dispatched by whoever else holds it, and MPRIS already runs its own private
// connection on this bus. Two owners of one shared handle would fight over
// dispatch. Never closed — the process outlives it, and tearing a bus down at
// exit buys nothing.
DBusConnection* bus() {
    static DBusConnection* conn = []() -> DBusConnection* {
        DBusError err;
        dbus_error_init(&err);
        DBusConnection* c = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
        if (dbus_error_is_set(&err)) {
            brls::Logger::debug("shell: no session bus ({})", err.message);
            dbus_error_free(&err);
            return nullptr;
        }
        if (c) {
            // Losing the bus must not take the app with it.
            dbus_connection_set_exit_on_disconnect(c, FALSE);
        }
        return c;
    }();
    return conn;
}

// One Notify call. replacesId 0 posts a new notification; a previous id
// updates that one in place, which is what makes a progress notification a
// single popup that changes rather than one per second.
//
// valuePct >= 0 adds the "value" hint. It is not in the spec, but it is the
// long-standing convention every notification daemon that draws a progress bar
// reads (KDE Plasma, Xfce, dunst); one that does not simply ignores it and
// shows the text.
//
// Returns the notification id, or 0. Only the first call of a run needs it, so
// wantId is false for the updates and they are sent without waiting for a reply.
dbus_uint32_t postNotification(const std::string& summary, const std::string& body,
                               dbus_uint32_t replacesId, int valuePct,
                               dbus_int32_t timeoutMs, bool lowUrgency, bool wantId) {
    DBusConnection* conn = bus();
    if (!conn) return 0;

    DBusMessage* msg = dbus_message_new_method_call(
        "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "Notify");
    if (!msg) return 0;

    const char* appName  = "VitaPlex";
    const char* icon     = APP_ICON_NAME;
    const char* summaryC = summary.c_str();
    const char* bodyC    = body.c_str();

    DBusMessageIter it;
    dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &appName);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &replacesId);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &icon);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &summaryC);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &bodyC);

    // actions: empty as{} — we offer no buttons.
    DBusMessageIter actions;
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &actions);
    dbus_message_iter_close_container(&it, &actions);

    DBusMessageIter hints;
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &hints);
    auto hint = [&hints](const char* key, int type, const char* sig, const void* val) {
        DBusMessageIter entry, variant;
        dbus_message_iter_open_container(&hints, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, sig, &variant);
        dbus_message_iter_append_basic(&variant, type, val);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&hints, &entry);
    };
    // desktop-entry lets the shell attribute the popup to us, which is what puts
    // our icon on it and groups it under the app.
    const char* desktopId = DESKTOP_ID;
    hint("desktop-entry", DBUS_TYPE_STRING, "s", &desktopId);
    if (valuePct >= 0) {
        dbus_int32_t v = valuePct;
        hint("value", DBUS_TYPE_INT32, "i", &v);
    }
    if (lowUrgency) {
        // A download reporting itself must never interrupt what the user is doing.
        unsigned char urgency = 0;
        hint("urgency", DBUS_TYPE_BYTE, "y", &urgency);
    }
    dbus_message_iter_close_container(&it, &hints);

    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &timeoutMs);

    dbus_uint32_t id = 0;
    if (wantId) {
        // Blocking, but only for the first post of a run, and with a short
        // timeout so a wedged daemon cannot stall the UI thread.
        DBusError err;
        dbus_error_init(&err);
        DBusMessage* reply =
            dbus_connection_send_with_reply_and_block(conn, msg, 300, &err);
        if (reply) {
            dbus_message_get_args(reply, nullptr, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID);
            dbus_message_unref(reply);
        } else if (dbus_error_is_set(&err)) {
            brls::Logger::debug("shell: Notify failed ({})", err.message);
            dbus_error_free(&err);
        }
    } else {
        dbus_message_set_no_reply(msg, TRUE);
        dbus_connection_send(conn, msg, nullptr);
        dbus_connection_flush(conn);
    }
    dbus_message_unref(msg);
    return id;
}

void closeNotification(dbus_uint32_t id) {
    DBusConnection* conn = bus();
    if (!conn || id == 0) return;
    DBusMessage* msg = dbus_message_new_method_call(
        "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "CloseNotification");
    if (!msg) return;
    DBusMessageIter it;
    dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &id);
    dbus_message_set_no_reply(msg, TRUE);
    dbus_connection_send(conn, msg, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(msg);
}

// The progress notification currently on screen, 0 when there is none.
dbus_uint32_t g_progressId = 0;

} // namespace

// Nothing to declare to this shell up front — the bus connection opens lazily
// on first use, and D-Bus has no equivalent of an AppUserModelID.
void init() {}

void notify(const std::string& summary, const std::string& body) {
    // A completion notice is its own popup, not an update of the progress one:
    // the progress notification is closed by then, and replacing it would make
    // "done" inherit the progress bar.
    postNotification(summary, body, /*replaces=*/0, /*valuePct=*/-1,
                     /*timeoutMs=*/-1, /*lowUrgency=*/false, /*wantId=*/false);
}

namespace {

// com.canonical.Unity.LauncherEntry.Update(s appuri, a{sv} properties), emitted
// as a signal on /. Every property is optional; senders include only what
// changed, and a shell that does not implement the interface ignores it.
void emitLauncherUpdate(const char* key1, int type1, const void* val1, const char* sig1,
                        const char* key2, const void* val2) {
    DBusConnection* conn = bus();
    if (!conn) return;

    DBusMessage* sig = dbus_message_new_signal(
        "/", "com.canonical.Unity.LauncherEntry", "Update");
    if (!sig) return;

    const std::string appUri = std::string("application://") + DESKTOP_ID;
    const char* appUriC = appUri.c_str();

    DBusMessageIter it;
    dbus_message_iter_init_append(sig, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &appUriC);

    DBusMessageIter props;
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &props);

    auto addProp = [&props](const char* key, int type, const char* sig, const void* value) {
        DBusMessageIter entry, variant;
        dbus_message_iter_open_container(&props, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, sig, &variant);
        dbus_message_iter_append_basic(&variant, type, value);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&props, &entry);
    };

    addProp(key1, type1, sig1, val1);
    dbus_bool_t visible = *static_cast<const dbus_bool_t*>(val2);
    addProp(key2, DBUS_TYPE_BOOLEAN, "b", &visible);

    dbus_message_iter_close_container(&it, &props);

    dbus_connection_send(conn, sig, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(sig);
}

} // namespace

void setProgress(double fraction, const std::string& title,
                 const std::string& detail, bool visible) {
    // Two ways of saying it, because no desktop implements both.
    //
    // The launcher bar is the nicer one where it exists — KDE Plasma and the
    // Ubuntu dock — but it is a Unity-era protocol and Cinnamon, stock GNOME
    // and most tiling setups never see the signal. On those the notification
    // below is the only thing the user gets, which is why it carries the text
    // rather than leaving the bar to speak for itself.
    {
        double clamped = fraction < 0.0 ? 0.0 : (fraction > 1.0 ? 1.0 : fraction);
        dbus_bool_t vis = visible ? TRUE : FALSE;
        emitLauncherUpdate("progress", DBUS_TYPE_DOUBLE, &clamped, "d",
                           "progress-visible", &vis);
    }

    if (!visible) {
        closeNotification(g_progressId);
        g_progressId = 0;
        return;
    }

    // -1 while the size is unknown, so a daemon that draws the hint shows an
    // empty bar rather than a misleading 0%.
    const int pct = fraction < 0.0 ? -1
                                   : (int)((fraction > 1.0 ? 1.0 : fraction) * 100.0 + 0.5);
    const std::string summary = title.empty() ? std::string("Downloading") : title;

    // timeout 0 means "until dismissed": a progress popup that expires after a
    // few seconds and returns a second later is worse than none. The first post
    // learns the id; every later one replaces that same notification, so this
    // stays one popup that updates rather than one per second.
    const dbus_uint32_t id = postNotification(summary, detail, g_progressId, pct,
                                              /*timeoutMs=*/0, /*lowUrgency=*/true,
                                              /*wantId=*/g_progressId == 0);
    if (g_progressId == 0 && id != 0) g_progressId = id;
}
} // namespace shell
} // namespace vitaplex

#endif // VITAPLEX_MPRIS
