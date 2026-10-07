package com.anland.consumer;

import static org.junit.Assert.assertEquals;

import android.view.KeyEvent;

import org.junit.Test;

/**
 * The evdev resolution order, pinned. This used to live inline in
 * {@code forwardKeyToLinux}; it is separated out because the bus session
 * resolves once on the way down and reuses the result on the way up, so the
 * order has to be testable on its own.
 */
public class KeyResolverTest {
    @Test
    public void aNormalKeyFallsBackToItsMappingWhenAndroidHasNoScanCode() {
        // Some devices deliver no scan code at all; the mapping is the only way
        // to know what was pressed.
        assertEquals(30, KeyResolver.resolveEvdevCode(KeyEvent.KEYCODE_A, 0, false));
    }

    @Test
    public void aRealScanCodeWinsOverTheMapping() {
        assertEquals(99, KeyResolver.resolveEvdevCode(KeyEvent.KEYCODE_A, 99, false));
    }

    @Test
    public void reservedKeysPreferTheirMappingOverAVendorScanCode() {
        // F13 is a reserved Android key whose vendor scan code Linux would not
        // recognize, so the explicit mapping is used even though a code arrived.
        assertEquals(183, KeyResolver.resolveEvdevCode(KeyEvent.KEYCODE_F13, 999, false));
        assertEquals(125, KeyResolver.resolveEvdevCode(KeyEvent.KEYCODE_META_LEFT, 999, false));
    }

    @Test
    public void aKeyWithNeitherScanCodeNorMappingResolvesToNothing() {
        // Volume keys have no mapping, so a scan code is the only way through.
        assertEquals(114, KeyResolver.resolveEvdevCode(KeyEvent.KEYCODE_VOLUME_UP, 114, false));
        assertEquals(-1, KeyResolver.resolveEvdevCode(KeyEvent.KEYCODE_VOLUME_UP, 0, false));
    }

    @Test
    public void backBecomesEscapeOnlyWhenAskedFor() {
        // The plain Activity path leaves Back alone: it is Android's Back.
        assertEquals(158, KeyResolver.resolveEvdevCode(KeyEvent.KEYCODE_BACK, 158, false));
        assertEquals(-1, KeyResolver.resolveEvdevCode(KeyEvent.KEYCODE_BACK, 0, false));

        // The accessibility path converts it, because a tablet keyboard may
        // report its physical Esc as Back.
        assertEquals(1, KeyResolver.resolveEvdevCode(KeyEvent.KEYCODE_BACK, 158, true));
        assertEquals(1, KeyResolver.resolveEvdevCode(KeyEvent.KEYCODE_BACK, 0, true));
    }

    @Test
    public void aBrowserBackScanCodeConvertsEvenUnderAnotherKeyCode() {
        // Some layouts report KEY_BACK as the scan code of some other key code;
        // the scan code is what identifies it.
        assertEquals(1, KeyResolver.resolveEvdevCode(KeyEvent.KEYCODE_A,
                KeyResolver.EVDEV_BROWSER_BACK, true));
        assertEquals(KeyResolver.EVDEV_BROWSER_BACK, KeyResolver.resolveEvdevCode(
                KeyEvent.KEYCODE_A, KeyResolver.EVDEV_BROWSER_BACK, false));
    }

    /**
     * The Meta keys are the Win/Super keys. KDE calls that modifier "Meta", so
     * this is the whole of what makes the key work there: evdev 125/126.
     */
    @Test
    public void metaKeysResolveToTheWinKeysRegardlessOfVendorScanCode() {
        assertEquals(125, KeyResolver.resolveEvdevCode(KeyEvent.KEYCODE_META_LEFT, 0, false));
        assertEquals(126, KeyResolver.resolveEvdevCode(KeyEvent.KEYCODE_META_RIGHT, 0, false));
        // A vendor scan code Linux would not recognise must not win.
        assertEquals(125, KeyResolver.resolveEvdevCode(KeyEvent.KEYCODE_META_LEFT, 999, false));
        assertEquals(126, KeyResolver.resolveEvdevCode(KeyEvent.KEYCODE_META_RIGHT, 999, false));
        // And a keyboard that reports the real code is still right.
        assertEquals(125, KeyResolver.resolveEvdevCode(KeyEvent.KEYCODE_META_LEFT, 125, false));
    }

    @Test
    public void escapeItselfIsUnaffectedByConversion() {
        assertEquals(1, KeyResolver.resolveEvdevCode(KeyEvent.KEYCODE_ESCAPE, 0, false));
        assertEquals(1, KeyResolver.resolveEvdevCode(KeyEvent.KEYCODE_ESCAPE, 0, true));
    }

    @Test
    public void reservedKeyRangeIsExactlyF13ToF24() {
        assertEquals(true, KeyResolver.shouldPreferMappedKey(KeyEvent.KEYCODE_F13));
        assertEquals(true, KeyResolver.shouldPreferMappedKey(KeyEvent.KEYCODE_F24));
        assertEquals(false, KeyResolver.shouldPreferMappedKey(KeyEvent.KEYCODE_F12));
    }
}
