package org.VitaPlex.app;

import android.app.SearchManager;
import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Intent;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.util.Log;

/**
 * Android TV global search.
 *
 * The TV home screen's search (and the remote's microphone) queries every app
 * that declares itself searchable, so an app that doesn't is invisible to it —
 * typing a film's name returned results from everything installed except the
 * app holding the library.
 *
 * The system binds this provider from its own process on a binder thread and
 * blocks on the answer, which suits a synchronous Plex search: native does the
 * request and hands back flat rows. It reuses the media browser's cold-start
 * bootstrap, so a search works with the app never having been opened.
 *
 * Each row carries a vitaplex://media/<key> VIEW intent — the same deep link
 * the home-screen channels use — so picking a result opens that item's page.
 */
public final class SearchSuggestionProvider extends ContentProvider {
    private static final String TAG = "VitaPlexSearch";

    /** Fields per row in the flat array native returns. */
    private static final int STRIDE = 4;   // key, title, subtitle, art uri

    private static final String[] COLUMNS = {
        "_id",
        SearchManager.SUGGEST_COLUMN_TEXT_1,
        SearchManager.SUGGEST_COLUMN_TEXT_2,
        SearchManager.SUGGEST_COLUMN_RESULT_CARD_IMAGE,
        SearchManager.SUGGEST_COLUMN_INTENT_ACTION,
        SearchManager.SUGGEST_COLUMN_INTENT_DATA,
    };

    private static native String[] nativeSearchSuggestions(String query);

    @Override
    public boolean onCreate() {
        return true;   // native comes up lazily, on the first query
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
                        String[] selectionArgs, String sortOrder) {
        MatrixCursor cursor = new MatrixCursor(COLUMNS);
        try {
            // The query is the last path segment for the suggestion URI, and
            // the system also sends a bare .../search_suggest_query with the
            // text in selectionArgs on some builds.
            String query = uri.getLastPathSegment();
            if (SearchManager.SUGGEST_URI_PATH_QUERY.equals(query)) {
                query = (selectionArgs != null && selectionArgs.length > 0)
                        ? selectionArgs[0] : null;
            }
            if (query == null || query.trim().isEmpty()) return cursor;

            if (!LibraryBrowserService.ensureNativeForSearch(getContext())) return cursor;

            String[] rows = nativeSearchSuggestions(query);
            if (rows == null) return cursor;
            for (int i = 0; i + STRIDE <= rows.length; i += STRIDE) {
                cursor.addRow(new Object[] {
                    i / STRIDE,
                    rows[i + 1],
                    rows[i + 2],
                    rows[i + 3],
                    Intent.ACTION_VIEW,
                    "vitaplex://media/" + rows[i],
                });
            }
        } catch (Throwable t) {
            Log.w(TAG, "suggestion query failed", t);
        }
        return cursor;
    }

    @Override public String getType(Uri uri) { return SearchManager.SUGGEST_MIME_TYPE; }
    @Override public Uri insert(Uri uri, ContentValues values) { return null; }
    @Override public int delete(Uri uri, String s, String[] a) { return 0; }
    @Override public int update(Uri uri, ContentValues v, String s, String[] a) { return 0; }
}
