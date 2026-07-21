package xh.xianhhh.test.nativebridge;

/** Verifies long JNI names generated for overloaded methods. */
public final class OverloadNative {
    static {
        System.loadLibrary("j2d_overload_native");
    }

    private OverloadNative() {
    }

    public static native String describe(int value);

    public static native String describe(String value);
}
