package org.VitaPlex.app;

import android.app.ActivityManager;
import android.app.usage.UsageStatsManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.os.Build;
import android.os.PowerManager;
import android.util.Log;

/**
 * What Android does to VitaPlex's network, written into VitaPlex's own log.
 *
 * A device log showed music stopping twice, both times with every request to
 * the server timing out and then failing to resolve its name, starting seconds
 * after the app went to the background. That log could not say whether the
 * phone had lost its connection or Android had cut VitaPlex off in the
 * background: VitaPlex logged only its own requests failing. Android reports
 * both, so this writes them down beside those requests:
 *
 *   - the default network arriving, going, and changing: Wi-Fi or mobile,
 *     whether it has internet, whether Android has validated it;
 *   - Android blocking or unblocking this app's network access (Data Saver,
 *     battery restrictions, Doze), from Android 10;
 *   - Doze, battery saver and Data Saver switching, and the screen going off
 *     and on, which also tells leaving the app apart from the screen going off;
 *   - at start, the restrictions already in place for the app.
 *
 * Each line goes through nativeLog() to the native logger, so it lands in
 * vitaplex.log. Native calls start() once that log file is open, so the first
 * report of the network is in it too.
 */
public final class NetworkMonitor {
    private static final String TAG = "VitaPlexNet";

    // Implemented in src/platform/platform_android.cpp.
    private static native void nativeLog(String line);

    private static boolean sStarted;
    // onCapabilitiesChanged fires for signal strength and other details too;
    // only a change in what describe() reports is worth a line.
    private static String sLastCaps = "";

    private NetworkMonitor() {}

    public static synchronized void start() {
        if (sStarted) return;
        Context ctx = VitaPlexActivity.getAppContext();
        if (ctx == null) return;
        sStarted = true;
        try {
            logRestrictions(ctx);
            watchNetwork(ctx);
            watchPower(ctx);
        } catch (Throwable t) {
            Log.w(TAG, "start failed", t);
            log("network monitor failed to start: " + t);
        }
    }

    private static void log(String line) {
        Log.i(TAG, line);
        try {
            nativeLog(line);
        } catch (Throwable ignore) {
            // Native side not loaded; logcat has it.
        }
    }

    private static void logRestrictions(Context ctx) {
        StringBuilder sb = new StringBuilder("app restrictions:");
        PowerManager pm = (PowerManager) ctx.getSystemService(Context.POWER_SERVICE);
        if (pm != null) {
            sb.append(" screen ").append(pm.isInteractive() ? "on" : "off");
            sb.append(", battery saver ").append(pm.isPowerSaveMode() ? "on" : "off");
            if (Build.VERSION.SDK_INT >= 23) {
                sb.append(", Doze ").append(pm.isDeviceIdleMode() ? "on" : "off");
                sb.append(", battery optimization ")
                  .append(pm.isIgnoringBatteryOptimizations(ctx.getPackageName())
                          ? "off for VitaPlex" : "on for VitaPlex");
            }
        }
        if (Build.VERSION.SDK_INT >= 28) {
            ActivityManager am = (ActivityManager) ctx.getSystemService(Context.ACTIVITY_SERVICE);
            if (am != null) {
                sb.append(", background ")
                  .append(am.isBackgroundRestricted() ? "RESTRICTED" : "not restricted");
            }
            UsageStatsManager us = (UsageStatsManager) ctx.getSystemService(Context.USAGE_STATS_SERVICE);
            if (us != null) sb.append(", standby bucket ").append(us.getAppStandbyBucket());
        }
        ConnectivityManager cm = (ConnectivityManager) ctx.getSystemService(Context.CONNECTIVITY_SERVICE);
        if (cm != null && Build.VERSION.SDK_INT >= 24) {
            sb.append(", Data Saver ").append(dataSaver(cm.getRestrictBackgroundStatus()));
        }
        log(sb.toString());
    }

    private static String dataSaver(int status) {
        switch (status) {
            case ConnectivityManager.RESTRICT_BACKGROUND_STATUS_DISABLED:
                return "off";
            case ConnectivityManager.RESTRICT_BACKGROUND_STATUS_WHITELISTED:
                return "on, VitaPlex exempt";
            case ConnectivityManager.RESTRICT_BACKGROUND_STATUS_ENABLED:
                return "on, VitaPlex restricted";
            default:
                return "unknown (" + status + ")";
        }
    }

    private static void watchNetwork(Context ctx) {
        ConnectivityManager cm = (ConnectivityManager) ctx.getSystemService(Context.CONNECTIVITY_SERVICE);
        if (cm == null) return;
        if (Build.VERSION.SDK_INT >= 23 && cm.getActiveNetwork() == null) {
            log("no network at start");
        }

        ConnectivityManager.NetworkCallback cb = new ConnectivityManager.NetworkCallback() {
            @Override public void onAvailable(Network n) {
                log("network " + n + " available");
            }

            @Override public void onLosing(Network n, int maxMsToLive) {
                log("network " + n + " about to be lost (" + maxMsToLive + " ms)");
            }

            @Override public void onLost(Network n) {
                synchronized (NetworkMonitor.class) { sLastCaps = ""; }
                log("network " + n + " LOST");
            }

            @Override public void onCapabilitiesChanged(Network n, NetworkCapabilities nc) {
                String s = "network " + n + ": " + describe(nc);
                synchronized (NetworkMonitor.class) {
                    if (s.equals(sLastCaps)) return;
                    sLastCaps = s;
                }
                log(s);
            }

            // Android 10+. This is the line that says Android itself cut
            // VitaPlex off, as opposed to the network going away.
            @Override public void onBlockedStatusChanged(Network n, boolean blocked) {
                log("network " + n + ": access " + (blocked ? "BLOCKED for VitaPlex"
                                                            : "allowed for VitaPlex"));
            }
        };

        // The default network is the one VitaPlex's requests use. Before
        // Android 7 there is no callback for it; any network with internet is
        // the next best thing.
        if (Build.VERSION.SDK_INT >= 24) {
            cm.registerDefaultNetworkCallback(cb);
        } else {
            cm.registerNetworkCallback(new NetworkRequest.Builder()
                .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .build(), cb);
        }
    }

    private static String describe(NetworkCapabilities nc) {
        StringBuilder sb = new StringBuilder();
        if (nc.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) sb.append("Wi-Fi");
        if (nc.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)) sb.append(sb.length() > 0 ? "+mobile" : "mobile");
        if (nc.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)) sb.append(sb.length() > 0 ? "+ethernet" : "ethernet");
        if (nc.hasTransport(NetworkCapabilities.TRANSPORT_VPN)) sb.append(sb.length() > 0 ? "+VPN" : "VPN");
        if (sb.length() == 0) sb.append("other");
        sb.append(nc.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                  ? ", internet" : ", NO internet");
        if (Build.VERSION.SDK_INT >= 23) {
            sb.append(nc.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED)
                      ? ", validated" : ", NOT validated");
        }
        if (Build.VERSION.SDK_INT >= 28 &&
            !nc.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_SUSPENDED)) {
            sb.append(", SUSPENDED");
        }
        if (!nc.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED)) sb.append(", metered");
        return sb.toString();
    }

    private static void watchPower(final Context ctx) {
        IntentFilter f = new IntentFilter();
        f.addAction(Intent.ACTION_SCREEN_ON);
        f.addAction(Intent.ACTION_SCREEN_OFF);
        f.addAction(PowerManager.ACTION_POWER_SAVE_MODE_CHANGED);
        if (Build.VERSION.SDK_INT >= 23) f.addAction(PowerManager.ACTION_DEVICE_IDLE_MODE_CHANGED);
        if (Build.VERSION.SDK_INT >= 24) f.addAction(ConnectivityManager.ACTION_RESTRICT_BACKGROUND_CHANGED);

        BroadcastReceiver r = new BroadcastReceiver() {
            @Override public void onReceive(Context c, Intent i) {
                String a = i.getAction();
                if (a == null) return;
                PowerManager pm = (PowerManager) ctx.getSystemService(Context.POWER_SERVICE);
                if (Intent.ACTION_SCREEN_ON.equals(a)) {
                    log("screen on");
                } else if (Intent.ACTION_SCREEN_OFF.equals(a)) {
                    log("screen off");
                } else if (PowerManager.ACTION_POWER_SAVE_MODE_CHANGED.equals(a)) {
                    log("battery saver " + (pm != null && pm.isPowerSaveMode() ? "on" : "off"));
                } else if (Build.VERSION.SDK_INT >= 23 &&
                           PowerManager.ACTION_DEVICE_IDLE_MODE_CHANGED.equals(a)) {
                    log("Doze " + (pm != null && pm.isDeviceIdleMode() ? "on" : "off"));
                } else if (Build.VERSION.SDK_INT >= 24 &&
                           ConnectivityManager.ACTION_RESTRICT_BACKGROUND_CHANGED.equals(a)) {
                    ConnectivityManager cm =
                        (ConnectivityManager) ctx.getSystemService(Context.CONNECTIVITY_SERVICE);
                    if (cm != null) log("Data Saver " + dataSaver(cm.getRestrictBackgroundStatus()));
                }
            }
        };
        // Screen on/off reach only receivers registered at runtime. These are
        // all system broadcasts, which arrive even when not exported.
        if (Build.VERSION.SDK_INT >= 33) {
            ctx.registerReceiver(r, f, Context.RECEIVER_NOT_EXPORTED);
        } else {
            ctx.registerReceiver(r, f);
        }
    }
}
