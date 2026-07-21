package xh.xianhhh.test.nativebridge;

/** Native fixture with primitive and string return values. */
public final class BasicNative {
    static {
        System.loadLibrary("j2d_basic_native");
    }

    private BasicNative() {
    }

    public static native int add(int left, int right);

    public static native String identity();
}
