import java.nio.file.Path;

/** Starts a JVM, loads the generated DLL, and waits for its bootstrap thread. */
public final class MemoryChainHost {
    public static void main(String[] args) throws Exception {
        if (args.length != 1) throw new IllegalArgumentException("Expected generated DLL path");
        System.load(Path.of(args[0]).toAbsolutePath().toString());
        Thread.sleep(2000);
    }
}
