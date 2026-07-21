package xh.xianhhh.test.memory;

import xh.xianhhh.test.nativebridge.ChainNative;

/** Entry point for the three-DLL memory-mode integration test. */
public final class MemoryChainMain {
    private MemoryChainMain() { }

    public static void load() {
        int first = ChainNative.stress(100_000);
        int second = ChainNative.stress(100_000);
        if (first != second) throw new AssertionError("Mapped JNI dependency chain is not deterministic");
        System.out.println("J2D_MEMORY_COMPLEX_CHAIN_OK value=" + first);
    }
}
