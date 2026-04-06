package org.VitaPlex.app;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.app.UiModeManager;
import android.database.ContentObserver;
import android.graphics.PixelFormat;
import android.os.Bundle;
import android.os.Handler;
import android.provider.Settings;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.Surface;

import org.libsdl.app.BorealisHandler;
import org.libsdl.app.PlatformUtils;
import org.libsdl.app.SDLActivity;

public class VitaPlexActivity extends SDLActivity
{
    private static final String TAG = "VitaPlex";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mSurface.getHolder().setFormat(PixelFormat.RGBA_8888);

        PlatformUtils.borealisHandler = new BorealisHandler();
        _setAppScreenBrightness(_getSystemScreenBrightness());
        getContentResolver().registerContentObserver(
                Settings.System.getUriFor(Settings.System.SCREEN_BRIGHTNESS),
                true,
                brightnessObserver);
    }

    private void _setAppScreenBrightness(float value) {
        PlatformUtils.setAppScreenBrightness(this, value);
    }

    private float _getSystemScreenBrightness() {
        return PlatformUtils.getSystemScreenBrightness(this);
    }

    private final ContentObserver brightnessObserver = new ContentObserver(new Handler()) {
        @Override
        public void onChange(boolean selfChange) {
            _setAppScreenBrightness(_getSystemScreenBrightness());
        }
    };

    public static Surface getMpvSurface() {
        if (mSurface == null) {
            return null;
        }
        return mSurface.getHolder().getSurface();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();

        getContentResolver().unregisterContentObserver(brightnessObserver);

        // Android does not recommend using exit(0) directly,
        // but borealis heavily uses static variables,
        // which can cause some problems when reloading the program.

        // In SDL3, we can use SDL_HINT_ANDROID_ALLOW_RECREATE_ACTIVITY to control the behavior

        // In SDL2, Force exit of the app.
        System.exit(0);
    }

    @Override
    protected String[] getLibraries() {
        // Load SDL2 and borealis demo app
        return new String[] {
                "curl",
                "SDL2",
                "VitaPlex"
        };
    }

    /**
     * Check if a key event comes from a basic TV remote (SOURCE_DPAD but not
     * a full gamepad).  TV remotes are detected as joysticks by SDL because
     * they report SOURCE_DPAD, but their KEYCODE_BACK should map to BUTTON_B
     * (navigation back), not BUTTON_BACK (gamepad select).  By returning true
     * here we allow dispatchKeyEvent to reroute those keys through the
     * keyboard path where borealis maps them correctly.
     */
    private boolean isTvRemoteEvent(KeyEvent event) {
        InputDevice device = event.getDevice();
        if (device == null) return false;
        int sources = device.getSources();
        boolean isDpad = (sources & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD;
        boolean isGamepad = (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD;
        boolean isJoystick = (sources & InputDevice.SOURCE_CLASS_JOYSTICK) != 0;
        // A TV remote has SOURCE_DPAD but not SOURCE_GAMEPAD or SOURCE_JOYSTICK
        return isDpad && !isGamepad && !isJoystick;
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (SDLActivity.mBrokenLibraries) {
            return false;
        }

        int keyCode = event.getKeyCode();

        // Let Android handle volume/camera/zoom as usual
        if (keyCode == KeyEvent.KEYCODE_VOLUME_DOWN ||
            keyCode == KeyEvent.KEYCODE_VOLUME_UP ||
            keyCode == KeyEvent.KEYCODE_CAMERA ||
            keyCode == KeyEvent.KEYCODE_ZOOM_IN ||
            keyCode == KeyEvent.KEYCODE_ZOOM_OUT) {
            return false;
        }

        // KEYCODE_BACK and DPAD_CENTER must always go through the keyboard
        // path regardless of input source. Without this, DPAD-source remotes
        // route them through SDL's joystick handler where BACK becomes
        // BUTTON_BACK (wrong, should be BUTTON_B) and DPAD_CENTER may not
        // map to BUTTON_A at all. Forcing the keyboard path ensures:
        //   BACK → SDL_SCANCODE_AC_BACK → BUTTON_B (navigation back)
        //   DPAD_CENTER → SDL_SCANCODE_RETURN → BUTTON_A (confirm/select)
        if (keyCode == KeyEvent.KEYCODE_BACK ||
            keyCode == KeyEvent.KEYCODE_DPAD_CENTER) {
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                SDLActivity.onNativeKeyDown(keyCode);
                return true;
            } else if (event.getAction() == KeyEvent.ACTION_UP) {
                SDLActivity.onNativeKeyUp(keyCode);
                return true;
            }
        }

        // For TV remote events, bypass the joystick handler for keys that need
        // keyboard-path mapping or translation to mapped keycodes.
        if (isTvRemoteEvent(event)) {
            int translatedKey = keyCode;
            switch (keyCode) {
                case KeyEvent.KEYCODE_ENTER:
                case KeyEvent.KEYCODE_MENU:
                    // Pass through as-is — SDL maps them to scancodes
                    // that borealis already handles (RETURN, MENU)
                    break;
                case KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE:
                case KeyEvent.KEYCODE_MEDIA_PLAY:
                case KeyEvent.KEYCODE_MEDIA_PAUSE:
                case KeyEvent.KEYCODE_MEDIA_STOP:
                    // Map to ENTER → SDL_SCANCODE_RETURN → BUTTON_A (play/pause)
                    translatedKey = KeyEvent.KEYCODE_ENTER;
                    break;
                case KeyEvent.KEYCODE_MEDIA_REWIND:
                case KeyEvent.KEYCODE_MEDIA_PREVIOUS:
                    // Map to DPAD_LEFT → SDL_SCANCODE_LEFT → BUTTON_LEFT (seek back)
                    translatedKey = KeyEvent.KEYCODE_DPAD_LEFT;
                    break;
                case KeyEvent.KEYCODE_MEDIA_FAST_FORWARD:
                case KeyEvent.KEYCODE_MEDIA_NEXT:
                    // Map to DPAD_RIGHT → SDL_SCANCODE_RIGHT → BUTTON_RIGHT (seek fwd)
                    translatedKey = KeyEvent.KEYCODE_DPAD_RIGHT;
                    break;
                default:
                    // D-pad and other keys go through normal SDL dispatch
                    return super.dispatchKeyEvent(event);
            }
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                SDLActivity.onNativeKeyDown(translatedKey);
                return true;
            } else if (event.getAction() == KeyEvent.ACTION_UP) {
                SDLActivity.onNativeKeyUp(translatedKey);
                return true;
            }
        }

        return super.dispatchKeyEvent(event);
    }

    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        // Keep Android behavior simple and consistent: allow both portrait and landscape.
        // This matches users' expectation on phones/tablets/TVs and supports natural rotation.
        int req = ActivityInfo.SCREEN_ORIENTATION_FULL_USER;
        Log.v(TAG, "setOrientationBis(): forcing FULL_USER (w=" + w + ", h=" + h
                + ", resizable=" + resizable + ", hint=" + hint + ")");
        setRequestedOrientation(req);
    }

}
