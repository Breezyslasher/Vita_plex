package org.VitaPlex.app;

import android.annotation.TargetApi;
import android.content.Intent;
import android.graphics.drawable.Icon;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;
import android.util.Log;

/**
 * Quick-settings tile for music playback.
 *
 * Pulling down the shade to reach the media notification means finding it among
 * everything else that posted one; a tile is one tap from anywhere, including
 * over full-screen apps. It toggles play/pause when something is playing and
 * opens the app when nothing is.
 *
 * The tile's own state mirrors playback, so the shade shows whether music is
 * running without expanding anything. Android only asks for that state while
 * the tile is visible (onStartListening / onStopListening), so there is nothing
 * to keep updated in the background.
 */
@TargetApi(24)
public final class MusicTileService extends TileService {
    private static final String TAG = "VitaPlexTile";

    // Transport codes shared with MediaNotification (keep in sync).
    private static final int CODE_TOGGLE = 1;

    @Override
    public void onStartListening() {
        super.onStartListening();
        refresh();
    }

    @Override
    public void onClick() {
        super.onClick();
        try {
            if (MediaNotification.isPlaybackLoaded()) {
                MediaNotification.sendTransport(this, CODE_TOGGLE);
                // The state change comes back through MediaNotification, but the
                // tile is only listening right now — update it directly so the
                // icon flips under the user's finger.
                refresh();
                return;
            }
            // Nothing loaded: the useful thing a tile can do is open the app.
            Intent launch = getPackageManager().getLaunchIntentForPackage(getPackageName());
            if (launch == null) return;
            launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivityAndCollapse(launch);
        } catch (Throwable t) {
            Log.w(TAG, "tile click failed", t);
        }
    }

    private void refresh() {
        try {
            Tile tile = getQsTile();
            if (tile == null) return;
            final boolean loaded = MediaNotification.isPlaybackLoaded();
            final boolean playing = loaded && MediaNotification.isPlaying();
            tile.setState(loaded ? Tile.STATE_ACTIVE : Tile.STATE_INACTIVE);
            tile.setIcon(Icon.createWithResource(this,
                playing ? android.R.drawable.ic_media_pause
                        : android.R.drawable.ic_media_play));
            // Looked up by name, like the rest of this hand-written Java, so
            // it carries no dependency on the generated R class.
            String label = MediaNotification.currentTitle();
            if (label.isEmpty()) {
                int id = getResources().getIdentifier("tile_music_label", "string", getPackageName());
                label = id != 0 ? getString(id) : "VitaPlex music";
            }
            tile.setLabel(label);
            tile.updateTile();
        } catch (Throwable t) {
            Log.w(TAG, "tile refresh failed", t);
        }
    }
}
