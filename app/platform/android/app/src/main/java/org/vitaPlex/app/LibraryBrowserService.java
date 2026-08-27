package org.VitaPlex.app;

import android.media.MediaDescription;
import android.media.browse.MediaBrowser;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.service.media.MediaBrowserService;
import android.util.Log;
import android.util.SparseArray;

import java.util.ArrayList;
import java.util.List;

/**
 * Exposes the Plex music library to system media browsers — Android Auto,
 * Google Assistant ("play X on VitaPlex"), Wear, and Android TV.
 *
 * Framework android.service.media.MediaBrowserService (API 21+), not the
 * AndroidX compat class: this module has no AndroidX dependency and the rest of
 * the media code (MediaNotification) is likewise framework-only. The session
 * this advertises is the same MediaSession that drives the notification and lock
 * screen, so a browser client gets working transport controls for free.
 *
 * The library itself lives in native code (PlexClient), so onLoadChildren hands
 * the request to JNI and detaches; native answers asynchronously through
 * deliverChildren(). Requests are matched by an int token because several
 * browsers can expand several nodes at once.
 */
public final class LibraryBrowserService extends MediaBrowserService {
    private static final String TAG = "VitaPlexBrowse";

    /** Root of the browse tree. Keep in sync with android_media_browser.cpp. */
    private static final String ROOT_ID = "__root__";

    private static final Handler sMain = new Handler(Looper.getMainLooper());

    // Pending onLoadChildren results, keyed by the token handed to native.
    private static final SparseArray<Result<List<MediaBrowser.MediaItem>>> sPending =
            new SparseArray<>();
    private static int sNextToken = 1;

    private static native void nativeLoadChildren(String parentId, int token);
    private static native void nativePlayFromMediaId(String mediaId);

    @Override
    public void onCreate() {
        super.onCreate();
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
        // Browsing exposes nothing a caller could not already reach through the
        // app itself, and the media ids are useless without the app's own Plex
        // credentials, so every caller gets the same read-only root.
        return new BrowserRoot(ROOT_ID, null);
    }

    @Override
    public void onLoadChildren(String parentId, Result<List<MediaBrowser.MediaItem>> result) {
        result.detach();

        final int token;
        synchronized (sPending) {
            token = sNextToken++;
            sPending.put(token, result);
        }

        try {
            nativeLoadChildren(parentId, token);
        } catch (Throwable t) {
            // UnsatisfiedLinkError when the browser bound us without the app
            // process having loaded the native library. Say so in the browser
            // rather than showing an unexplained empty list.
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
                Result<List<MediaBrowser.MediaItem>> r;
                synchronized (sPending) {
                    r = sPending.get(token);
                    sPending.remove(token);
                }
                if (r == null) return;

                List<MediaBrowser.MediaItem> out = new ArrayList<>();
                if (ids != null) {
                    for (int i = 0; i < ids.length; i++) {
                        if (ids[i] == null) continue;
                        MediaDescription.Builder d =
                                new MediaDescription.Builder().setMediaId(ids[i]);
                        if (titles != null && i < titles.length)
                            d.setTitle(titles[i]);
                        if (subtitles != null && i < subtitles.length
                                && subtitles[i] != null && !subtitles[i].isEmpty())
                            d.setSubtitle(subtitles[i]);
                        if (iconUris != null && i < iconUris.length
                                && iconUris[i] != null && !iconUris[i].isEmpty())
                            d.setIconUri(Uri.parse(iconUris[i]));

                        int f = (flags != null && i < flags.length) ? flags[i] : 0;
                        out.add(new MediaBrowser.MediaItem(d.build(), f));
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
        try {
            nativePlayFromMediaId(mediaId);
        } catch (Throwable t) {
            Log.w(TAG, "nativePlayFromMediaId unavailable", t);
        }
    }
}
