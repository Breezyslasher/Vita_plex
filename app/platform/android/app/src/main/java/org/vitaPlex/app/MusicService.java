package org.VitaPlex.app;

import android.app.Notification;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

/**
 * Foreground service that keeps music playback (and its media notification)
 * alive while the app is backgrounded or the screen is off. It owns nothing —
 * MediaNotification builds the notification + MediaSession; this service just
 * foregrounds that same notification so Android won't reclaim the process.
 *
 * Lifecycle: MediaNotification.ensureService() starts it (from the foreground,
 * when music begins) and clear() stops it when playback ends.
 */
public final class MusicService extends Service {
    private static final String TAG = "VitaPlexMusicSvc";

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
        } catch (Throwable t) {
            Log.w(TAG, "startForeground failed", t);
            stopSelf();
        }
        // Don't auto-restart if the system kills us; playback is already gone.
        return START_NOT_STICKY;
    }
}
