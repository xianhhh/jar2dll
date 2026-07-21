package xh.xianhhh.test;

import xh.xianhhh.jar2dll.DllMain;
import xh.xianhhh.test.nativebridge.NativeFixtureMain;

/** A small end-to-end fixture for Gradle buildDll and DLL-to-JAR dumping. */
public final class J2dTestMain {
    private J2dTestMain() {
    }

    @DllMain
    public static void load(String[] args) {
        System.out.println("J2D_DLL_ENTRY_OK args=" + args.length);
    }

    public static void main(String[] args) {
        System.out.println("J2D_DUMP_JAR_OK");
    }

    /** Invoked manually with the JNI fixture directory on java.library.path. */
    public static void verifyNativeFixtures(String[] args) {
        NativeFixtureMain.main(args);
    }
}
