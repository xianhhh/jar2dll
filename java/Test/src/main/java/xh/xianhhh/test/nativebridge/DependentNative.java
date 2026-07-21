package xh.xianhhh.test.nativebridge;

/** JNI DLL whose import table depends on j2d_fixture_provider.dll. */
public final class DependentNative {
    static {
        // Windows does not reliably search java.library.path for a DLL imported
        // by another DLL, so the provider is loaded first in stable mode.
        System.loadLibrary("j2d_fixture_provider");
        System.loadLibrary("j2d_dependent_native");
    }
    private DependentNative() { }
    public static native int scale(int value);
}
