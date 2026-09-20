package org.libsdl.app;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

/**
 * Serves a fixed handful of files out of {filesDir}/VitaPlex: the
 * downloaded update APK, to the system package installer, and the two log
 * files, to whatever the user picks from the share sheet. file:// URIs throw
 * FileUriExposedException on API 24+, and the project deliberately carries no
 * androidx dependency, so this stands in for androidx FileProvider.
 *
 * The allow-list is the whole security model. A provider that resolved an
 * arbitrary path would be a way out of the sandbox, so resolve() takes a bare
 * filename, refuses anything containing a separator, and matches it against
 * SERVED by equality — nothing else in filesDir is reachable through it.
 */
public class ApkProvider extends ContentProvider {

    private static final String DIR = "VitaPlex";

    private static final String[] SERVED = {
        "update.apk",
        "vitaplex.log",
        "vitaplex.prev.log",
    };

    private File resolve(Uri uri) throws FileNotFoundException {
        String path = uri.getPath();
        if (path == null)
            throw new FileNotFoundException("not served: " + uri);
        if (path.startsWith("/")) path = path.substring(1);
        // No separators, so "..", nested paths and absolute paths cannot
        // resolve to anything outside the directory below.
        if (path.indexOf('/') >= 0 || path.indexOf('\\') >= 0)
            throw new FileNotFoundException("not served: " + uri);

        boolean served = false;
        for (String s : SERVED) {
            if (s.equals(path)) { served = true; break; }
        }
        if (!served)
            throw new FileNotFoundException("not served: " + uri);

        File f = new File(new File(getContext().getFilesDir(), DIR), path);
        if (!f.isFile())
            throw new FileNotFoundException("no such file: " + f);
        return f;
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        return ParcelFileDescriptor.open(resolve(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public String getType(Uri uri) {
        String path = uri.getPath();
        if (path != null && path.endsWith(".apk"))
            return "application/vnd.android.package-archive";
        // The installer only ever asks about the APK; everything else here is
        // a log, and text/plain is what makes mail and chat apps accept it.
        return "text/plain";
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
                        String[] selectionArgs, String sortOrder) {
        // The installer asks for display name and size before opening.
        File f;
        try {
            f = resolve(uri);
        } catch (FileNotFoundException e) {
            return null;
        }
        if (projection == null)
            projection = new String[] { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE };
        MatrixCursor cursor = new MatrixCursor(projection, 1);
        Object[] row = new Object[projection.length];
        for (int i = 0; i < projection.length; i++) {
            if (OpenableColumns.DISPLAY_NAME.equals(projection[i])) row[i] = f.getName();
            else if (OpenableColumns.SIZE.equals(projection[i]))    row[i] = f.length();
        }
        cursor.addRow(row);
        return cursor;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) { return null; }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) { return 0; }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) { return 0; }
}
