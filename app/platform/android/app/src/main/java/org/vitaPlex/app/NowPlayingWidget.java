package org.VitaPlex.app;

import android.app.PendingIntent;
import android.appwidget.AppWidgetManager;
import android.appwidget.AppWidgetProvider;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.res.Resources;
import android.graphics.Bitmap;
import android.os.Build;
import android.util.Log;
import android.widget.RemoteViews;

/**
 * Home-screen now-playing widget.
 *
 * The media notification already carries these controls, but only while the
 * shade is pulled down and only among everything else that posted one. A widget
 * keeps the current track and its transport on the home screen.
 *
 * State is pushed rather than polled: MediaNotification calls refresh() whenever
 * it updates or clears, so the widget tracks playback exactly as closely as the
 * notification does and costs nothing in between. The periodic onUpdate from the
 * system is only a backstop for a widget added while nothing was playing.
 *
 * Buttons reuse MediaNotification's own broadcast, so they take the identical
 * path to native that the notification buttons do. That receiver is registered
 * at runtime, which means the buttons are live exactly while there is playback
 * to control — a dead process has nothing to pause.
 *
 * Resource ids are resolved by name, like the rest of this hand-written Java,
 * so none of it depends on the generated R class.
 */
public final class NowPlayingWidget extends AppWidgetProvider {
    private static final String TAG = "VitaPlexWidget";

    // Transport codes shared with MediaNotification (keep in sync).
    private static final int CODE_TOGGLE = 1;
    private static final int CODE_NEXT = 4;
    private static final int CODE_PREVIOUS = 5;

    @Override
    public void onUpdate(Context ctx, AppWidgetManager mgr, int[] ids) {
        for (int id : ids) render(ctx, mgr, id);
    }

    /** Called by MediaNotification whenever the track or play state changes. */
    static void refresh(Context ctx) {
        if (ctx == null) return;
        try {
            AppWidgetManager mgr = AppWidgetManager.getInstance(ctx);
            if (mgr == null) return;
            int[] ids = mgr.getAppWidgetIds(new ComponentName(ctx, NowPlayingWidget.class));
            if (ids == null || ids.length == 0) return;   // nobody has one placed
            for (int id : ids) render(ctx, mgr, id);
        } catch (Throwable t) {
            Log.w(TAG, "refresh failed", t);
        }
    }

    private static void render(Context ctx, AppWidgetManager mgr, int widgetId) {
        try {
            Resources res = ctx.getResources();
            final String pkg = ctx.getPackageName();
            int layout = res.getIdentifier("widget_now_playing", "layout", pkg);
            if (layout == 0) return;

            RemoteViews rv = new RemoteViews(pkg, layout);
            final int idTitle  = res.getIdentifier("widget_title", "id", pkg);
            final int idArtist = res.getIdentifier("widget_artist", "id", pkg);
            final int idArt    = res.getIdentifier("widget_art", "id", pkg);
            final int idToggle = res.getIdentifier("widget_toggle", "id", pkg);
            final int idPrev   = res.getIdentifier("widget_prev", "id", pkg);
            final int idNext   = res.getIdentifier("widget_next", "id", pkg);

            final boolean loaded = MediaNotification.isPlaybackLoaded();
            final boolean playing = loaded && MediaNotification.isPlaying();

            if (idTitle != 0) {
                String title = MediaNotification.currentTitle();
                if (!loaded || title.isEmpty()) {
                    int s = res.getIdentifier("widget_nothing_playing", "string", pkg);
                    title = s != 0 ? ctx.getString(s) : "Nothing playing";
                }
                rv.setTextViewText(idTitle, title);
            }
            if (idArtist != 0) {
                rv.setTextViewText(idArtist, loaded ? MediaNotification.currentArtist() : "");
            }
            if (idArt != 0) {
                Bitmap art = MediaNotification.currentArt();
                if (art != null) {
                    rv.setImageViewBitmap(idArt, art);
                } else {
                    int fallback = res.getIdentifier("ic_launcher", "mipmap", pkg);
                    if (fallback != 0) rv.setImageViewResource(idArt, fallback);
                }
            }
            if (idToggle != 0) {
                rv.setImageViewResource(idToggle, playing
                    ? android.R.drawable.ic_media_pause
                    : android.R.drawable.ic_media_play);
            }

            // Transport buttons while something is loaded; tapping the widget
            // otherwise just opens the app, which is the only useful thing left.
            if (loaded) {
                bind(ctx, rv, idPrev, CODE_PREVIOUS);
                bind(ctx, rv, idToggle, CODE_TOGGLE);
                bind(ctx, rv, idNext, CODE_NEXT);
            } else {
                Intent launch = ctx.getPackageManager().getLaunchIntentForPackage(pkg);
                if (launch != null) {
                    PendingIntent pi = PendingIntent.getActivity(ctx, 200, launch, piFlags());
                    if (idToggle != 0) rv.setOnClickPendingIntent(idToggle, pi);
                }
            }

            mgr.updateAppWidget(widgetId, rv);
        } catch (Throwable t) {
            Log.w(TAG, "render failed", t);
        }
    }

    private static void bind(Context ctx, RemoteViews rv, int viewId, int code) {
        if (viewId == 0) return;
        Intent i = new Intent("org.VitaPlex.app.MEDIA_ACTION")
            .setPackage(ctx.getPackageName())
            .putExtra("code", code);
        // Distinct request codes so the three buttons don't collapse into one
        // PendingIntent — the extras alone are not part of the identity.
        rv.setOnClickPendingIntent(viewId,
            PendingIntent.getBroadcast(ctx, 300 + code, i, piFlags()));
    }

    private static int piFlags() {
        int f = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) f |= PendingIntent.FLAG_IMMUTABLE;
        return f;
    }
}
