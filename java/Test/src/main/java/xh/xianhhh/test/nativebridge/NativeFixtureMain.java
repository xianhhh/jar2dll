package xh.xianhhh.test.nativebridge;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/**
 * Run with -Djava.library.path=<cpp/jni-tests/build/Debug> after building the
 * fixtures. The same classes are used later by memory-mode redirection tests.
 */
public final class NativeFixtureMain {
    private NativeFixtureMain() {
    }

    public static void main(String[] args) {
        if (BasicNative.add(19, 23) != 42) throw new AssertionError("basic add failed");
        if (!"basic-native-ok".equals(BasicNative.identity())) throw new AssertionError("basic identity failed");
        if (!"int:7".equals(OverloadNative.describe(7))) throw new AssertionError("int overload failed");
        if (!"jni".equals(OverloadNative.describe("jni"))) throw new AssertionError("string overload failed");
        if (!Arrays.equals(new int[] {1, 2, 3, 4}, ComplexNative.normalizeAndSort(new int[] {3, -1, 4, -2}))) throw new AssertionError("array transform failed");
        byte[] payload = "jni".getBytes(StandardCharsets.UTF_8);
        if (ComplexNative.fnv1a64(payload) != fnv1a64(payload)) throw new AssertionError("hash failed");
        if (DependentNative.scale(13) != 40) throw new AssertionError("dependent native import failed");
        int first = ChainNative.stress(100_000);
        if (first != ChainNative.stress(100_000)) throw new AssertionError("multi-DLL stress is not deterministic");
        System.out.println("J2D_NATIVE_FIXTURES_OK");
    }

    private static long fnv1a64(byte[] values) {
        long hash = 0xCBF29CE484222325L;
        for (byte value : values) {
            hash ^= value & 0xFFL;
            hash *= 0x100000001B3L;
        }
        return hash;
    }
}
