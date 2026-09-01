package org.VitaPlex.app;

import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.SparseArray;

import android.support.v4.media.MediaBrowserCompat;
import android.support.v4.media.MediaDescriptionCompat;
import androidx.media.MediaBrowserServiceCompat;

import java.util.ArrayList;
import java.util.List;

/**
 * Exposes the Plex music library to system media browsers — Android Auto,
 * Google Assistant ("play X on VitaPlex"), Wear, and Android TV.
 *
 * MediaBrowserServiceCompat, matching MediaNotification's MediaSessionCompat —
 * the token types have to agree, and the compat session is what lets a client
 * set shuffle/repeat at all (the framework MediaSession.Callback has no
 * onSetShuffleMode/onSetRepeatMode). The session advertised here is that same
 * session, so a browser client gets working transport controls for free.
 *
 * The library itself lives in native code (PlexClient), so onLoadChildren hands
 * the request to JNI and detaches; native answers asynchronously through
 * deliverChildren(). Requests are matched by an int token because several
 * browsers can expand several nodes at once.
 */
public final class LibraryBrowserService extends MediaBrowserServiceCompat {
    private static final String TAG = "VitaPlexBrowse";

    /** Root of the browse tree. Keep in sync with android_media_browser.cpp. */
    private static final String ROOT_ID = "__root__";
    // Android 11+ media resumption uses a separate, single-item root: the system
    // asks for it with EXTRA_RECENT after a reboot and never browses further.
    private static final String RECENT_ROOT_ID = "__recent__";
    private static final String RECENT_PREFS = "vitaplex_recent";
    private static final String KEY_ID = "mediaId", KEY_TITLE = "title";
    private static final String KEY_SUBTITLE = "subtitle", KEY_ART = "art";

    private static final Handler sMain = new Handler(Looper.getMainLooper());

    // Pending onLoadChildren results, keyed by the token handed to native.
    private static final SparseArray<Result<List<MediaBrowserCompat.MediaItem>>> sPending =
            new SparseArray<>();
    private static int sNextToken = 1;

    private static boolean sNativeReady = false;
    private static android.content.Context sAppContext;

    private static native void nativeInit(String filesDir);
    private static native void nativeLoadChildren(String parentId, int token);
    private static native void nativePlayFromMediaId(String mediaId);
    private static native void nativePlayFromSearch(String query);

    /**
     * Bring the native side up in this process.
     *
     * Android Auto, Assistant and watch companions bind this service without
     * launching the activity, so the process can start with no native library
     * loaded at all. Load the same libraries VitaPlexActivity does (already-
     * loaded ones are a no-op) and hand native the Context's files dir, which
     * is how it locates the saved config when SDL's JNI setup has never run.
     */
    private static synchronized boolean ensureNative(android.content.Context ctx) {
        if (sNativeReady) return true;
        if (ctx == null) return false;
        try {
            System.loadLibrary("curl");
            System.loadLibrary("SDL2");
            System.loadLibrary("VitaPlex");
            nativeInit(ctx.getFilesDir().getAbsolutePath());
            sNativeReady = true;
        } catch (Throwable t) {
            Log.w(TAG, "native bridge unavailable", t);
        }
        return sNativeReady;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        sAppContext = getApplicationContext();
        ensureNative(this);
        try {
            // A browser binds before anything is playing, so the session must
            // already exist — MediaNotification creates it on demand here.
            setSessionToken(MediaNotification.getSessionToken(this));
        } catch (Throwable t) {
            Log.w(TAG, "setSessionToken failed", t);
        }
    }

    @Override
    public BrowserRoot onGetRoot(String clientPackageName, int clientUid, Bundle rootHints) {
        // Media resumption: after a reboot the system binds us with EXTRA_RECENT
        // to rebuild a player for whatever was last playing. Answer with the
        // one-item root — but only when there is something to resume, since a
        // root with no children leaves a dead player in the carousel.
        if (rootHints != null && rootHints.getBoolean(BrowserRoot.EXTRA_RECENT, false)) {
            return recentMediaId().isEmpty() ? null : new BrowserRoot(RECENT_ROOT_ID, null);
        }
        // Browsing exposes nothing a caller could not already reach through the
        // app itself, and the media ids are useless without the app's own Plex
        // credentials, so every caller gets the same read-only root.
        return new BrowserRoot(ROOT_ID, null);
    }

    /**
     * Remember what is playing, so the system can offer it again after a reboot.
     * Called from MediaNotification whenever the track changes.
     */
    static void rememberRecent(String mediaId, String title, String subtitle, String artUri) {
        try {
            android.content.Context ctx = sAppContext != null
                    ? sAppContext : VitaPlexActivity.getAppContext();
            if (ctx == null || mediaId == null || mediaId.isEmpty()) return;
            ctx.getSharedPreferences(RECENT_PREFS, android.content.Context.MODE_PRIVATE)
               .edit()
               .putString(KEY_ID, mediaId)
               .putString(KEY_TITLE, title != null ? title : "")
               .putString(KEY_SUBTITLE, subtitle != null ? subtitle : "")
               .putString(KEY_ART, artUri != null ? artUri : "")
               .apply();
        } catch (Throwable t) {
            Log.w(TAG, "rememberRecent failed", t);
        }
    }

    private String recentMediaId() {
        try {
            return getSharedPreferences(RECENT_PREFS, android.content.Context.MODE_PRIVATE)
                    .getString(KEY_ID, "");
        } catch (Throwable t) {
            return "";
        }
    }

    /** The single row the resumption root serves: last track, playable. */
    private List<MediaBrowserCompat.MediaItem> recentChildren() {
        List<MediaBrowserCompat.MediaItem> out = new ArrayList<>();
        try {
            android.content.SharedPreferences p =
                    getSharedPreferences(RECENT_PREFS, android.content.Context.MODE_PRIVATE);
            String id = p.getString(KEY_ID, "");
            if (id.isEmpty()) return out;
            MediaDescriptionCompat.Builder d = new MediaDescriptionCompat.Builder()
                    .setMediaId(id)
                    .setTitle(p.getString(KEY_TITLE, ""))
                    .setSubtitle(p.getString(KEY_SUBTITLE, ""));
            String art = p.getString(KEY_ART, "");
            if (!art.isEmpty()) d.setIconUri(Uri.parse(art));
            out.add(new MediaBrowserCompat.MediaItem(
                    d.build(), MediaBrowserCompat.MediaItem.FLAG_PLAYABLE));
        } catch (Throwable t) {
            Log.w(TAG, "recentChildren failed", t);
        }
        return out;
    }

    @Override
    public void onLoadChildren(String parentId, Result<List<MediaBrowserCompat.MediaItem>> result) {
        // The resumption root is served straight from disk: native may not even
        // be loadable this early after a reboot, and one row is all it wants.
        if (RECENT_ROOT_ID.equals(parentId)) {
            result.sendResult(recentChildren());
            return;
        }
        result.detach();

        final int token;
        synchronized (sPending) {
            token = sNextToken++;
            sPending.put(token, result);
        }

        try {
            if (!ensureNative(this)) throw new IllegalStateException("native bridge unavailable");
            nativeLoadChildren(parentId, token);
        } catch (Throwable t) {
            // The native library could not be brought up in this process. Say so
            // in the browser rather than showing an unexplained empty list.
            Log.w(TAG, "nativeLoadChildren unavailable", t);
            deliverChildren(token,
                    new String[] { "__unavailable__" },
                    new String[] { "Open VitaPlex to browse your library" },
                    new String[] { "" },
                    new String[] { "" },
                    new int[] { 0 });
        }
    }

    /**
     * Completes a pending onLoadChildren. Called from native on the borealis
     * main loop, which is not the Android main looper, so this re-posts before
     * touching the Result.
     */
    public static void deliverChildren(final int token, final String[] ids, final String[] titles,
                                       final String[] subtitles, final String[] iconUris,
                                       final int[] flags) {
        sMain.post(new Runnable() {
            @Override
            public void run() {
                Result<List<MediaBrowserCompat.MediaItem>> r;
                synchronized (sPending) {
                    r = sPending.get(token);
                    sPending.remove(token);
                }
                if (r == null) return;

                List<MediaBrowserCompat.MediaItem> out = new ArrayList<>();
                if (ids != null) {
                    for (int i = 0; i < ids.length; i++) {
                        if (ids[i] == null) continue;
                        MediaDescriptionCompat.Builder d =
                                new MediaDescriptionCompat.Builder().setMediaId(ids[i]);
                        if (titles != null && i < titles.length)
                            d.setTitle(titles[i]);
                        if (subtitles != null && i < subtitles.length
                                && subtitles[i] != null && !subtitles[i].isEmpty())
                            d.setSubtitle(subtitles[i]);
                        if (iconUris != null && i < iconUris.length
                                && iconUris[i] != null && !iconUris[i].isEmpty())
                            d.setIconUri(Uri.parse(iconUris[i]));

                        int f = (flags != null && i < flags.length) ? flags[i] : 0;
                        out.add(new MediaBrowserCompat.MediaItem(d.build(), f));
                    }
                }

                try {
                    r.sendResult(out);
                } catch (Throwable t) {
                    Log.w(TAG, "sendResult failed", t);
                }
            }
        });
    }

    /** Routed here from MediaNotification's session callback. */
    static void playFromMediaId(String mediaId) {
        if (mediaId == null || mediaId.isEmpty()) return;
        ensureAppRunning();
        try {
            if (!ensureNative(sAppContext)) return;
            nativePlayFromMediaId(mediaId);
        } catch (Throwable t) {
            Log.w(TAG, "nativePlayFromMediaId unavailable", t);
        }
    }

    /**
     * "Hey Google, play <something> on VitaPlex" — routed here from the
     * session's onPlayFromSearch. Native matches the query against the music
     * library. An empty query is Assistant's bare "play music"; native answers
     * it by resuming whatever is queued.
     */
    static void playFromSearch(String query) {
        ensureAppRunning();
        try {
            if (!ensureNative(sAppContext)) return;
            nativePlayFromSearch(query != null ? query : "");
        } catch (Throwable t) {
            Log.w(TAG, "nativePlayFromSearch unavailable", t);
        }
    }

    /**
     * A client warming the session up before it asks for anything (the session's
     * onPrepare*). The slow part of a cold request is loading the native library
     * and reconnecting to the Plex server, so do exactly that and nothing else —
     * prepare must not start playing.
     */
    static void prepare() {
        try {
            ensureNative(sAppContext);
        } catch (Throwable t) {
            Log.w(TAG, "prepare failed", t);
        }
    }

    /**
     * Browsing works without the UI, but starting playback does not: native
     * hands the queue to a PlayerActivity on the borealis loop, and that loop
     * only runs while the app is up. When a browser or Assistant asks for
     * playback cold, bring the app up — the native request is queued onto the
     * loop and runs as soon as it starts, so the pick is not lost.
     */
    private static void ensureAppRunning() {
        if (VitaPlexActivity.getAppContext() != null || sAppContext == null) return;
        try {
            android.content.Intent launch = sAppContext.getPackageManager()
                    .getLaunchIntentForPackage(sAppContext.getPackageName());
            if (launch != null) {
                launch.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
                sAppContext.startActivity(launch);
            }
        } catch (Throwable t) {
            // Android restricts background activity starts; if it is refused
            // the queued request still plays once the user opens the app.
            Log.w(TAG, "could not launch app for playback", t);
        }
    }
}
