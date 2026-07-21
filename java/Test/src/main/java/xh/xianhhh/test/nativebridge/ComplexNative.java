package xh.xianhhh.test.nativebridge;

public final class ComplexNative {
    static { System.loadLibrary("j2d_complex_native"); }
    private ComplexNative() { }
    public static native int[] normalizeAndSort(int[] input);
    public static native long fnv1a64(byte[] input);
}
