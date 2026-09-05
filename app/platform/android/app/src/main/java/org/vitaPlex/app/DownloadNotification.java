package org.VitaPlex.app;

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
    private static final int    PROGRESS_ID  = 0x56444C50;  // "VDLP"
    private static final int    DONE_ID      = 0x56444C44;  // "VDLD"

    private static boolean sChannelCreated = false;

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
    public static void setProgress(final float fraction, final boolean visible) {
        try {
            Context ctx = VitaPlexActivity.getAppContext();
            if (ctx == null) return;
            NotificationManager nm = manager(ctx);
            if (nm == null) return;

            if (!visible) {
                nm.cancel(PROGRESS_ID);
                return;
            }
            ensureChannel(ctx);

            final boolean indeterminate = fraction < 0.0f;
            final int pct = indeterminate
                ? 0
                : Math.max(0, Math.min(100, Math.round(fraction * 100.0f)));

            NotificationCompat.Builder b =
                new NotificationCompat.Builder(ctx, CHANNEL_ID)
                    .setSmallIcon(android.R.drawable.stat_sys_download)
                    .setContentTitle("Downloading")
                    .setContentText(indeterminate ? null : pct + "%")
                    .setProgress(100, pct, indeterminate)
                    .setCategory(NotificationCompat.CATEGORY_PROGRESS)
                    .setPriority(NotificationCompat.PRIORITY_LOW)
                    .setOngoing(true)
                    // This is re-posted roughly once a second. Without it every
                    // repost would re-alert.
                    .setOnlyAlertOnce(true)
                    .setContentIntent(openApp(ctx));

            nm.notify(PROGRESS_ID, b.build());
        } catch (Throwable t) {
            Log.w(TAG, "setProgress failed", t);
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
