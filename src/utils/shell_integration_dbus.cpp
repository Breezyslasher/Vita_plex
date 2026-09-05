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

// The progress notification on screen, 0 when there is none or its id has not
// come back yet. g_pending is the in-flight Notify whose reply carries that id;
// g_progressBroken latches when the daemon will not give us one, so we stop
// posting rather than post a new popup every second.
dbus_uint32_t    g_progressId     = 0;
DBusPendingCall* g_pending        = nullptr;
bool             g_progressBroken = false;

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

    if (wantId) {
        // Asynchronous, deliberately. Blocking here was wrong twice over: it
        // stalled the UI thread on a service that may be slow to answer — on
        // Cinnamon the daemon lives inside the GJS shell process — and any
        // timeout short enough to be safe was short enough to miss the reply.
        // Missing it left the id at 0, and every later tick then posted a fresh
        // notification instead of replacing one, which is notification spam for
        // the length of the download.
        dbus_connection_send_with_reply(conn, msg, &g_pending, 5000);
        dbus_connection_flush(conn);
    } else {
        dbus_message_set_no_reply(msg, TRUE);
        dbus_connection_send(conn, msg, nullptr);
        dbus_connection_flush(conn);
    }
    dbus_message_unref(msg);
    return 0;
}

// Collect the id the daemon assigned, if the reply has landed. Called from the
// once-a-second progress tick, so the pending call is polled rather than
// waited on and nothing has to integrate this private connection with a main
// loop.
void pumpPendingId() {
    if (!g_pending) return;
    DBusConnection* conn = bus();
    // read_write_dispatch dispatches one message per call, and the bus puts its
    // own NameAcquired signal in the queue ahead of our reply — so a single
    // call collected nothing and the id took three ticks to surface even from
    // an instant daemon, losing the first seconds of every download. Read once,
    // bounded, then drain the queue until the reply turns up or it empties.
    if (conn) {
        dbus_connection_read_write(conn, 25);
        while (!dbus_pending_call_get_completed(g_pending) &&
               dbus_connection_dispatch(conn) == DBUS_DISPATCH_DATA_REMAINS) {
        }
    }
    if (!dbus_pending_call_get_completed(g_pending)) return;

    if (DBusMessage* reply = dbus_pending_call_steal_reply(g_pending)) {
        dbus_uint32_t id = 0;
        DBusError err;
        dbus_error_init(&err);
        if (dbus_message_get_args(reply, &err, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID) &&
            id != 0) {
            g_progressId = id;
            brls::Logger::debug("shell: progress notification id {}", id);
        } else {
            // An error reply, or a daemon that answered with nothing usable.
            // Give up on replacing rather than spam: the launcher bar carries on.
            g_progressBroken = true;
            brls::Logger::debug("shell: no usable notification id ({}) — progress "
                                "notification disabled for this run",
                                dbus_error_is_set(&err) ? err.message : "no id in reply");
        }
        if (dbus_error_is_set(&err)) dbus_error_free(&err);
        dbus_message_unref(reply);
    }
    dbus_pending_call_unref(g_pending);
    g_pending = nullptr;
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
        pumpPendingId();               // so a late id can still be closed
        closeNotification(g_progressId);
        if (g_pending) { dbus_pending_call_cancel(g_pending); dbus_pending_call_unref(g_pending); }
        g_pending        = nullptr;
        g_progressId     = 0;
        g_progressBroken = false;      // a new run gets a fresh try
        return;
    }

    // Has the id from the opening Notify arrived since the last tick?
    pumpPendingId();
    if (g_progressBroken) return;      // launcher bar only; never spam popups

    // -1 while the size is unknown, so a daemon that draws the hint shows an
    // empty bar rather than a misleading 0%.
    const int pct = fraction < 0.0 ? -1
                                   : (int)((fraction > 1.0 ? 1.0 : fraction) * 100.0 + 0.5);
    const std::string summary = title.empty() ? std::string("Downloading") : title;

    // Exactly one notification is ever posted without a replaces id: the first.
    // Until its reply names the id, later ticks are dropped rather than posted,
    // because posting them would each create a popup of their own. That costs a
    // second or two of staleness at the start of a download and is the whole
    // difference between one notification and sixty.
    if (g_progressId == 0) {
        if (g_pending) return;         // opening call still in flight
        // timeout 0 means "until dismissed": a progress popup that expires after
        // a few seconds and returns a second later is worse than none.
        postNotification(summary, detail, 0, pct, /*timeoutMs=*/0,
                         /*lowUrgency=*/true, /*wantId=*/true);
        return;
    }

    postNotification(summary, detail, g_progressId, pct, /*timeoutMs=*/0,
                     /*lowUrgency=*/true, /*wantId=*/false);
}
} // namespace shell
} // namespace vitaplex

#endif // VITAPLEX_MPRIS
