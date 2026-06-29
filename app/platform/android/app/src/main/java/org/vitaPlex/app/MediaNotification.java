package org.VitaPlex.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * Framework MediaSession + MediaStyle notification for music playback.
 *
 * Driven from native (MusicController) through update()/clear(); transport
 * buttons (lock screen, notification shade, headset) route back to native via
 * nativeMediaAction() / nativeMediaSeek(). No AndroidX — minSdk-21 framework
 * classes only. Everything runs on the main looper; art is fetched off-thread.
 */
public final class MediaNotification {
    private static final String TAG = "VitaPlexMedia";
    private static final String CHANNEL_ID = "vitaplex_music";
    private static final int NOTIFICATION_ID = 0x7654;

    // Transport codes shared with src/utils/now_playing.cpp (keep in sync).
    private static final int CODE_TOGGLE = 1;
    private static final int CODE_PLAY = 2;
    private static final int CODE_PAUSE = 3;
    private static final int CODE_NEXT = 4;
    private static final int CODE_PREVIOUS = 5;
    private static final int CODE_STOP = 6;

    private static final String ACTION = "org.VitaPlex.app.MEDIA_ACTION";
    private static final String EXTRA_CODE = "code";

    private static native void nativeMediaAction(int code);
    private static native void nativeMediaSeek(long positionMs);

    private static final Handler sMain = new Handler(Looper.getMainLooper());

    private static MediaSession sSession;
    private static boolean sChannelCreated;
    private static BroadcastReceiver sReceiver;

    // Last-known state, so an async art load can re-post without re-plumbing.
    private static String sTitle = "", sArtist = "", sAlbum = "", sArtUrl = "";
    private static long sDurationMs, sPositionMs;
    private static boolean sPlaying, sHasNext, sHasPrev;
    private static String sLoadedArtUrl;   // url whose bitmap is in sArtBitmap
    private static Bitmap sArtBitmap;

    private MediaNotification() {}

    /** Called from native (any thread). Marshals to the main looper. */
    public static void update(final String title, final String artist, final String album,
                              final String artUrl, final long durationMs, final long positionMs,
                              final boolean playing, final boolean hasNext, final boolean hasPrev) {
        sMain.post(new Runnable() {
            @Override public void run() {
                sTitle = title != null ? title : "";
                sArtist = artist != null ? artist : "";
                sAlbum = album != null ? album : "";
                sArtUrl = artUrl != null ? artUrl : "";
                sDurationMs = durationMs;
                sPositionMs = positionMs;
                sPlaying = playing;
                sHasNext = hasNext;
                sHasPrev = hasPrev;
                // Drop a stale cover the instant the track changes.
                if (!sArtUrl.equals(sLoadedArtUrl)) sArtBitmap = null;
                try { applyUpdate(); } catch (Throwable t) { Log.w(TAG, "update failed", t); }
                maybeLoadArt(sArtUrl);
            }
        });
    }

    /** Called from native (any thread). Tears down the session + notification. */
    public static void clear() {
        sMain.post(new Runnable() {
            @Override public void run() {
                try {
                    Context ctx = VitaPlexActivity.getAppContext();
                    if (ctx != null) {
                        NotificationManager nm = (NotificationManager)
                            ctx.getSystemService(Context.NOTIFICATION_SERVICE);
                        if (nm != null) nm.cancel(NOTIFICATION_ID);
                        if (sReceiver != null) {
                            try { ctx.unregisterReceiver(sReceiver); } catch (Throwable ignore) {}
                        }
                    }
                    sReceiver = null;
                    if (sSession != null) {
                        sSession.setActive(false);
                        sSession.release();
                        sSession = null;
                    }
                    sArtBitmap = null;
                    sLoadedArtUrl = null;
                } catch (Throwable t) {
                    Log.w(TAG, "clear failed", t);
                }
            }
        });
    }

    private static void applyUpdate() {
        Context ctx = VitaPlexActivity.getAppContext();
        if (ctx == null) return;

        ensureSession(ctx);
        ensureChannel(ctx);
        ensureReceiver(ctx);

        MediaMetadata.Builder meta = new MediaMetadata.Builder()
            .putString(MediaMetadata.METADATA_KEY_TITLE, sTitle)
            .putString(MediaMetadata.METADATA_KEY_ARTIST, sArtist)
            .putString(MediaMetadata.METADATA_KEY_ALBUM, sAlbum)
            .putLong(MediaMetadata.METADATA_KEY_DURATION, sDurationMs);
        if (sArtBitmap != null) {
            meta.putBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART, sArtBitmap);
        }
        sSession.setMetadata(meta.build());

        long actions = PlaybackState.ACTION_PLAY_PAUSE | PlaybackState.ACTION_PLAY
            | PlaybackState.ACTION_PAUSE | PlaybackState.ACTION_SEEK_TO | PlaybackState.ACTION_STOP;
        if (sHasNext) actions |= PlaybackState.ACTION_SKIP_TO_NEXT;
        if (sHasPrev) actions |= PlaybackState.ACTION_SKIP_TO_PREVIOUS;
        PlaybackState state = new PlaybackState.Builder()
            .setActions(actions)
            .setState(sPlaying ? PlaybackState.STATE_PLAYING : PlaybackState.STATE_PAUSED,
                      sPositionMs, 1.0f, SystemClock.elapsedRealtime())
            .build();
        sSession.setPlaybackState(state);
        sSession.setActive(true);

        postNotification(ctx);
    }

    private static void ensureSession(Context ctx) {
        if (sSession != null) return;
        sSession = new MediaSession(ctx, "VitaPlex");
        sSession.setFlags(MediaSession.FLAG_HANDLES_MEDIA_BUTTONS
                          | MediaSession.FLAG_HANDLES_TRANSPORT_CONTROLS);
        sSession.setCallback(new MediaSession.Callback() {
            @Override public void onPlay() { send(CODE_PLAY); }
            @Override public void onPause() { send(CODE_PAUSE); }
            @Override public void onSkipToNext() { send(CODE_NEXT); }
            @Override public void onSkipToPrevious() { send(CODE_PREVIOUS); }
            @Override public void onStop() { send(CODE_STOP); }
            @Override public void onSeekTo(long pos) {
                try { nativeMediaSeek(pos); } catch (Throwable t) { Log.w(TAG, "seek", t); }
            }
        });
    }

    private static void ensureChannel(Context ctx) {
        if (sChannelCreated || Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return;
        NotificationManager nm = (NotificationManager)
            ctx.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm == null) return;
        NotificationChannel ch = new NotificationChannel(
            CHANNEL_ID, "Music playback", NotificationManager.IMPORTANCE_LOW);
        ch.setShowBadge(false);
        ch.setLockscreenVisibility(Notification.VISIBILITY_PUBLIC);
        nm.createNotificationChannel(ch);
        sChannelCreated = true;
    }

    private static void ensureReceiver(Context ctx) {
        if (sReceiver != null) return;
        sReceiver = new BroadcastReceiver() {
            @Override public void onReceive(Context c, Intent i) {
                if (i == null || !ACTION.equals(i.getAction())) return;
                send(i.getIntExtra(EXTRA_CODE, 0));
            }
        };
        IntentFilter f = new IntentFilter(ACTION);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            ctx.registerReceiver(sReceiver, f, Context.RECEIVER_NOT_EXPORTED);
        } else {
            ctx.registerReceiver(sReceiver, f);
        }
    }

    private static void postNotification(Context ctx) {
        NotificationManager nm = (NotificationManager)
            ctx.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm == null) return;

        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            b = new Notification.Builder(ctx, CHANNEL_ID);
        } else {
            b = new Notification.Builder(ctx);
        }
        b.setContentTitle(sTitle)
         .setContentText(sArtist)
         .setSmallIcon(android.R.drawable.ic_media_play)
         .setVisibility(Notification.VISIBILITY_PUBLIC)
         .setOngoing(sPlaying)
         .setShowWhen(false);
        if (!sAlbum.isEmpty()) b.setSubText(sAlbum);
        if (sArtBitmap != null) b.setLargeIcon(sArtBitmap);

        Intent open = ctx.getPackageManager().getLaunchIntentForPackage(ctx.getPackageName());
        if (open != null) {
            int piFlags = PendingIntent.FLAG_UPDATE_CURRENT;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) piFlags |= PendingIntent.FLAG_IMMUTABLE;
            b.setContentIntent(PendingIntent.getActivity(ctx, 100, open, piFlags));
        }

        int idx = 0, prevIdx = -1, toggleIdx, nextIdx = -1;
        if (sHasPrev) {
            b.addAction(action(ctx, android.R.drawable.ic_media_previous, "Previous", CODE_PREVIOUS));
            prevIdx = idx++;
        }
        b.addAction(action(ctx,
            sPlaying ? android.R.drawable.ic_media_pause : android.R.drawable.ic_media_play,
            sPlaying ? "Pause" : "Play", CODE_TOGGLE));
        toggleIdx = idx++;
        if (sHasNext) {
            b.addAction(action(ctx, android.R.drawable.ic_media_next, "Next", CODE_NEXT));
            nextIdx = idx++;
        }

        Notification.MediaStyle style = new Notification.MediaStyle()
            .setMediaSession(sSession.getSessionToken());
        if (prevIdx >= 0 && nextIdx >= 0) {
            style.setShowActionsInCompactView(prevIdx, toggleIdx, nextIdx);
        } else {
            style.setShowActionsInCompactView(toggleIdx);
        }
        b.setStyle(style);

        try {
            nm.notify(NOTIFICATION_ID, b.build());
        } catch (Throwable t) {
            Log.w(TAG, "notify failed", t);
        }
    }

    private static Notification.Action action(Context ctx, int icon, String title, int code) {
        Intent i = new Intent(ACTION).setPackage(ctx.getPackageName()).putExtra(EXTRA_CODE, code);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) flags |= PendingIntent.FLAG_IMMUTABLE;
        PendingIntent pi = PendingIntent.getBroadcast(ctx, code, i, flags);
        return new Notification.Action.Builder(icon, title, pi).build();
    }

    private static void send(int code) {
        try { nativeMediaAction(code); } catch (Throwable t) { Log.w(TAG, "nativeMediaAction", t); }
    }

    private static void maybeLoadArt(final String url) {
        if (url == null || url.isEmpty()) { sArtBitmap = null; sLoadedArtUrl = null; return; }
        if (url.equals(sLoadedArtUrl) && sArtBitmap != null) return;  // already have it
        new Thread(new Runnable() {
            @Override public void run() {
                final Bitmap bmp = loadBitmap(url);
                if (bmp == null) return;
                sMain.post(new Runnable() {
                    @Override public void run() {
                        if (!url.equals(sArtUrl)) return;  // track moved on; discard
                        sArtBitmap = bmp;
                        sLoadedArtUrl = url;
                        try { applyUpdate(); } catch (Throwable t) { Log.w(TAG, "art apply", t); }
                    }
                });
            }
        }).start();
    }

    private static Bitmap loadBitmap(String url) {
        try {
            if (url.startsWith("/") || url.startsWith("file:")) {
                String path = url.startsWith("file:") ? Uri.parse(url).getPath() : url;
                return BitmapFactory.decodeFile(path);
            }
            HttpURLConnection conn = (HttpURLConnection) new URL(url).openConnection();
            conn.setConnectTimeout(8000);
            conn.setReadTimeout(8000);
            conn.setInstanceFollowRedirects(true);
            InputStream in = conn.getInputStream();
            Bitmap bmp = BitmapFactory.decodeStream(in);
            in.close();
            conn.disconnect();
            return bmp;
        } catch (Throwable t) {
            Log.w(TAG, "loadBitmap failed: " + t);
            return null;
        }
    }
}
