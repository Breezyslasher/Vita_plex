package org.VitaPlex.app;

import android.app.Notification;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.util.Log;

/**
 * Foreground service that keeps music playback (and its media notification)
 * alive while the app is backgrounded or the screen is off. It owns nothing —
 * MediaNotification builds the notification + MediaSession; this service just
 * foregrounds that same notification so Android won't reclaim the process.
 *
 * It also holds a partial wake lock + a high-perf Wi-Fi lock for as long as it
 * is foregrounded. Without them, a screen-off device suspends the CPU and parks
 * the Wi-Fi radio after a few minutes, the network transcode stream underruns,
 * and playback stops. The foreground service alone does not prevent that.
 *
 * Lifecycle: MediaNotification.ensureService() starts it (from the foreground,
 * when music begins) and clear() stops it when playback ends.
 */
public final class MusicService extends Service {
    private static final String TAG = "VitaPlexMusicSvc";

    private PowerManager.WakeLock wakeLock;
    private WifiManager.WifiLock wifiLock;

    @Override
    public IBinder onBind(Intent intent) {
        return null;  // started service, not bound
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        try {
            Notification n = MediaNotification.buildNotification(this);
            if (n == null) {
                // Nothing playing yet — don't hold a foreground slot.
                stopSelf();
                return START_NOT_STICKY;
            }
            // startForegroundService() requires startForeground() within ~5s.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                startForeground(MediaNotification.NOTIFICATION_ID, n,
                                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
            } else {
                startForeground(MediaNotification.NOTIFICATION_ID, n);
            }
            acquireLocks();
        } catch (Throwable t) {
            Log.w(TAG, "startForeground failed", t);
            stopSelf();
        }
        // Don't auto-restart if the system kills us; playback is already gone.
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        releaseLocks();
        super.onDestroy();
    }

    // Keep the CPU running and the Wi-Fi radio at full power while we're the
    // foreground media service. Held for the whole playback session (across
    // pauses) and released when MediaNotification.clear() stops the service.
    private void acquireLocks() {
        try {
            if (wakeLock == null) {
                PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
                if (pm != null) {
                    wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "VitaPlex:music");
                    wakeLock.setReferenceCounted(false);
                }
            }
            if (wakeLock != null && !wakeLock.isHeld()) wakeLock.acquire();

            if (wifiLock == null) {
                WifiManager wm = (WifiManager)
                    getApplicationContext().getSystemService(Context.WIFI_SERVICE);
                if (wm != null) {
                    wifiLock = wm.createWifiLock(
                        WifiManager.WIFI_MODE_FULL_HIGH_PERF, "VitaPlex:music");
                    wifiLock.setReferenceCounted(false);
                }
            }
            if (wifiLock != null && !wifiLock.isHeld()) wifiLock.acquire();
        } catch (Throwable t) {
            Log.w(TAG, "acquireLocks failed", t);
        }
    }

    private void releaseLocks() {
        try {
            if (wifiLock != null && wifiLock.isHeld()) wifiLock.release();
            if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
        } catch (Throwable t) {
            Log.w(TAG, "releaseLocks failed", t);
        }
    }
}
