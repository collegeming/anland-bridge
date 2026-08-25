package com.anland.consumer;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import java.io.File;
import java.io.FileNotFoundException;

/**
 * Minimal {@link ContentProvider} that serves one file — the clipboard image
 * the bridge wrote to {@code cacheDir/clip/bridge_clip.png} — so the app can
 * share it on the system clipboard without any androidx dependency.
 *
 * The app deliberately has zero library dependencies; a full androidx
 * FileProvider would pull the androidx.core dependency in. This provider only
 * answers {@link #openFile} for the single known clipboard path, and is not
 * exported (only our own process can mint the URI, and we grant the consumer
 * read access at setPrimaryClip time).
 */
public final class ClipImageProvider extends ContentProvider {

    private static final String AUTHORITY_PATH = "clip";
    private static final String FILE_NAME = "bridge_clip.png";

    /** URI for the current clipboard image (served from {@code cacheDir/clip}). */
    public static Uri uriFor(Context context) {
        return new Uri.Builder()
                .scheme("content")
                .authority(context.getPackageName() + ".clip")
                .appendPath(AUTHORITY_PATH)
                .appendPath(FILE_NAME)
                .build();
    }

    private File resolve(Uri uri) throws FileNotFoundException {
        java.util.List<String> segments = uri.getPathSegments();
        if (segments.isEmpty() || !"clip".equals(segments.get(0))
                || uri.getLastPathSegment() == null
                || !FILE_NAME.equals(uri.getLastPathSegment())) {
            throw new FileNotFoundException("Unknown clipboard image URI: " + uri);
        }
        File file = new File(new File(getContext().getCacheDir(), "clip"), FILE_NAME);
        if (!file.isFile()) {
            throw new FileNotFoundException("Clipboard image not present: " + file);
        }
        return file;
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) {
            throw new FileNotFoundException("Clipboard image is read-only");
        }
        return ParcelFileDescriptor.open(resolve(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public String getType(Uri uri) {
        return "image/png";
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
                        String[] selectionArgs, String sortOrder) {
        return null;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        return 0;
    }
}
