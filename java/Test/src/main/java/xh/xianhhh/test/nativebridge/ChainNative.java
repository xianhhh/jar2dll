package xh.xianhhh.test.nativebridge;
public final class ChainNative {
    static { System.loadLibrary("j2d_chain_provider"); System.loadLibrary("j2d_chain_bridge"); System.loadLibrary("j2d_chain_jni"); }
    private ChainNative() { }
    public static native int stress(int rounds);
}
