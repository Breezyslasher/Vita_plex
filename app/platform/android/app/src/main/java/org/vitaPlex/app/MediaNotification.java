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
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
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
    // Package-visible so MusicService can foreground this same notification.
    static final int NOTIFICATION_ID = 0x7654;

    // Transport codes shared with src/utils/now_playing.cpp (keep in sync).
    private static final int CODE_TOGGLE = 1;
    private static final int CODE_PLAY = 2;
    private static final int CODE_PAUSE = 3;
    private static final int CODE_NEXT = 4;
    private static final int CODE_PREVIOUS = 5;
    private static final int CODE_STOP = 6;
    private static final int CODE_REPEAT = 7;    // cycle repeat off -> all -> one
    private static final int CODE_SHUFFLE = 8;   // toggle shuffle

    private static final String ACTION = "org.VitaPlex.app.MEDIA_ACTION";
    private static final String EXTRA_CODE = "code";

    // PlaybackState custom-action ids. Android 13+ builds the media controls from
    // the MediaSession's PlaybackState (it ignores notification addAction buttons),
    // so shuffle/repeat must also be exposed as custom actions to appear there.
    private static final String CUSTOM_SHUFFLE = "org.VitaPlex.app.CUSTOM_SHUFFLE";
    private static final String CUSTOM_REPEAT = "org.VitaPlex.app.CUSTOM_REPEAT";

    private static native void nativeMediaAction(int code);
    private static native void nativeMediaSeek(long positionMs);

    private static final Handler sMain = new Handler(Looper.getMainLooper());

    private static MediaSession sSession;
    private static boolean sChannelCreated;
    private static BroadcastReceiver sReceiver;
    private static boolean sServiceStarted;   // MusicService is foregrounding us
    private static PowerManager.WakeLock sWakeLock;   // held only while playing
    private static WifiManager.WifiLock sWifiLock;    // held only while playing

    // Last-known state, so an async art load can re-post without re-plumbing.
    private static String sTitle = "", sArtist = "", sAlbum = "", sArtUrl = "";
    private static long sDurationMs, sPositionMs;
    private static boolean sPlaying, sHasNext, sHasPrev;
    private static int sRepeat;          // 0 off, 1 all, 2 one
    private static boolean sShuffle;
    private static boolean sShowModes;   // expose repeat/shuffle (music, not video)
    private static String sLoadedArtUrl;   // url whose bitmap is in sArtBitmap
    private static Bitmap sArtBitmap;

    private MediaNotification() {}

    /** Called from native (any thread). Marshals to the main looper. */
    public static void update(final String title, final String artist, final String album,
                              final String artUrl, final long durationMs, final long positionMs,
                              final boolean playing, final boolean hasNext, final boolean hasPrev,
                              final int repeat, final boolean shuffle, final boolean showModes) {
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
                sRepeat = repeat;
                sShuffle = shuffle;
                sShowModes = showModes;
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
                    releaseLocks();  // never leave the CPU/Wi-Fi held after playback ends
                    Context ctx = VitaPlexActivity.getAppContext();
                    if (ctx != null) {
                        stopService(ctx);  // drop the foreground service first
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

        // Always advertise prev/next so the system media controls keep both
        // buttons visible even at the first/last track (the queue just no-ops
        // there). Gating on hasPrev made the Previous button vanish on track 1.
        long actions = PlaybackState.ACTION_PLAY_PAUSE | PlaybackState.ACTION_PLAY
            | PlaybackState.ACTION_PAUSE | PlaybackState.ACTION_SEEK_TO | PlaybackState.ACTION_STOP
            | PlaybackState.ACTION_SKIP_TO_NEXT | PlaybackState.ACTION_SKIP_TO_PREVIOUS;
        PlaybackState.Builder psb = new PlaybackState.Builder()
            .setActions(actions)
            .setState(sPlaying ? PlaybackState.STATE_PLAYING : PlaybackState.STATE_PAUSED,
                      sPositionMs, 1.0f, SystemClock.elapsedRealtime());
        // Shuffle/repeat as PlaybackState custom actions so they appear in the
        // Android 13+ system media controls (which ignore notification actions).
        // onCustomAction() routes them back; pre-13 uses the notification actions.
        if (sShowModes) {
            int shufIcon = drawableId(ctx, sShuffle ? "ic_shuffle_on" : "ic_shuffle");
            if (shufIcon != 0) {
                psb.addCustomAction(new PlaybackState.CustomAction.Builder(
                    CUSTOM_SHUFFLE, sShuffle ? "Shuffle on" : "Shuffle off", shufIcon).build());
            }
            int repIcon = drawableId(ctx, sRepeat == 2 ? "ic_repeat_one" : sRepeat == 1 ? "ic_repeat_on" : "ic_repeat");
            if (repIcon != 0) {
                String rt = sRepeat == 2 ? "Repeat one" : (sRepeat == 1 ? "Repeat all" : "Repeat off");
                psb.addCustomAction(new PlaybackState.CustomAction.Builder(
                    CUSTOM_REPEAT, rt, repIcon).build());
            }
        }
        sSession.setPlaybackState(psb.build());
        sSession.setActive(true);

        updateLocks(ctx, sPlaying);
        postNotification(ctx);
    }

    // Hold the CPU (PARTIAL_WAKE_LOCK) and Wi-Fi radio (FULL_HIGH_PERF) awake only
    // while actually playing, so a screen-off device can't suspend the CPU / park
    // the Wi-Fi radio and stall the network transcode stream. Released the moment
    // we pause (and on clear()) so we don't drain the battery sitting paused in the
    // background. Both are non-reference-counted + isHeld()-guarded, so the
    // repeated applyUpdate() path can't double-acquire or over-release.
    private static void updateLocks(Context ctx, boolean playing) {
        try {
            if (playing) {
                if (sWakeLock == null) {
                    PowerManager pm = (PowerManager) ctx.getSystemService(Context.POWER_SERVICE);
                    if (pm != null) {
                        sWakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "VitaPlex:music");
                        sWakeLock.setReferenceCounted(false);
                    }
                }
                if (sWakeLock != null && !sWakeLock.isHeld()) sWakeLock.acquire();

                if (sWifiLock == null) {
                    WifiManager wm = (WifiManager)
                        ctx.getApplicationContext().getSystemService(Context.WIFI_SERVICE);
                    if (wm != null) {
                        sWifiLock = wm.createWifiLock(
                            WifiManager.WIFI_MODE_FULL_HIGH_PERF, "VitaPlex:music");
                        sWifiLock.setReferenceCounted(false);
                    }
                }
                if (sWifiLock != null && !sWifiLock.isHeld()) sWifiLock.acquire();
            } else {
                releaseLocks();
            }
        } catch (Throwable t) {
            Log.w(TAG, "updateLocks failed", t);
        }
    }

    private static void releaseLocks() {
        try {
            if (sWifiLock != null && sWifiLock.isHeld()) sWifiLock.release();
            if (sWakeLock != null && sWakeLock.isHeld()) sWakeLock.release();
        } catch (Throwable ignore) {}
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
            // Android 13+ media controls fire shuffle/repeat as custom actions
            // (the framework Callback has no onSetRepeatMode/onSetShuffleMode).
            @Override public void onCustomAction(String action, Bundle extras) {
                if (CUSTOM_SHUFFLE.equals(action))      send(CODE_SHUFFLE);
                else if (CUSTOM_REPEAT.equals(action))  send(CODE_REPEAT);
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
        Notification n = buildNotification(ctx);
        if (n == null) return;
        try {
            nm.notify(NOTIFICATION_ID, n);
        } catch (Throwable t) {
            Log.w(TAG, "notify failed", t);
        }
        ensureService(ctx);  // run a media foreground service so playback survives backgrounding
    }

    /**
     * Build the current MediaStyle notification. Package-visible: MusicService
     * calls this to obtain the notification it foregrounds with. Returns null
     * before a session exists.
     */
    static Notification buildNotification(Context ctx) {
        ensureSession(ctx);
        ensureChannel(ctx);
        if (sSession == null) return null;

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

        // Order: shuffle, prev, play/pause, next, repeat. Compact view keeps
        // prev/toggle/next; shuffle + repeat are extras (music only — sShowModes).
        // Prev/next are ALWAYS shown so the transport row doesn't reshuffle (and
        // prev doesn't vanish) at the first/last track — the queue just no-ops
        // there. Glyphs vary by state: shuffle on/off and repeat off/all/one each
        // get a distinct icon so the current mode is readable. Looked up by name
        // so this hand-written Java needs no generated-R dependency.
        int idx = 0, prevIdx, toggleIdx, nextIdx;

        int shuffleIcon = sShowModes ? drawableId(ctx, sShuffle ? "ic_shuffle_on" : "ic_shuffle") : 0;
        if (shuffleIcon != 0) {
            b.addAction(action(ctx, shuffleIcon, sShuffle ? "Shuffle on" : "Shuffle off", CODE_SHUFFLE));
            idx++;
        }
        b.addAction(action(ctx, android.R.drawable.ic_media_previous, "Previous", CODE_PREVIOUS));
        prevIdx = idx++;
        b.addAction(action(ctx,
            sPlaying ? android.R.drawable.ic_media_pause : android.R.drawable.ic_media_play,
            sPlaying ? "Pause" : "Play", CODE_TOGGLE));
        toggleIdx = idx++;
        b.addAction(action(ctx, android.R.drawable.ic_media_next, "Next", CODE_NEXT));
        nextIdx = idx++;
        int repeatIcon = sShowModes
            ? drawableId(ctx, sRepeat == 2 ? "ic_repeat_one" : sRepeat == 1 ? "ic_repeat_on" : "ic_repeat") : 0;
        if (repeatIcon != 0) {
            String rt = sRepeat == 2 ? "Repeat one" : (sRepeat == 1 ? "Repeat all" : "Repeat off");
            b.addAction(action(ctx, repeatIcon, rt, CODE_REPEAT));
            idx++;
        }

        Notification.MediaStyle style = new Notification.MediaStyle()
            .setMediaSession(sSession.getSessionToken());
        style.setShowActionsInCompactView(prevIdx, toggleIdx, nextIdx);
        b.setStyle(style);
        return b.build();
    }

    // Resolve a drawable resource id by name (avoids a compile-time R dependency
    // from this hand-written Java). Returns 0 if not found.
    private static int drawableId(Context ctx, String name) {
        try {
            return ctx.getResources().getIdentifier(name, "drawable", ctx.getPackageName());
        } catch (Throwable t) {
            return 0;
        }
    }

    // Start the media foreground service so audio + the notification survive the
    // app being backgrounded / the screen turning off. Started from the app's
    // foreground (music begins while the app is visible), so it's allowed.
    private static void ensureService(Context ctx) {
        if (sServiceStarted) return;
        try {
            Intent i = new Intent(ctx, MusicService.class);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                ctx.startForegroundService(i);
            } else {
                ctx.startService(i);
            }
            sServiceStarted = true;
        } catch (Throwable t) {
            Log.w(TAG, "startForegroundService failed", t);
        }
    }

    private static void stopService(Context ctx) {
        if (!sServiceStarted) return;
        try { ctx.stopService(new Intent(ctx, MusicService.class)); } catch (Throwable ignore) {}
        sServiceStarted = false;
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
