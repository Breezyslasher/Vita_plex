package org.VitaPlex.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.util.Log;

import androidx.core.app.NotificationCompat;

/**
 * Downloads shown in the Android notification shade — the native half of
 * vitaplex::shell.
 *
 * Two notifications, and they are deliberately different kinds:
 *
 *   PROGRESS_ID  An ongoing notification carrying a progress bar, posted while
 *                the queue drains. Ongoing means the user cannot swipe it away
 *                while work is still happening, which is the convention for
 *                this and also what stops a stray swipe from looking like a
 *                cancel. It is removed when the queue empties.
 *
 *                This one is also DownloadService's foreground notification —
 *                the service is what keeps the download running once the app
 *                is backgrounded — so both refer to the same PROGRESS_ID and
 *                only one notification ever appears.
 *
 *   DONE_ID      A one-shot summary when everything finishes. Dismissable,
 *                and it opens the app when tapped.
 *
 * Both sit on one IMPORTANCE_LOW channel, so neither makes a sound. That is a
 * choice, not an oversight: downloading an album finishes twenty tracks in a
 * row, and the app queues them as twenty items.
 */
public final class DownloadNotification {

    private static final String TAG = "VitaPlexDownloads";

    private static final String CHANNEL_ID   = "vitaplex_downloads";
    // Package-visible: DownloadService foregrounds this exact notification, so
    // the two must agree on the id or the shade shows both.
    static final int PROGRESS_ID = 0x56444C50;  // "VDLP"
    private static final int DONE_ID = 0x56444C44;  // "VDLD"

    private static boolean sChannelCreated = false;
    private static boolean sServiceStarted = false;
    // Latest reported progress, so the service can rebuild the same
    // notification when it foregrounds without being handed one.
    private static volatile float  sFraction = -1.0f;
    private static volatile String sTitle    = "Downloading";
    private static volatile String sDetail   = "";

    private DownloadNotification() {}

    /**
     * Post a one-shot notification.
     *
     * Not called notify(): Object.notify() is inherited and final, and while
     * an overload of it is legal, a static one that shadows it by name on
     * every subclass and reader is not worth the tidier name.
     */
    public static void showComplete(final String title, final String text) {
        try {
            Context ctx = VitaPlexActivity.getAppContext();
            if (ctx == null) return;
            NotificationManager nm = manager(ctx);
            if (nm == null) return;
            ensureChannel(ctx);

            NotificationCompat.Builder b =
                new NotificationCompat.Builder(ctx, CHANNEL_ID)
                    .setSmallIcon(android.R.drawable.stat_sys_download_done)
                    .setContentTitle(title)
                    .setContentText(text)
                    // The title alone is often longer than one line on a phone.
                    .setStyle(new NotificationCompat.BigTextStyle().bigText(text))
                    .setCategory(NotificationCompat.CATEGORY_STATUS)
                    .setPriority(NotificationCompat.PRIORITY_LOW)
                    .setAutoCancel(true)
                    .setContentIntent(openApp(ctx));

            nm.notify(DONE_ID, b.build());
        } catch (Throwable t) {
            // A revoked POST_NOTIFICATIONS grant lands here. Nothing to do
            // about it, and it must not disturb the download that triggered it.
            Log.w(TAG, "notify failed", t);
        }
    }

    /**
     * Show or hide the ongoing progress notification.
     *
     * fraction < 0 means "working, but the size is unknown" — a HLS download
     * whose segment count has not been established yet — and draws the
     * indeterminate bar rather than a misleading 0%.
     */
    public static void setProgress(final float fraction, final String title,
                                   final String detail, final boolean visible) {
        try {
            Context ctx = VitaPlexActivity.getAppContext();
            if (ctx == null) return;
            NotificationManager nm = manager(ctx);
            if (nm == null) return;

            if (!visible) {
                stopService(ctx);   // clears the notification with it
                nm.cancel(PROGRESS_ID);
                sFraction = -1.0f;
                sTitle    = "Downloading";
                sDetail   = "";
                return;
            }
            sFraction = fraction;
            if (title  != null && !title.isEmpty())  sTitle  = title;
            if (detail != null)                      sDetail = detail;
            ensureChannel(ctx);

            // First progress of a run: hand the work to a foreground service so
            // the system leaves the process alone once the app is backgrounded.
            // It posts the same notification itself; posting again here as well
            // would be harmless but pointless.
            if (!sServiceStarted) {
                startService(ctx);
                return;
            }

            Notification n = buildProgress(ctx);
            if (n != null) nm.notify(PROGRESS_ID, n);
        } catch (Throwable t) {
            Log.w(TAG, "setProgress failed", t);
        }
    }

    /**
     * The ongoing progress notification for the latest reported fraction.
     * DownloadService foregrounds this; setProgress re-posts it on each update.
     * Returns null if there is nothing in flight to describe.
     */
    static Notification buildProgress(Context ctx) {
        if (ctx == null) return null;
        ensureChannel(ctx);

        final float fraction = sFraction;
        final boolean indeterminate = fraction < 0.0f;
        final int pct = indeterminate
            ? 0
            : Math.max(0, Math.min(100, Math.round(fraction * 100.0f)));

        // Title is what is downloading, detail the live line under it —
        // "3 of 12 · 45% · 1.2 MB/s". A bare percentage told the user nothing
        // they could not already see from the bar.
        return new NotificationCompat.Builder(ctx, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.stat_sys_download)
            .setContentTitle(sTitle)
            .setContentText(sDetail.isEmpty() ? (indeterminate ? null : pct + "%") : sDetail)
            .setSubText(indeterminate ? null : pct + "%")
            .setProgress(100, pct, indeterminate)
            .setCategory(NotificationCompat.CATEGORY_PROGRESS)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setOngoing(true)
            // This is re-posted roughly once a second. Without it every repost
            // would re-alert.
            .setOnlyAlertOnce(true)
            .setContentIntent(openApp(ctx))
            .build();
    }

    private static void startService(Context ctx) {
        try {
            Intent i = new Intent(ctx, DownloadService.class);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                ctx.startForegroundService(i);
            } else {
                ctx.startService(i);
            }
            sServiceStarted = true;
        } catch (Throwable t) {
            // Android 12+ refuses this from the background. Fall back to a plain
            // notification: the download keeps going for as long as the process
            // does, which is exactly the old behaviour.
            Log.w(TAG, "startForegroundService failed", t);
            try {
                NotificationManager nm = manager(ctx);
                Notification n = buildProgress(ctx);
                if (nm != null && n != null) nm.notify(PROGRESS_ID, n);
            } catch (Throwable ignored) {}
        }
    }

    private static void stopService(Context ctx) {
        if (!sServiceStarted) return;
        sServiceStarted = false;
        try {
            ctx.stopService(new Intent(ctx, DownloadService.class));
        } catch (Throwable t) {
            Log.w(TAG, "stopService failed", t);
        }
    }

    private static NotificationManager manager(Context ctx) {
        return (NotificationManager) ctx.getSystemService(Context.NOTIFICATION_SERVICE);
    }

    private static void ensureChannel(Context ctx) {
        if (sChannelCreated || Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return;
        NotificationManager nm = manager(ctx);
        if (nm == null) return;
        NotificationChannel ch = new NotificationChannel(
            CHANNEL_ID, "Downloads", NotificationManager.IMPORTANCE_LOW);
        ch.setDescription("Progress and completion of offline downloads");
        ch.setShowBadge(false);
        nm.createNotificationChannel(ch);
        sChannelCreated = true;
    }

    /** Tapping either notification brings the app back to the front. */
    private static PendingIntent openApp(Context ctx) {
        Intent i = new Intent(ctx, VitaPlexActivity.class);
        i.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            // Required from API 31, and harmless before it.
            flags |= PendingIntent.FLAG_IMMUTABLE;
        }
        return PendingIntent.getActivity(ctx, 0, i, flags);
    }
}
