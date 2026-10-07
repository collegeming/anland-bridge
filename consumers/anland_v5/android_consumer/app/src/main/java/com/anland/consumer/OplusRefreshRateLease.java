package com.anland.consumer;

import android.os.IBinder;
import android.os.Parcel;
import android.util.Log;

import java.lang.reflect.Method;
import java.util.IdentityHashMap;
import java.util.Map;

/**
 * Process-wide lease for the Oplus refresh-rate policy used by immersive mode.
 *
 * <p>The ordinary {@code Surface.setFrameRate()} vote is only a producer hint
 * on Oplus devices. This vendor transaction is the policy switch that disables
 * TouchIdle's low-rate vote while the desktop has exclusive input. The API is
 * not part of the Android SDK, so every failure is deliberately non-fatal.</p>
 *
 * <p>The service method is global rather than token-scoped. Keep one reference
 * per Activity so a secondary Anland window cannot release the lock while
 * another window is still immersive. Identity semantics are intentional:
 * Activity instances, not their {@code equals()} implementations, own leases.</p>
 */
final class OplusRefreshRateLease {
    private static final String TAG = "Anland";
    static final String KEY_ENABLED = "oplus_refresh_rate_lock";
    static final boolean DEFAULT_ENABLED = false;
    private static final String SERVICE_NAME = "oplus_vrr_service";
    private static final String DESCRIPTOR = "com.oplus.vrr.IOPlusRefreshRate";
    private static final int TRANSACTION_SET_EXTERNAL_REFRESH_RATE_STATUS = 1;

    private static final Object LOCK = new Object();
    private static final Map<Object, Boolean> OWNERS = new IdentityHashMap<>();
    private static boolean policyEnabled;
    private static boolean failureLogged;

    private OplusRefreshRateLease() {}

    /** Acquire the process-wide policy for {@code owner}; repeated calls are safe. */
    static boolean acquire(Object owner) {
        if (owner == null)
            return false;
        synchronized (LOCK) {
            if (OWNERS.containsKey(owner))
                return policyEnabled;

            // Only the first window talks to the service. This also makes the
            // callback from ImmersiveMode.onReady() harmless after start().
            if (OWNERS.isEmpty()) {
                if (!setPolicyLocked(true))
                    return false;
                policyEnabled = true;
            }
            OWNERS.put(owner, Boolean.TRUE);
            return true;
        }
    }

    /** Release the owner's reference; repeated calls and missing owners are safe. */
    static void release(Object owner) {
        if (owner == null)
            return;
        synchronized (LOCK) {
            if (OWNERS.remove(owner) == null)
                return;
            if (OWNERS.isEmpty() && policyEnabled) {
                // Clear local state even if the vendor service has gone away;
                // a later session gets a fresh service lookup and can recover.
                setPolicyLocked(false);
                policyEnabled = false;
            }
        }
    }

    private static boolean setPolicyLocked(boolean enabled) {
        Parcel data = Parcel.obtain();
        Parcel reply = Parcel.obtain();
        try {
            IBinder service = getService();
            if (service == null)
                throw new IllegalStateException("oplus_vrr_service is unavailable");

            data.writeInterfaceToken(DESCRIPTOR);
            data.writeInt(enabled ? 1 : 0);
            if (!service.transact(TRANSACTION_SET_EXTERNAL_REFRESH_RATE_STATUS,
                    data, reply, 0)) {
                throw new IllegalStateException("setExternalRefreshRateStatus transact failed");
            }
            reply.readException();
            Log.i(TAG, "oplus refresh-rate policy " + (enabled ? "enabled" : "released"));
            return true;
        } catch (Throwable error) {
            if (!failureLogged) {
                failureLogged = true;
                Log.w(TAG, "oplus refresh-rate policy unavailable; continuing without lock",
                        error);
            } else {
                Log.w(TAG, "oplus refresh-rate policy change failed", error);
            }
            return false;
        } finally {
            reply.recycle();
            data.recycle();
        }
    }

    private static IBinder getService() throws Exception {
        Class<?> serviceManager = Class.forName("android.os.ServiceManager");
        Method getService = serviceManager.getDeclaredMethod("getService", String.class);
        getService.setAccessible(true);
        return (IBinder) getService.invoke(null, SERVICE_NAME);
    }
}
