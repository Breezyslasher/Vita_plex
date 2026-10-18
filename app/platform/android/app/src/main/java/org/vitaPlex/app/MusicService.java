package org.VitaPlex.app;

import android.app.Notification;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

import android.support.v4.media.session.MediaSessionCompat;
import androidx.media.session.MediaButtonReceiver;

/**
 * Foreground service that keeps music playback (and its media notification)
 * alive while the app is backgrounded or the screen is off. It owns nothing —
 * MediaNotification builds the notification + MediaSession; this service just
 * foregrounds that same notification so Android won't reclaim the process.
 * Running in the foreground is also what keeps a backgrounded VitaPlex on the
 * network: without it Android blocks the app's access seconds after it leaves
 * the screen, and the next track cannot load.
 *
 * The CPU + Wi-Fi locks that keep a screen-off stream from stalling are managed
 * by MediaNotification, tied to play/pause state so they're not held (draining
 * battery) while paused in the background.
 *
 * Lifecycle: MediaNotification.ensureService() starts it (from the foreground,
 * when music begins, and again each time playback starts) and clear() stops it
 * when playback ends. Each step is written to vitaplex.log.
 */
public final class MusicService extends Service {
    private static final String TAG = "VitaPlexMusicSvc";

    // Whether the service is up and has called startForeground(), as the
    // service itself last saw it. MediaNotification used to keep a flag of its
    // own, set when it asked for a start; that only recorded the request, so it
    // could not tell when the service had stopped.
    private static volatile boolean sForeground;

    static boolean isForeground() {
        return sForeground;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;  // started service, not bound
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        // Headset, Bluetooth and keyboard media keys arrive as ACTION_MEDIA_BUTTON,
        // forwarded here by the manifest's MediaButtonReceiver. The session decodes
        // the key event into the same transport callbacks the notification buttons
        // use, so this is the whole of the handling.
        if (intent != null && Intent.ACTION_MEDIA_BUTTON.equals(intent.getAction())) {
            try {
                MediaSessionCompat session = MediaNotification.getSession(this);
                if (session != null) MediaButtonReceiver.handleIntent(session, intent);
            } catch (Throwable t) {
                Log.w(TAG, "media button", t);
            }
        }
        try {
            Notification n = MediaNotification.buildNotification(this);
            if (n == null) {
                // Nothing playing yet — don't hold a foreground slot.
                NetworkMonitor.log("music service: nothing to show, stopping");
                sForeground = false;
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
            sForeground = true;
            NetworkMonitor.log("music service: in the foreground");
        } catch (Throwable t) {
            Log.w(TAG, "startForeground failed", t);
            NetworkMonitor.log("music service: could not enter the foreground: " + t);
            sForeground = false;
            stopSelf();
        }
        // Don't auto-restart if the system kills us; playback is already gone.
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        sForeground = false;
        NetworkMonitor.log("music service: stopped");
        super.onDestroy();
    }
}
