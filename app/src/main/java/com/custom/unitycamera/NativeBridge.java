package com.custom.unitycamera;

public final class NativeBridge {
    static {
        System.loadLibrary("UnityCam");
    }

    public static native String[] nativeListProcesses();

    public static native String nativeAttach(int pid);

    public static native void nativeStop();

    public static native void nativeSetScreen(int w, int h);

    public static native String nativeStatus();

    public static native float[] nativeLines();

    private NativeBridge() {}
}
