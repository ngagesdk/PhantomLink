/*
 * JNI binding of the bridge engine (firmware/src/bridge_core.c), see
 * app/src/main/cpp/jni_bridge.c. One handle per N-Gage; all calls for a
 * handle on one thread.
 *
 * SPDX-License-Identifier: MIT
 */
package de.ngagesdk.phantomlink;

public final class NativeBridge
{
    static {
        System.loadLibrary("gcbridge");
    }

    /* nativeStatus() indices */
    public static final int ST_SRV_STATE = 0;    /* SRV_* */
    public static final int ST_REGISTERED = 1;
    public static final int ST_DEV_LINK = 2;     /* LINK_* */
    public static final int ST_SRV_LINK = 3;
    public static final int ST_TO_SERVER = 4;    /* messages */
    public static final int ST_FROM_SERVER = 5;
    public static final int ST_DEV_PRESENT = 6;
    public static final int ST_DEV_GRACE = 7;    /* server state kept while the N-Gage is away */
    public static final int ST_COUNT = 8;

    public static final int SRV_DOWN = 0;
    public static final int SRV_CONNECTING = 1;
    public static final int SRV_UP = 2;

    public static final int LINK_UP = 3;

    private NativeBridge()
    {
    }

    public static native long nativeCreate(BridgeCallbacks callbacks, int linkId, String accessKey);
    public static native void nativeDestroy(long h);
    public static native void nativeDevConnected(long h, byte[] address, int now);
    public static native void nativeDevInput(long h, byte[] data, int off, int len, int now);
    public static native void nativeDevDisconnected(long h, int now);
    public static native void nativeSetNetwork(long h, boolean up, int rssi, int now);
    public static native void nativeSrvConnected(long h, int now);
    public static native void nativeSrvInput(long h, byte[] data, int off, int len, int now);
    public static native void nativeSrvDisconnected(long h, int now);
    public static native void nativePoll(long h, int now);
    public static native int nativeNextTimeout(long h, int now);
    public static native void nativeStatus(long h, int[] out);
    public static native String nativeVersion();
}
