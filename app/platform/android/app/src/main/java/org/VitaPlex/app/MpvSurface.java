/*
 * VitaPlex — Android direct-surface MpvSurface view.
 *
 * Stage 2 of the dual-surface integration (see
 * docs/android-direct-surface-playback.md). This view gives mpv its own
 * Android Surface so it can render directly to the display via vo=gpu
 * instead of the FBO/NanoVG composite that drowns weak TV SoCs.
 *
 * Stage 2 just defines the class — it exists, the lifecycle callbacks
 * forward to the JNI bridge in src/platform/android_mpv_surface.cpp,
 * and the native methods resolve against libmain.so (loaded by
 * SDLActivity). Nothing instantiates this view yet — the layout
 * integration is Stage 3, and the VO-path flip that actually starts
 * using it is Stage 4. Until then the existing libmpv/FBO render path
 * still drives playback.
 *
 * Lifecycle order on surfaceDestroyed mirrors mpv-android's
 * BaseMPVView: tear the VO down (vo=null) before the JNI layer
 * releases the Surface global ref, so mpv doesn't render into a
 * freed window. See nativeDetachSurface in android_mpv_surface.cpp.
 */

package org.VitaPlex.app;

import android.content.Context;
import android.util.AttributeSet;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

public class MpvSurface extends SurfaceView implements SurfaceHolder.Callback {
    private static final String TAG = "MpvSurface";

    // libmain.so is already loaded by SDLActivity.loadLibraries(); we
    // just declare the natives. JNI symbol names match the functions
    // in src/platform/android_mpv_surface.cpp — don't rename either
    // side without updating both.
    public static native void nativeAttachSurface(Surface surface);
    public static native void nativeDetachSurface();
    public static native void nativeSetSurfaceSize(int width, int height);

    public MpvSurface(Context context) {
        super(context);
        init();
    }

    public MpvSurface(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    private void init() {
        getHolder().addCallback(this);
        // Stage 3 will flip this view to TRANSLUCENT pixel format and
        // call setZOrderMediaOverlay(false) so the SDL surface sits on
        // top with alpha. Stage 2 leaves the format defaults alone so
        // the class can be instantiated without affecting anything.
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        Log.i(TAG, "surfaceCreated -> attach");
        nativeAttachSurface(holder.getSurface());
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        Log.i(TAG, "surfaceChanged " + width + "x" + height + " fmt=" + format);
        nativeSetSurfaceSize(width, height);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        Log.i(TAG, "surfaceDestroyed -> detach (vo=null first)");
        // nativeDetachSurface sets vo=null *before* deleting the
        // Surface global ref mpv was reading as wid, mirroring
        // mpv-android's BaseMPVView teardown ordering. There's an
        // inherent race here that mpv-android also calls out — setting
        // a property doesn't synchronously wait for VO deinit — but
        // it's the best we can do at the JNI/mpv boundary.
        nativeDetachSurface();
    }
}
