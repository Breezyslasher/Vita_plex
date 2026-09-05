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
 * Foreground service that keeps offline downloads running while the app is
 * backgrounded or the screen is off.
 *
 * Downloads run on a plain thread inside the app process. Without a foreground
 * service Android is free to reclaim that process once the app leaves the
 * screen, and a long album download would simply stop part-way — the user
 * finding out only on coming back. Holding a foreground slot is what tells the
 * system the work is user-visible and must be left alone.
 *
 * The service owns nothing: DownloadNotification builds the progress
 * notification, and this foregrounds that same notification, so there is one
 * notification rather than two competing for the shade.
 *
 * It also holds two locks for the duration, because a foreground service on its
 * own does not stop the CPU sleeping or Wi-Fi powering down:
 *
 *   PARTIAL_WAKE_LOCK   keeps the CPU up with the screen off.
 *   WIFI_MODE_FULL_HIGH_PERF  keeps the radio from dropping to a power-saving
 *                             mode that stalls a long transfer.
 *
 * Both are released in onDestroy, so they last exactly as long as the queue.
 *
 * Lifecycle: DownloadNotification.setProgress(_, true) starts it when a
 * download reports progress; setProgress(_, false) stops it when the queue
 * drains. Started, never bound.
 */
public final class DownloadService extends Service {
    private static final String TAG = "VitaPlexDownloadSvc";

    private PowerManager.WakeLock  mWakeLock;
    private WifiManager.WifiLock   mWifiLock;

    @Override
    public IBinder onBind(Intent intent) {
        return null;  // started service, not bound
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        try {
            Notification n = DownloadNotification.buildProgress(this);
            if (n == null) {
                // Nothing downloading — don't hold a foreground slot.
                stopSelf();
                return START_NOT_STICKY;
            }
            // startForegroundService() requires startForeground() within ~5s.
            // From API 34 the type must be given here as well as in the
            // manifest, and must match.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                startForeground(DownloadNotification.PROGRESS_ID, n,
                                ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
            } else {
                startForeground(DownloadNotification.PROGRESS_ID, n);
            }
            acquireLocks();
        } catch (Throwable t) {
            // Android 12+ refuses a foreground service started from the
            // background. Downloads begin from a tap, so this is the unusual
            // case; take the loss rather than the crash — the download carries
            // on for as long as the process happens to live.
            Log.w(TAG, "startForeground failed", t);
            stopSelf();
        }
        // The queue lives in the app process; restarting this alone would
        // foreground a service with nothing behind it.
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        releaseLocks();
        super.onDestroy();
    }

    private void acquireLocks() {
        try {
            if (mWakeLock == null) {
                PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
                if (pm != null) {
                    mWakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK,
                                               "VitaPlex:downloads");
                    mWakeLock.setReferenceCounted(false);
                }
            }
            if (mWakeLock != null && !mWakeLock.isHeld()) mWakeLock.acquire();

            if (mWifiLock == null) {
                WifiManager wm = (WifiManager)
                    getApplicationContext().getSystemService(Context.WIFI_SERVICE);
                if (wm != null) {
                    mWifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF,
                                                  "VitaPlex:downloads");
                    mWifiLock.setReferenceCounted(false);
                }
            }
            if (mWifiLock != null && !mWifiLock.isHeld()) mWifiLock.acquire();
        } catch (Throwable t) {
            Log.w(TAG, "acquireLocks", t);
        }
    }

    private void releaseLocks() {
        try {
            if (mWakeLock != null && mWakeLock.isHeld()) mWakeLock.release();
        } catch (Throwable t) {
            Log.w(TAG, "release wake", t);
        }
        try {
            if (mWifiLock != null && mWifiLock.isHeld()) mWifiLock.release();
        } catch (Throwable t) {
            Log.w(TAG, "release wifi", t);
        }
    }
}
