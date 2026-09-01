package org.VitaPlex.app;

import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.util.Log;

import androidx.tvprovider.media.tv.Channel;
import androidx.tvprovider.media.tv.ChannelLogoUtils;
import androidx.tvprovider.media.tv.PreviewProgram;
import androidx.tvprovider.media.tv.TvContractCompat;
import androidx.tvprovider.media.tv.WatchNextProgram;

import java.util.ArrayList;
import java.util.List;

/**
 * Android TV home-screen integration.
 *
 * The TV home screen is built out of app channels, so an app that publishes
 * none is a tile you have to open before it shows you anything. VitaPlex
 * already knows what you were watching and what arrived recently; this puts
 * both on the home screen.
 *
 * Two different surfaces, with different rules:
 *
 *  - The "Watch Next" row is system-owned and shared by every app. A partly
 *    watched episode belongs there with its resume position, and the system
 *    orders and evicts entries itself. Nothing has to be created first.
 *  - An app channel is ours, has to be created once, and only appears after
 *    the user adds it from the home screen's channel list. Programs are
 *    replaced wholesale on each refresh, because a stale row is worse than a
 *    short one.
 *
 * All of it is best-effort: TV providers are absent on phones, and a device
 * that has none simply gets nothing. Called from native (HomeTab) once the
 * Continue Watching and Recently Added rows have loaded.
 */
public final class TvChannels {
    private static final String TAG = "VitaPlexTvChannels";

    // Stored so the channel is updated rather than duplicated on each launch.
    private static final String PREFS = "vitaplex_tv_channels";
    private static final String KEY_CHANNEL_ID = "channelId";

    private TvChannels() {}

    /** Whether this device has the TV provider at all. */
    private static boolean available(Context ctx) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return false;  // preview programs are API 26
        if (ctx == null) return false;
        try {
            return ctx.getPackageManager().resolveContentProvider(
                    TvContractCompat.AUTHORITY, 0) != null;
        } catch (Throwable t) {
            return false;
        }
    }

    /**
     * Publish the "Continue Watching" items to the system Watch Next row.
     *
     * Called from native with parallel arrays. positionMs/durationMs drive the
     * progress bar and the resume point; an item with no progress is published
     * as NEXT rather than CONTINUE, which is what the row means by "you have
     * not started this yet".
     */
    public static void publishWatchNext(final String[] mediaIds, final String[] titles,
                                        final String[] descriptions, final String[] artUris,
                                        final long[] positionMs, final long[] durationMs,
                                        final int[] episodeInfo) {
        final Context ctx = VitaPlexActivity.getAppContext();
        if (!available(ctx) || mediaIds == null) return;
        new Thread(new Runnable() {
            @Override public void run() {
                try {
                    publishWatchNextInner(ctx, mediaIds, titles, descriptions, artUris,
                                          positionMs, durationMs, episodeInfo);
                } catch (Throwable t) {
                    Log.w(TAG, "publishWatchNext failed", t);
                }
            }
        }).start();
    }

    private static void publishWatchNextInner(Context ctx, String[] mediaIds, String[] titles,
                                              String[] descriptions, String[] artUris,
                                              long[] positionMs, long[] durationMs,
                                              int[] episodeInfo) {
        // Our previous entries, so an item that has since been finished (or
        // dropped off the server's list) doesn't linger on the home screen.
        List<Long> stale = existingWatchNextIds(ctx);

        for (int i = 0; i < mediaIds.length; i++) {
            if (mediaIds[i] == null || mediaIds[i].isEmpty()) continue;
            final long pos = at(positionMs, i);
            final long dur = at(durationMs, i);

            WatchNextProgram.Builder b = new WatchNextProgram.Builder()
                .setType(TvContractCompat.WatchNextPrograms.TYPE_TV_EPISODE)
                .setWatchNextType(pos > 0
                    ? TvContractCompat.WatchNextPrograms.WATCH_NEXT_TYPE_CONTINUE
                    : TvContractCompat.WatchNextPrograms.WATCH_NEXT_TYPE_NEXT)
                .setLastEngagementTimeUtcMillis(System.currentTimeMillis())
                .setTitle(str(titles, i))
                .setDescription(str(descriptions, i))
                .setIntentUri(Uri.parse("vitaplex://media/" + mediaIds[i]))
                .setInternalProviderId(mediaIds[i]);

            if (dur > 0) b.setDurationMillis((int) Math.min(dur, Integer.MAX_VALUE));
            if (pos > 0) b.setLastPlaybackPositionMillis((int) Math.min(pos, Integer.MAX_VALUE));
            String art = str(artUris, i);
            if (!art.isEmpty()) {
                b.setPosterArtUri(Uri.parse(art));
                // 16:9 stills for episodes; the row crops anything else.
                b.setPosterArtAspectRatio(TvContractCompat.PreviewPrograms.ASPECT_RATIO_16_9);
            }
            // Season and episode, packed two ints per item by the caller.
            int season = at(episodeInfo, i * 2), episode = at(episodeInfo, i * 2 + 1);
            if (season > 0) b.setSeasonNumber(season);
            if (episode > 0) b.setEpisodeNumber(episode);

            Long existing = findWatchNext(ctx, mediaIds[i]);
            ContentValues values = b.build().toContentValues();
            if (existing != null) {
                ctx.getContentResolver().update(
                    TvContractCompat.buildWatchNextProgramUri(existing), values, null, null);
                stale.remove(existing);
            } else {
                ctx.getContentResolver().insert(
                    TvContractCompat.WatchNextPrograms.CONTENT_URI, values);
            }
        }

        for (Long id : stale) {
            ctx.getContentResolver().delete(
                TvContractCompat.buildWatchNextProgramUri(id), null, null);
        }
    }

    /** Row ids of every Watch Next entry this app owns. */
    private static List<Long> existingWatchNextIds(Context ctx) {
        List<Long> ids = new ArrayList<>();
        Cursor c = null;
        try {
            c = ctx.getContentResolver().query(
                TvContractCompat.WatchNextPrograms.CONTENT_URI,
                WatchNextProgram.PROJECTION, null, null, null);
            while (c != null && c.moveToNext()) {
                WatchNextProgram p = WatchNextProgram.fromCursor(c);
                // Only ours: the row is shared with every other app on the box.
                if (p.getInternalProviderId() != null) ids.add(p.getId());
            }
        } catch (Throwable t) {
            Log.w(TAG, "watch-next scan failed", t);
        } finally {
            if (c != null) c.close();
        }
        return ids;
    }

    private static Long findWatchNext(Context ctx, String mediaId) {
        Cursor c = null;
        try {
            c = ctx.getContentResolver().query(
                TvContractCompat.WatchNextPrograms.CONTENT_URI,
                WatchNextProgram.PROJECTION, null, null, null);
            while (c != null && c.moveToNext()) {
                WatchNextProgram p = WatchNextProgram.fromCursor(c);
                if (mediaId.equals(p.getInternalProviderId())) return p.getId();
            }
        } catch (Throwable t) {
            Log.w(TAG, "watch-next lookup failed", t);
        } finally {
            if (c != null) c.close();
        }
        return null;
    }

    /**
     * Publish a "Recently Added" app channel.
     *
     * The channel has to exist before the user can add it from the home
     * screen's channel list, so it is created on first run and updated after.
     * Programs are replaced wholesale: the list is short, and reconciling it
     * item by item would leave rows the server has since dropped.
     */
    public static void publishRecentlyAdded(final String[] mediaIds, final String[] titles,
                                            final String[] descriptions, final String[] artUris) {
        final Context ctx = VitaPlexActivity.getAppContext();
        if (!available(ctx) || mediaIds == null || mediaIds.length == 0) return;
        new Thread(new Runnable() {
            @Override public void run() {
                try {
                    publishChannelInner(ctx, mediaIds, titles, descriptions, artUris);
                } catch (Throwable t) {
                    Log.w(TAG, "publishRecentlyAdded failed", t);
                }
            }
        }).start();
    }

    private static void publishChannelInner(Context ctx, String[] mediaIds, String[] titles,
                                            String[] descriptions, String[] artUris) {
        android.content.SharedPreferences prefs =
            ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        long channelId = prefs.getLong(KEY_CHANNEL_ID, -1);

        Channel channel = new Channel.Builder()
            .setType(TvContractCompat.Channels.TYPE_PREVIEW)
            .setDisplayName("Recently Added")
            .setAppLinkIntentUri(Uri.parse("vitaplex://home"))
            .build();

        if (channelId < 0) {
            Uri created = ctx.getContentResolver().insert(
                TvContractCompat.Channels.CONTENT_URI, channel.toContentValues());
            if (created == null) return;
            channelId = ContentUris.parseId(created);
            prefs.edit().putLong(KEY_CHANNEL_ID, channelId).apply();
            // The logo is what identifies the row on the home screen, and it is
            // only accepted after the channel exists.
            try {
                android.graphics.Bitmap logo = android.graphics.BitmapFactory.decodeResource(
                    ctx.getResources(),
                    ctx.getResources().getIdentifier("banner", "drawable", ctx.getPackageName()));
                if (logo != null) ChannelLogoUtils.storeChannelLogo(ctx, channelId, logo);
            } catch (Throwable t) {
                Log.w(TAG, "channel logo failed", t);
            }
            // Only ever done once: making ourselves browsable again after the
            // user removed the row would be putting it back uninvited.
            TvContractCompat.requestChannelBrowsable(ctx, channelId);
        } else {
            ctx.getContentResolver().update(
                TvContractCompat.buildChannelUri(channelId), channel.toContentValues(), null, null);
        }

        ctx.getContentResolver().delete(
            TvContractCompat.buildPreviewProgramsUriForChannel(channelId), null, null);

        for (int i = 0; i < mediaIds.length; i++) {
            if (mediaIds[i] == null || mediaIds[i].isEmpty()) continue;
            PreviewProgram.Builder b = new PreviewProgram.Builder()
                .setChannelId(channelId)
                .setType(TvContractCompat.PreviewPrograms.TYPE_MOVIE)
                .setTitle(str(titles, i))
                .setDescription(str(descriptions, i))
                .setIntentUri(Uri.parse("vitaplex://media/" + mediaIds[i]))
                .setInternalProviderId(mediaIds[i]);
            String art = str(artUris, i);
            if (!art.isEmpty()) {
                b.setPosterArtUri(Uri.parse(art));
                b.setPosterArtAspectRatio(TvContractCompat.PreviewPrograms.ASPECT_RATIO_MOVIE_POSTER);
            }
            ctx.getContentResolver().insert(
                TvContractCompat.PreviewPrograms.CONTENT_URI, b.build().toContentValues());
        }
        Log.i(TAG, "published " + mediaIds.length + " programs to channel " + channelId);
    }

    private static String str(String[] a, int i) {
        return (a != null && i < a.length && a[i] != null) ? a[i] : "";
    }

    private static long at(long[] a, int i) {
        return (a != null && i < a.length) ? a[i] : 0;
    }

    private static int at(int[] a, int i) {
        return (a != null && i < a.length) ? a[i] : 0;
    }
}
