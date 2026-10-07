package com.anland.consumer;

import android.content.Context;
import android.hardware.input.InputManager;
import android.view.InputDevice;

/** Discovery for Settings. Input itself is read exclusively by the root helper. */
final class GoldKeyboard {
    // Keep in sync with Gold's fn_remap.c and input_grab.c.
    static final String GOLD_NAME_PREFIX = "Gold Keyboardremaps";
    static final int GOLD_VENDOR = 0xffff;
    static final int GOLD_PRODUCT = 0xffff;

    private GoldKeyboard() {}

    static boolean matchesGoldKeyboard(boolean present, String name, int vendorId,
                                       int productId, int sources) {
        return present && name != null && name.startsWith(GOLD_NAME_PREFIX)
                && vendorId == GOLD_VENDOR && productId == GOLD_PRODUCT
                && (sources & InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD;
    }

    static boolean goldKeyboardPresent(Context context) {
        InputManager manager = context.getSystemService(InputManager.class);
        if (manager == null)
            return false;
        for (int id : manager.getInputDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (device != null && matchesGoldKeyboard(true, device.getName(),
                    device.getVendorId(), device.getProductId(), device.getSources()))
                return true;
        }
        return false;
    }

    static boolean matches(InputDevice device) {
        return device != null && matchesGoldKeyboard(true, device.getName(),
                device.getVendorId(), device.getProductId(), device.getSources());
    }
}
