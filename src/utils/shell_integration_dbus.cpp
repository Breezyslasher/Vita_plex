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

} // namespace

// Nothing to declare to this shell up front — the bus connection opens
// lazily on first use, and D-Bus has no equivalent of an AppUserModelID.
void init() {}

void notify(const std::string& summary, const std::string& body) {
    DBusConnection* conn = bus();
    if (!conn) return;

    DBusMessage* msg = dbus_message_new_method_call(
        "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "Notify");
    if (!msg) return;

    const char* appName    = "VitaPlex";
    const char* icon       = APP_ICON_NAME;
    const char* summaryC   = summary.c_str();
    const char* bodyC      = body.c_str();
    dbus_uint32_t replaces = 0;
    dbus_int32_t  timeout  = -1;   // let the daemon decide

    DBusMessageIter it;
    dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &appName);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &replaces);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &icon);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &summaryC);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &bodyC);

    // actions: empty as{} — we offer no buttons.
    DBusMessageIter actions;
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &actions);
    dbus_message_iter_close_container(&it, &actions);

    // hints: a{sv}. desktop-entry lets the shell attribute the popup to us,
    // which is what puts our icon on it and groups it under the app.
    DBusMessageIter hints;
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &hints);
    {
        DBusMessageIter entry, variant;
        const char* key = "desktop-entry";
        const char* val = DESKTOP_ID;
        dbus_message_iter_open_container(&hints, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &val);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&hints, &entry);
    }
    dbus_message_iter_close_container(&it, &hints);

    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &timeout);

    // No reply wanted: the returned notification id is only useful for replacing or closing one, and we do neither.
    dbus_message_set_no_reply(msg, TRUE);
    dbus_connection_send(conn, msg, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(msg);
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

// The launcher protocol carries a progress fraction and nothing else, so the
// title and detail have nowhere to go here.
void setProgress(double fraction, const std::string&, const std::string&, bool visible) {
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;
    dbus_bool_t vis = visible ? TRUE : FALSE;
    emitLauncherUpdate("progress", DBUS_TYPE_DOUBLE, &fraction, "d",
                       "progress-visible", &vis);
}
} // namespace shell
} // namespace vitaplex

#endif // VITAPLEX_MPRIS
