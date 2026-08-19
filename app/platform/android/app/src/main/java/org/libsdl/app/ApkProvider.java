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
 * Serves exactly one file — the downloaded update APK — to the system
 * package installer. file:// URIs throw FileUriExposedException on
 * API 24+, and the project deliberately carries no androidx dependency,
 * so this stands in for androidx FileProvider. It refuses every path
 * except {filesDir}/VitaPlex/update.apk: nothing else in the sandbox is
 * reachable through it.
 */
public class ApkProvider extends ContentProvider {

    private File updateFile() {
        return new File(new File(getContext().getFilesDir(), "VitaPlex"), "update.apk");
    }

    private File resolve(Uri uri) throws FileNotFoundException {
        if (!"/update.apk".equals(uri.getPath()))
            throw new FileNotFoundException("not served: " + uri);
        File f = updateFile();
        if (!f.isFile())
            throw new FileNotFoundException("no downloaded update at " + f);
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
        return "application/vnd.android.package-archive";
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
