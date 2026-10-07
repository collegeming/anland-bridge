package com.anland.consumer;

import android.view.KeyEvent;

/**
 * Turns an Android key into the evdev code the desktop expects.
 *
 * <p>Pure: it reads only the two numbers a key event carries, so the whole
 * resolution order can be unit tested on the JVM. It is deliberately separated
 * from the sending, because the two happen at different times — a key is
 * resolved once, when it goes down, and the release reuses that result rather
 * than resolving again from metadata that may since have changed.
 */
final class KeyResolver {
    /** linux/input-event-codes.h: KEY_BACK, the browser-back key. */
    static final int EVDEV_BROWSER_BACK = 158;

    private KeyResolver() {
    }

    /**
     * @param convertBackToEscape tablet keyboards that report their physical Esc
     *                            as Android Back; only the accessibility path
     *                            asks for this, so plain Android Back elsewhere
     *                            keeps its own meaning
     * @return the evdev code, or -1 when this key has none
     */
    static int resolveEvdevCode(int keyCode, int scanCode, boolean convertBackToEscape) {
        int evdev = -1;

        if (convertBackToEscape
                && (keyCode == KeyEvent.KEYCODE_BACK || scanCode == EVDEV_BROWSER_BACK))
            evdev = KeyCodeMapper.getScanCode(KeyEvent.KEYCODE_ESCAPE);

        // Reserved Android keys may carry vendor scan codes that Linux does not
        // recognize, so prefer their explicit evdev mapping.
        if (evdev == -1 && shouldPreferMappedKey(keyCode))
            evdev = KeyCodeMapper.getScanCode(keyCode);

        if (evdev == -1 && scanCode != 0)
            evdev = scanCode;

        if (evdev == -1)
            evdev = KeyCodeMapper.getScanCode(keyCode);

        return evdev;
    }

    /** Keys whose vendor scan code is less trustworthy than their mapping. */
    static boolean shouldPreferMappedKey(int keyCode) {
        return keyCode == KeyEvent.KEYCODE_META_LEFT
                || keyCode == KeyEvent.KEYCODE_META_RIGHT
                || keyCode == KeyEvent.KEYCODE_SEARCH
                || keyCode == KeyEvent.KEYCODE_ASSIST
                || (keyCode >= KeyEvent.KEYCODE_F13 && keyCode <= KeyEvent.KEYCODE_F24);
    }
}
