package xh.xianhhh;

import org.objectweb.asm.ClassReader;
import org.objectweb.asm.ClassWriter;
import org.objectweb.asm.Opcodes;
import org.objectweb.asm.tree.AbstractInsnNode;
import org.objectweb.asm.tree.ClassNode;
import org.objectweb.asm.tree.InsnList;
import org.objectweb.asm.tree.LdcInsnNode;
import org.objectweb.asm.tree.MethodInsnNode;
import org.objectweb.asm.tree.MethodNode;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

/**
 * Rewrites direct JNI loading calls for DLL resources embedded in the same JAR.
 * The removed calls are handled by jar2dll's memory module mapper instead.
 */
public final class MemoryLoadRewriter {
    private MemoryLoadRewriter() { }

    public static void main(String[] args) throws Exception {
        Arguments arguments = Arguments.parse(args);
        RewriteResult result = rewrite(arguments.input(), arguments.output());
        System.out.printf("J2D_MEMORY_REWRITE_OK dlls=%d classes=%d calls=%d%n",
            result.dllCount(), result.classCount(), result.callCount());
    }

    public static RewriteResult rewrite(Path input, Path output) throws IOException {
        if (!Files.isRegularFile(input)) throw new IOException("Input JAR does not exist: " + input);
        Path parent = output.toAbsolutePath().getParent();
        if (parent != null) Files.createDirectories(parent);

        try (ZipFile source = new ZipFile(input.toFile());
             OutputStream file = Files.newOutputStream(output);
             ZipOutputStream target = new ZipOutputStream(file)) {
            Set<String> libraries = findEmbeddedLibraries(source);
            int classes = 0;
            int calls = 0;
            var entries = source.entries();
            while (entries.hasMoreElements()) {
                ZipEntry entry = entries.nextElement();
                ZipEntry copy = new ZipEntry(entry.getName());
                target.putNextEntry(copy);
                try (InputStream data = source.getInputStream(entry)) {
                    if (entry.getName().endsWith(".class")) {
                        RewriteClassResult rewritten = rewriteClass(data.readAllBytes(), libraries);
                        target.write(rewritten.bytes());
                        if (rewritten.callsRemoved() > 0) ++classes;
                        calls += rewritten.callsRemoved();
                    } else {
                        data.transferTo(target);
                    }
                }
                target.closeEntry();
            }
            int dllCount = (int) libraries.stream().filter(name -> name.endsWith(".dll")).count();
            return new RewriteResult(dllCount, classes, calls);
        }
    }

    private static Set<String> findEmbeddedLibraries(ZipFile source) {
        Set<String> result = new HashSet<>();
        var entries = source.entries();
        while (entries.hasMoreElements()) {
            ZipEntry entry = entries.nextElement();
            if (!entry.isDirectory() && entry.getName().toLowerCase(Locale.ROOT).endsWith(".dll")) {
                String name = entry.getName().substring(entry.getName().lastIndexOf('/') + 1);
                result.add(name.toLowerCase(Locale.ROOT));
                result.add(name.substring(0, name.length() - 4).toLowerCase(Locale.ROOT));
            }
        }
        return result;
    }

    private static RewriteClassResult rewriteClass(byte[] original, Set<String> libraries) {
        ClassNode node = new ClassNode();
        new ClassReader(original).accept(node, 0);
        int callsRemoved = 0;
        for (MethodNode method : node.methods) callsRemoved += rewriteMethod(method.instructions, libraries);
        if (callsRemoved == 0) return new RewriteClassResult(original, 0);
        ClassWriter writer = new ClassWriter(ClassWriter.COMPUTE_MAXS);
        node.accept(writer);
        return new RewriteClassResult(writer.toByteArray(), callsRemoved);
    }

    private static int rewriteMethod(InsnList instructions, Set<String> libraries) {
        int removed = 0;
        for (AbstractInsnNode current = instructions.getFirst(); current != null;) {
            AbstractInsnNode next = current.getNext();
            if (!(current instanceof LdcInsnNode ldc) || !(ldc.cst instanceof String value) || !matchesLibrary(value, libraries)) {
                current = next;
                continue;
            }
            AbstractInsnNode first = nextInstruction(current);
            if (isSystemLoad(first)) {
                AbstractInsnNode after = nextInstruction(first);
                instructions.remove(current);
                instructions.remove(first);
                ++removed;
                current = after;
                continue;
            } else if (isRuntimeGetRuntime(previousInstruction(current)) && isRuntimeLoad(first)) {
                AbstractInsnNode getRuntime = previousInstruction(current);
                AbstractInsnNode after = nextInstruction(first);
                instructions.remove(getRuntime);
                instructions.remove(current);
                instructions.remove(first);
                ++removed;
                current = after;
                continue;
            }
            current = next;
        }
        return removed;
    }

    private static AbstractInsnNode nextInstruction(AbstractInsnNode node) {
        for (AbstractInsnNode next = node.getNext(); next != null; next = next.getNext()) {
            if (next.getOpcode() >= 0) return next;
        }
        return null;
    }

    private static AbstractInsnNode previousInstruction(AbstractInsnNode node) {
        for (AbstractInsnNode previous = node.getPrevious(); previous != null; previous = previous.getPrevious()) {
            if (previous.getOpcode() >= 0) return previous;
        }
        return null;
    }

    private static boolean isSystemLoad(AbstractInsnNode node) {
        return node instanceof MethodInsnNode call
            && call.getOpcode() == Opcodes.INVOKESTATIC
            && call.owner.equals("java/lang/System")
            && (call.name.equals("load") || call.name.equals("loadLibrary"))
            && call.desc.equals("(Ljava/lang/String;)V");
    }

    private static boolean isRuntimeGetRuntime(AbstractInsnNode node) {
        return node instanceof MethodInsnNode call
            && call.getOpcode() == Opcodes.INVOKESTATIC
            && call.owner.equals("java/lang/Runtime")
            && call.name.equals("getRuntime")
            && call.desc.equals("()Ljava/lang/Runtime;");
    }

    private static boolean isRuntimeLoad(AbstractInsnNode node) {
        return node instanceof MethodInsnNode call
            && call.getOpcode() == Opcodes.INVOKEVIRTUAL
            && call.owner.equals("java/lang/Runtime")
            && (call.name.equals("load") || call.name.equals("loadLibrary"))
            && call.desc.equals("(Ljava/lang/String;)V");
    }

    private static boolean matchesLibrary(String value, Set<String> libraries) {
        String normalized = value.replace('\\', '/').toLowerCase(Locale.ROOT);
        if (libraries.contains(normalized)) return true;
        int slash = normalized.lastIndexOf('/');
        return slash >= 0 && libraries.contains(normalized.substring(slash + 1));
    }

    public record RewriteResult(int dllCount, int classCount, int callCount) { }
    private record RewriteClassResult(byte[] bytes, int callsRemoved) { }
    private record Arguments(Path input, Path output) {
        static Arguments parse(String[] args) {
            Path input = null;
            Path output = null;
            for (int index = 0; index < args.length; ++index) {
                if (args[index].equals("--input") && index + 1 < args.length) input = Path.of(args[++index]);
                else if (args[index].equals("--output") && index + 1 < args.length) output = Path.of(args[++index]);
                else throw new IllegalArgumentException("Usage: MemoryLoadRewriter --input <jar> --output <jar>");
            }
            if (input == null || output == null) throw new IllegalArgumentException("Usage: MemoryLoadRewriter --input <jar> --output <jar>");
            return new Arguments(input, output);
        }
    }
}
