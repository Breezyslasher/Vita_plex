package org.VitaPlex.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.media.audiofx.AudioEffect;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemClock;
import android.util.Log;

import android.support.v4.media.MediaDescriptionCompat;
import android.support.v4.media.RatingCompat;
import android.support.v4.media.MediaMetadataCompat;
import android.support.v4.media.session.MediaSessionCompat;
import android.support.v4.media.session.PlaybackStateCompat;
import androidx.core.app.NotificationCompat;
import androidx.media.session.MediaButtonReceiver;

import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.List;

/**
 * MediaSessionCompat + MediaStyle notification for music playback.
 *
 * Driven from native (MusicController) through update()/clear(); transport
 * buttons (lock screen, notification shade, headset) route back to native via
 * nativeMediaAction() / nativeMediaSeek(). Everything runs on the main looper;
 * art is fetched off-thread.
 *
 * Compat rather than the framework MediaSession because MediaSession.Callback
 * has no onSetShuffleMode/onSetRepeatMode: with it, a remote controller had no
 * standard way to set those modes and no way to read the current state, leaving
 * only app-specific PlaybackState custom actions that most clients ignore.
 * MediaSessionCompat exposes both, so Android Auto head units and watch media
 * browsers get real shuffle/repeat controls.
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
    // Explicit mode setters, used by MediaSessionCompat's onSetRepeatMode /
    // onSetShuffleMode. Mode ints use this class's own convention (0 off, 1 all,
    // 2 one) — the same one update() receives — so both directions agree.
    private static native void nativeSetRepeatMode(int mode);
    private static native void nativeSetShuffle(boolean on);
    // A row picked out of the published queue; the long is the id we published.
    private static native void nativeSkipToQueueItem(long id);
    // "Like this track" — writes the Plex user rating of what's playing.
    private static native void nativeSetRating(boolean liked);

    private static final Handler sMain = new Handler(Looper.getMainLooper());

    private static MediaSessionCompat sSession;
    private static boolean sChannelCreated;
    private static BroadcastReceiver sReceiver;
    private static boolean sServiceStarted;   // MusicService is foregrounding us
    // Audio focus. Without holding it Android never asks us to stop, so starting
    // a video or a podcast elsewhere left both playing over each other.
    private static Object  sFocusRequest;        // AudioFocusRequest on O+, unused below
    private static boolean sHasFocus;
    private static boolean sPausedByFocusLoss;   // only resume what focus loss paused

    // Headphones pulled, or Bluetooth gone. Not the same as losing audio focus —
    // nothing takes focus when a jack is removed — so this is the only thing
    // that stops music blaring out of the speaker.
    private static BroadcastReceiver sNoisyReceiver;
    private static boolean sNoisyRegistered;

    // System equalizer plumbing. mpv only routes through our audio session if
    // this libmpv accepted --audiotrack-session-id, which native tells us; until
    // then the session is never advertised, so an effect host can't attach to
    // audio that isn't going through it.
    private static boolean sAudioSessionUsable;
    private static boolean sEffectSessionOpen;

    private static PowerManager.WakeLock sWakeLock;   // held only while playing
    private static WifiManager.WifiLock sWifiLock;    // held only while playing

    // Last-known state, so an async art load can re-post without re-plumbing.
    private static String sMediaId = "";   // browse-tree id of the current track
    private static String sRememberedMediaId;  // last id written for media resumption
    private static String sTitle = "", sArtist = "", sAlbum = "", sArtUrl = "";
    private static long sDurationMs, sPositionMs;
    private static boolean sPlaying, sHasNext, sHasPrev;
    private static int sRepeat;          // 0 off, 1 all, 2 one
    private static boolean sShuffle;
    private static boolean sShowModes;   // expose repeat/shuffle (music, not video)
    // Viewer's own 0-10 Plex rating; -1 when the item has no rating concept.
    // Published as a heart, which is all the Android controls offer.
    private static float sUserRating = -1f;
    private static String sLoadedArtUrl;   // url whose bitmap is in sArtBitmap
    private static Bitmap sArtBitmap;
    private static boolean sHasState;    // native has pushed a track at least once
    private static int sQueueSize;       // rows currently published to the session
    private static long sActiveQueueId = MediaSessionCompat.QueueItem.UNKNOWN_ID;

    private MediaNotification() {}

    /**
     * Session token for MediaBrowserService.setSessionToken().
     *
     * The browser service is bound by Android Auto / Assistant / Wear before any
     * music is playing, so the session has to exist up front rather than being
     * created lazily on the first update(). Creating it early is harmless: an
     * idle MediaSession posts nothing until setActive()/setPlaybackState() run.
     * Callable from the browser service's onCreate (main looper).
     */
    static MediaSessionCompat.Token getSessionToken(Context ctx) {
        ensureSession(ctx);
        return sSession != null ? sSession.getSessionToken() : null;
    }

    /**
     * The live session, for MusicService to feed media-button intents into.
     * Creates one if needed: a media key can arrive before any update() has,
     * and the session is what decodes the key event into a transport callback.
     */
    static MediaSessionCompat getSession(Context ctx) {
        ensureSession(ctx);
        return sSession;
    }

    /** Called from native once mpv has accepted our audio session id. */
    public static void setAudioSessionUsable() {
        sMain.post(new Runnable() {
            @Override public void run() { sAudioSessionUsable = true; }
        });
    }

    /**
     * Tell the system's AudioEffect hosts (the stock equalizer and friends)
     * that our audio session exists, so their controls apply to our output.
     * Opened once playback is actually running and closed when it ends.
     */
    private static void updateEffectSession(Context ctx, boolean open) {
        if (!sAudioSessionUsable || open == sEffectSessionOpen) return;
        try {
            final int session = VitaPlexActivity.audioSessionId();
            if (session == 0) return;
            Intent i = new Intent(open
                ? AudioEffect.ACTION_OPEN_AUDIO_EFFECT_CONTROL_SESSION
                : AudioEffect.ACTION_CLOSE_AUDIO_EFFECT_CONTROL_SESSION);
            i.putExtra(AudioEffect.EXTRA_AUDIO_SESSION, session);
            i.putExtra(AudioEffect.EXTRA_PACKAGE_NAME, ctx.getPackageName());
            if (open) i.putExtra(AudioEffect.EXTRA_CONTENT_TYPE, AudioEffect.CONTENT_TYPE_MUSIC);
            ctx.sendBroadcast(i);
            sEffectSessionOpen = open;
        } catch (Throwable t) {
            Log.w(TAG, "effect session broadcast failed", t);
        }
    }

    // ---- Read-only view of the current state, for the tile and the widget ----
    //
    // Both live outside this class and outside the player, and neither should
    // be poking at the session; these are the whole of what they need.

    /** True once native has pushed a track, i.e. there is something to control. */
    static boolean isPlaybackLoaded() { return sHasState; }
    static boolean isPlaying() { return sPlaying; }
    static String currentTitle() { return sTitle != null ? sTitle : ""; }
    static String currentArtist() { return sArtist != null ? sArtist : ""; }
    static Bitmap currentArt() { return sArtBitmap; }

    /** Send a transport code as if a notification button had been pressed. */
    static void sendTransport(Context ctx, int code) {
        try {
            ctx.sendBroadcast(new Intent(ACTION)
                .setPackage(ctx.getPackageName())
                .putExtra(EXTRA_CODE, code));
        } catch (Throwable t) {
            Log.w(TAG, "sendTransport failed", t);
        }
    }

    /** PendingIntent flags with the mutability bit Android 12+ demands. */
    private static int pendingIntentFlags(int extra) {
        int f = PendingIntent.FLAG_UPDATE_CURRENT | extra;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) f |= PendingIntent.FLAG_IMMUTABLE;
        return f;
    }

    /** Called from native (any thread). Marshals to the main looper. */
    public static void update(final String mediaId,
                              final String title, final String artist, final String album,
                              final String artUrl, final long durationMs, final long positionMs,
                              final boolean playing, final boolean hasNext, final boolean hasPrev,
                              final int repeat, final boolean shuffle, final boolean showModes,
                              final float userRating) {
        sMain.post(new Runnable() {
            @Override public void run() {
                sMediaId = mediaId != null ? mediaId : "";
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
                sUserRating = userRating;
                sHasState = true;
                // Android 11+ media resumption: the system rebuilds a player
                // for the last thing that played, long after this process is
                // gone, so it has to be on disk rather than in these statics.
                if (!sMediaId.isEmpty() && !sMediaId.equals(sRememberedMediaId)) {
                    sRememberedMediaId = sMediaId;
                    LibraryBrowserService.rememberRecent(sMediaId, sTitle, sArtist, sArtUrl);
                }
                // Drop a stale cover the instant the track changes.
                if (!sArtUrl.equals(sLoadedArtUrl)) sArtBitmap = null;
                try { applyUpdate(); } catch (Throwable t) { Log.w(TAG, "update failed", t); }
                maybeLoadArt(sArtUrl);
            }
        });
    }

    /**
     * Called from native (any thread). Publishes the play queue and which row is
     * playing, so Android Auto / Assistant / Wear can show an up-next list and
     * jump straight to a track instead of pressing Next repeatedly.
     *
     * Arrays are parallel; ids are opaque handles that come back through
     * onSkipToQueueItem. The caller sends a window around the current track —
     * the whole list crosses a Binder transaction.
     */
    public static void setQueue(final long[] ids, final String[] mediaIds,
                                final String[] titles, final String[] artists,
                                final String[] artUrls, final long activeId) {
        sMain.post(new Runnable() {
            @Override public void run() {
                try {
                    Context ctx = VitaPlexActivity.getAppContext();
                    if (ctx == null) return;
                    ensureSession(ctx);
                    if (sSession == null) return;

                    final int n = ids != null ? ids.length : 0;
                    if (n == 0) {
                        sSession.setQueue(null);
                        sQueueSize = 0;
                        sActiveQueueId = MediaSessionCompat.QueueItem.UNKNOWN_ID;
                        return;
                    }
                    List<MediaSessionCompat.QueueItem> q = new ArrayList<>(n);
                    for (int i = 0; i < n; i++) {
                        MediaDescriptionCompat.Builder d = new MediaDescriptionCompat.Builder()
                            .setMediaId(str(mediaIds, i))
                            .setTitle(str(titles, i))
                            .setSubtitle(str(artists, i));
                        String art = str(artUrls, i);
                        if (!art.isEmpty()) d.setIconUri(Uri.parse(art));
                        q.add(new MediaSessionCompat.QueueItem(d.build(), ids[i]));
                    }
                    sSession.setQueue(q);
                    sQueueSize = n;
                    sActiveQueueId = activeId;
                } catch (Throwable t) {
                    Log.w(TAG, "setQueue failed", t);
                }
            }
        });
    }

    private static String str(String[] a, int i) {
        return (a != null && i < a.length && a[i] != null) ? a[i] : "";
    }

    /** Called from native (any thread). Tears down the session + notification. */
    public static void clear() {
        sMain.post(new Runnable() {
            @Override public void run() {
                try {
                    releaseLocks();  // never leave the CPU/Wi-Fi held after playback ends
                    abandonAudioFocus();
                    Context ctx = VitaPlexActivity.getAppContext();
                    if (ctx != null) {
                        updateNoisyReceiver(ctx, false);
                        updateEffectSession(ctx, false);
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
                    sHasState = false;
                    sQueueSize = 0;
                    sActiveQueueId = MediaSessionCompat.QueueItem.UNKNOWN_ID;
                    if (ctx != null) NowPlayingWidget.refresh(ctx);
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

        MediaMetadataCompat.Builder meta = new MediaMetadataCompat.Builder()
            .putString(MediaMetadataCompat.METADATA_KEY_MEDIA_ID, sMediaId)
            .putString(MediaMetadataCompat.METADATA_KEY_TITLE, sTitle)
            .putString(MediaMetadataCompat.METADATA_KEY_ARTIST, sArtist)
            .putString(MediaMetadataCompat.METADATA_KEY_ALBUM, sAlbum)
            .putLong(MediaMetadataCompat.METADATA_KEY_DURATION, sDurationMs);
        if (sArtBitmap != null) {
            meta.putBitmap(MediaMetadataCompat.METADATA_KEY_ALBUM_ART, sArtBitmap);
        }
        // Show the rating that is already set, rather than an empty heart every
        // session. Negative means the item has no rating concept (video), and
        // publishing nothing is what keeps the control off there.
        if (sUserRating >= 0f) {
            meta.putRating(MediaMetadataCompat.METADATA_KEY_USER_RATING,
                           RatingCompat.newHeartRating(sUserRating > 0f));
        }
        sSession.setMetadata(meta.build());

        // Always advertise prev/next so the system media controls keep both
        // buttons visible even at the first/last track (the queue just no-ops
        // there). Gating on hasPrev made the Previous button vanish on track 1.
        long actions = PlaybackStateCompat.ACTION_PLAY_PAUSE | PlaybackStateCompat.ACTION_PLAY
            | PlaybackStateCompat.ACTION_PAUSE | PlaybackStateCompat.ACTION_SEEK_TO
            | PlaybackStateCompat.ACTION_STOP
            | PlaybackStateCompat.ACTION_SKIP_TO_NEXT
            | PlaybackStateCompat.ACTION_SKIP_TO_PREVIOUS
            | PlaybackStateCompat.ACTION_PLAY_FROM_MEDIA_ID
            // Assistant only offers "play X on VitaPlex" when the session says
            // it can take a search. The prepare variants let a client warm the
            // session up first, which is most of the latency on a cold bind.
            | PlaybackStateCompat.ACTION_PLAY_FROM_SEARCH
            | PlaybackStateCompat.ACTION_PREPARE
            | PlaybackStateCompat.ACTION_PREPARE_FROM_SEARCH
            | PlaybackStateCompat.ACTION_PREPARE_FROM_MEDIA_ID;
        // Only offer "jump to this row" once a queue has actually been published.
        if (sQueueSize > 0) actions |= PlaybackStateCompat.ACTION_SKIP_TO_QUEUE_ITEM;
        // Rating is a music idea; a video session has nothing to like.
        if (sShowModes) actions |= PlaybackStateCompat.ACTION_SET_RATING;
        // Advertise the mode setters for music so remote controllers render real
        // shuffle/repeat controls rather than nothing. Without these in the
        // action mask a client has no way to know the session accepts them.
        if (sShowModes) {
            actions |= PlaybackStateCompat.ACTION_SET_SHUFFLE_MODE
                     | PlaybackStateCompat.ACTION_SET_REPEAT_MODE;
        }
        PlaybackStateCompat.Builder psb = new PlaybackStateCompat.Builder()
            .setActions(actions)
            .setState(sPlaying ? PlaybackStateCompat.STATE_PLAYING
                               : PlaybackStateCompat.STATE_PAUSED,
                      // Rate 0 while paused. At 1.0 a controller extrapolates
                      // from the timestamp and its scrubber keeps advancing over
                      // a track that is not moving.
                      sPositionMs, sPlaying ? 1.0f : 0.0f, SystemClock.elapsedRealtime())
            // Which published row is playing — this is what highlights the
            // current track in an up-next list.
            .setActiveQueueItemId(sActiveQueueId);
        // Keep the custom actions too: they are what the Android 13+ system
        // media controls render (those ignore notification actions), and what
        // clients that don't use the standard mode setters fall back to.
        if (sShowModes) {
            int shufIcon = drawableId(ctx, sShuffle ? "ic_shuffle_on" : "ic_shuffle");
            if (shufIcon != 0) {
                psb.addCustomAction(new PlaybackStateCompat.CustomAction.Builder(
                    CUSTOM_SHUFFLE, sShuffle ? "Shuffle on" : "Shuffle off", shufIcon).build());
            }
            int repIcon = drawableId(ctx, sRepeat == 2 ? "ic_repeat_one" : sRepeat == 1 ? "ic_repeat_on" : "ic_repeat");
            if (repIcon != 0) {
                String rt = sRepeat == 2 ? "Repeat one" : (sRepeat == 1 ? "Repeat all" : "Repeat off");
                psb.addCustomAction(new PlaybackStateCompat.CustomAction.Builder(
                    CUSTOM_REPEAT, rt, repIcon).build());
            }
        }
        sSession.setPlaybackState(psb.build());
        // Publish the modes themselves, so a controller can display the current
        // state instead of guessing — the other half of what custom actions
        // could never provide.
        if (sShowModes) {
            sSession.setShuffleMode(sShuffle ? PlaybackStateCompat.SHUFFLE_MODE_ALL
                                             : PlaybackStateCompat.SHUFFLE_MODE_NONE);
            sSession.setRepeatMode(toCompatRepeat(sRepeat));
        }
        sSession.setActive(true);

        updateLocks(ctx, sPlaying);
        updateAudioFocus(ctx, sPlaying);
        updateNoisyReceiver(ctx, sPlaying);
        // Opened on the first play and left open while paused, so an equalizer
        // keeps its settings across a pause; clear() closes it.
        if (sPlaying) updateEffectSession(ctx, true);
        postNotification(ctx);
        NowPlayingWidget.refresh(ctx);
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

    private static final AudioManager.OnAudioFocusChangeListener sFocusListener =
        new AudioManager.OnAudioFocusChangeListener() {
            @Override public void onAudioFocusChange(int change) {
                switch (change) {
                    case AudioManager.AUDIOFOCUS_LOSS:
                        // Taken for good — stop and let go. Not a candidate for
                        // resume: whatever took over is staying.
                        sPausedByFocusLoss = false;
                        if (sPlaying) send(CODE_PAUSE);
                        abandonAudioFocus();
                        break;
                    case AudioManager.AUDIOFOCUS_LOSS_TRANSIENT:
                    case AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK:
                        // A call, or a navigation prompt. Pause rather than duck:
                        // this is music being listened to, not a background bed.
                        if (sPlaying) {
                            sPausedByFocusLoss = true;
                            send(CODE_PAUSE);
                        }
                        break;
                    case AudioManager.AUDIOFOCUS_GAIN:
                        if (sPausedByFocusLoss) {
                            sPausedByFocusLoss = false;
                            send(CODE_PLAY);
                        }
                        break;
                    default:
                        break;
                }
            }
        };

    private static void updateAudioFocus(Context ctx, boolean playing) {
        try {
            if (playing) {
                requestAudioFocus(ctx);
            } else if (!sPausedByFocusLoss) {
                // Keep focus while a transient loss is what paused us, or the
                // GAIN that would resume playback never arrives.
                abandonAudioFocus(ctx);
            }
        } catch (Throwable t) {
            Log.w(TAG, "audio focus update failed", t);
        }
    }

    private static void requestAudioFocus(Context ctx) {
        if (sHasFocus || ctx == null) return;
        AudioManager am = (AudioManager) ctx.getSystemService(Context.AUDIO_SERVICE);
        if (am == null) return;
        int res;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            AudioFocusRequest req = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build())
                .setOnAudioFocusChangeListener(sFocusListener, sMain)
                .build();
            sFocusRequest = req;
            res = am.requestAudioFocus(req);
        } else {
            res = am.requestAudioFocus(sFocusListener, AudioManager.STREAM_MUSIC,
                                       AudioManager.AUDIOFOCUS_GAIN);
        }
        sHasFocus = (res == AudioManager.AUDIOFOCUS_REQUEST_GRANTED);
        if (!sHasFocus) Log.w(TAG, "audio focus denied (" + res + ")");
    }

    private static void abandonAudioFocus() {
        abandonAudioFocus(VitaPlexActivity.getAppContext());
    }

    private static void abandonAudioFocus(Context ctx) {
        if (!sHasFocus || ctx == null) return;
        try {
            AudioManager am = (AudioManager) ctx.getSystemService(Context.AUDIO_SERVICE);
            if (am != null) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                    && sFocusRequest instanceof AudioFocusRequest) {
                    am.abandonAudioFocusRequest((AudioFocusRequest) sFocusRequest);
                } else {
                    am.abandonAudioFocus(sFocusListener);
                }
            }
        } catch (Throwable ignore) {}
        sFocusRequest = null;
        sHasFocus = false;
        sPausedByFocusLoss = false;
    }

    private static void updateNoisyReceiver(Context ctx, boolean playing) {
        try {
            if (playing && !sNoisyRegistered) {
                if (sNoisyReceiver == null) {
                    sNoisyReceiver = new BroadcastReceiver() {
                        @Override public void onReceive(Context c, Intent i) {
                            if (AudioManager.ACTION_AUDIO_BECOMING_NOISY.equals(i.getAction())
                                && sPlaying) {
                                send(CODE_PAUSE);
                            }
                        }
                    };
                }
                IntentFilter f = new IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY);
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    ctx.getApplicationContext().registerReceiver(
                        sNoisyReceiver, f, Context.RECEIVER_NOT_EXPORTED);
                } else {
                    ctx.getApplicationContext().registerReceiver(sNoisyReceiver, f);
                }
                sNoisyRegistered = true;
            } else if (!playing && sNoisyRegistered) {
                ctx.getApplicationContext().unregisterReceiver(sNoisyReceiver);
                sNoisyRegistered = false;
            }
        } catch (Throwable t) {
            Log.w(TAG, "noisy receiver update failed", t);
        }
    }

    private static void releaseLocks() {
        try {
            if (sWifiLock != null && sWifiLock.isHeld()) sWifiLock.release();
            if (sWakeLock != null && sWakeLock.isHeld()) sWakeLock.release();
        } catch (Throwable ignore) {}
    }

    /** VitaPlex repeat (0 off, 1 all, 2 one) -> PlaybackStateCompat constant. */
    private static int toCompatRepeat(int vitaRepeat) {
        if (vitaRepeat == 1) return PlaybackStateCompat.REPEAT_MODE_ALL;
        if (vitaRepeat == 2) return PlaybackStateCompat.REPEAT_MODE_ONE;
        return PlaybackStateCompat.REPEAT_MODE_NONE;
    }

    /** PlaybackStateCompat constant -> VitaPlex repeat. Note the orders differ:
     *  compat is NONE/ONE/ALL/GROUP, ours is off/all/one. GROUP folds to all. */
    private static int fromCompatRepeat(int compatRepeat) {
        switch (compatRepeat) {
            case PlaybackStateCompat.REPEAT_MODE_ONE:   return 2;
            case PlaybackStateCompat.REPEAT_MODE_ALL:
            case PlaybackStateCompat.REPEAT_MODE_GROUP: return 1;
            default:                                    return 0;
        }
    }

    private static void ensureSession(Context ctx) {
        if (sSession != null) return;
        // Bind the session to the manifest's MediaButtonReceiver. Without a
        // receiver component the session only sees media keys while the app is
        // in the foreground; with one, headset and Bluetooth buttons reach us
        // from the background too (they arrive as a broadcast that the receiver
        // forwards to MusicService, which hands them back here).
        ComponentName mbr = new ComponentName(ctx, MediaButtonReceiver.class);
        Intent mbIntent = new Intent(Intent.ACTION_MEDIA_BUTTON).setComponent(mbr);
        sSession = new MediaSessionCompat(ctx, "VitaPlex", mbr,
            PendingIntent.getBroadcast(ctx, 0, mbIntent, pendingIntentFlags(0)));
        sSession.setFlags(MediaSessionCompat.FLAG_HANDLES_MEDIA_BUTTONS
                          | MediaSessionCompat.FLAG_HANDLES_TRANSPORT_CONTROLS);
        // A heart, not stars: Plex stores a 0-10 user rating, and "like this
        // track" from a car or a watch is the only rating gesture those
        // surfaces offer. Write-only for now — a track's existing rating isn't
        // carried on the queue, so the heart starts empty each session.
        sSession.setRatingType(RatingCompat.RATING_HEART);
        // What a controller opens when its "now playing" card is tapped: the
        // lock screen, Android Auto, Wear and the Android 13+ system controls
        // all use this. Unset, those surfaces have no way back into the app.
        Intent open = ctx.getPackageManager().getLaunchIntentForPackage(ctx.getPackageName());
        if (open != null) {
            sSession.setSessionActivity(
                PendingIntent.getActivity(ctx, 101, open, pendingIntentFlags(0)));
        }
        sSession.setCallback(new MediaSessionCompat.Callback() {
            @Override public void onPlay() { send(CODE_PLAY); }
            @Override public void onPause() { send(CODE_PAUSE); }
            @Override public void onSkipToNext() { send(CODE_NEXT); }
            @Override public void onSkipToPrevious() { send(CODE_PREVIOUS); }
            @Override public void onStop() { send(CODE_STOP); }
            @Override public void onSeekTo(long pos) {
                // Logged before the hop, not only on failure. A seek that never
                // moves playback can die at any of three places — Android not
                // delivering the callback, this JNI call throwing (which the
                // catch below would otherwise swallow into a warning nobody
                // reads), or mpv declining the seek — and only the presence or
                // absence of this line separates the first two.
                Log.i(TAG, "onSeekTo " + pos + "ms");
                try { nativeMediaSeek(pos); }
                catch (Throwable t) { Log.w(TAG, "onSeekTo: native call failed", t); }
            }
            // A media id picked in Android Auto / Assistant / Wear. The ids come
            // from LibraryBrowserService's tree, so it owns resolving them.
            @Override public void onPlayFromMediaId(String mediaId, Bundle extras) {
                LibraryBrowserService.playFromMediaId(mediaId);
            }
            // Spoken requests: "play <album> on VitaPlex". Prepare-variants are
            // the same work minus the play, and clients treat a missing
            // onPrepare* as "this session can't be warmed up" — so answer both.
            @Override public void onPlayFromSearch(String query, Bundle extras) {
                LibraryBrowserService.playFromSearch(query);
            }
            // Prepare means "get ready", not "start". The expensive part of a
            // cold request is loading the native library and reconnecting to
            // the Plex server, so that is what these do — no playback.
            @Override public void onPrepare() { LibraryBrowserService.prepare(); }
            @Override public void onPrepareFromSearch(String query, Bundle extras) {
                LibraryBrowserService.prepare();
            }
            @Override public void onPrepareFromMediaId(String mediaId, Bundle extras) {
                LibraryBrowserService.prepare();
            }
            // A row picked out of the up-next list.
            @Override public void onSkipToQueueItem(long id) {
                try { nativeSkipToQueueItem(id); }
                catch (Throwable t) { Log.w(TAG, "skipToQueueItem", t); }
            }
            @Override public void onSetRating(RatingCompat rating) {
                if (rating == null) return;
                try { nativeSetRating(rating.hasHeart()); }
                catch (Throwable t) { Log.w(TAG, "setRating", t); }
            }
            // Kept for clients (and the Android 13+ system controls) that drive
            // shuffle/repeat as PlaybackState custom actions. These are toggles
            // with no target, so they still route to the cycle codes.
            @Override public void onCustomAction(String action, Bundle extras) {
                if (CUSTOM_SHUFFLE.equals(action))      send(CODE_SHUFFLE);
                else if (CUSTOM_REPEAT.equals(action))  send(CODE_REPEAT);
            }
            // The reason for the compat session: a client can request a specific
            // mode instead of blindly cycling. Android Auto, Wear and watch media
            // browsers use these; the framework Callback had no such methods.
            @Override public void onSetShuffleMode(int shuffleMode) {
                final boolean on = shuffleMode == PlaybackStateCompat.SHUFFLE_MODE_ALL
                                || shuffleMode == PlaybackStateCompat.SHUFFLE_MODE_GROUP;
                try { nativeSetShuffle(on); } catch (Throwable t) { Log.w(TAG, "setShuffle", t); }
            }
            @Override public void onSetRepeatMode(int repeatMode) {
                try {
                    nativeSetRepeatMode(fromCompatRepeat(repeatMode));
                } catch (Throwable t) { Log.w(TAG, "setRepeat", t); }
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
     * when there is no track to show — a media key can start the service from a
     * cold process, and an empty notification is worse than none.
     */
    static Notification buildNotification(Context ctx) {
        ensureSession(ctx);
        ensureChannel(ctx);
        if (sSession == null || !sHasState) return null;

        // NotificationCompat handles the pre-O channel split itself, so the
        // SDK_INT branch the framework builder needed is gone.
        NotificationCompat.Builder b = new NotificationCompat.Builder(ctx, CHANNEL_ID);
        b.setContentTitle(sTitle)
         .setContentText(sArtist)
         .setSmallIcon(android.R.drawable.ic_media_play)
         .setVisibility(NotificationCompat.VISIBILITY_PUBLIC)
         .setOngoing(sPlaying)
         .setShowWhen(false);
        if (!sAlbum.isEmpty()) b.setSubText(sAlbum);
        if (sArtBitmap != null) b.setLargeIcon(sArtBitmap);

        Intent open = ctx.getPackageManager().getLaunchIntentForPackage(ctx.getPackageName());
        if (open != null) {
            b.setContentIntent(PendingIntent.getActivity(ctx, 100, open, pendingIntentFlags(0)));
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

        androidx.media.app.NotificationCompat.MediaStyle style =
            new androidx.media.app.NotificationCompat.MediaStyle()
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

    private static NotificationCompat.Action action(Context ctx, int icon, String title, int code) {
        Intent i = new Intent(ACTION).setPackage(ctx.getPackageName()).putExtra(EXTRA_CODE, code);
        PendingIntent pi = PendingIntent.getBroadcast(ctx, code, i, pendingIntentFlags(0));
        return new NotificationCompat.Action.Builder(icon, title, pi).build();
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
