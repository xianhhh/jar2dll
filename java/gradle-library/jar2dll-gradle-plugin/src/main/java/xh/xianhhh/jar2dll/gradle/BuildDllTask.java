package xh.xianhhh.jar2dll.gradle;

import org.gradle.api.DefaultTask;
import org.gradle.api.GradleException;
import org.gradle.api.file.ConfigurableFileCollection;
import org.gradle.api.file.DirectoryProperty;
import org.gradle.api.file.RegularFileProperty;
import org.gradle.api.provider.ListProperty;
import org.gradle.api.provider.Property;
import org.gradle.api.tasks.Input;
import org.gradle.api.tasks.InputFile;
import org.gradle.api.tasks.InputFiles;
import org.gradle.api.tasks.Internal;
import org.gradle.api.tasks.Optional;
import org.gradle.api.tasks.OutputFile;
import org.gradle.api.tasks.PathSensitive;
import org.gradle.api.tasks.PathSensitivity;
import org.gradle.api.tasks.TaskAction;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;

public abstract class BuildDllTask extends DefaultTask {
    private static final String DLL_MAIN_DESCRIPTOR = "Lxh/xianhhh/jar2dll/DllMain;";

    @InputFile @PathSensitive(PathSensitivity.RELATIVE) public abstract RegularFileProperty getInputJar();
    @InputFiles @PathSensitive(PathSensitivity.RELATIVE) public abstract ConfigurableFileCollection getClassDirectories();
    @Input public abstract Property<String> getOutputName();
    @Input public abstract Property<String> getPlatform();
    @Input public abstract Property<String> getConfiguration();
    @Input public abstract Property<Boolean> getCanDump();
    @Input public abstract ListProperty<String> getClassLoaders();
    @InputFiles @PathSensitive(PathSensitivity.RELATIVE) public abstract ConfigurableFileCollection getLibraries();
    @Optional @InputFile @PathSensitive(PathSensitivity.RELATIVE) public abstract RegularFileProperty getConfiguredBuilder();
    @Internal public abstract RegularFileProperty getConfiguredOutput();
    @Internal public abstract DirectoryProperty getDefaultOutputDirectory();

    @OutputFile
    public Path getOutputDll() {
        if (getConfiguredOutput().isPresent()) return getConfiguredOutput().get().getAsFile().toPath();
        return getDefaultOutputDirectory().file(getOutputName()).get().getAsFile().toPath();
    }

    @TaskAction
    public void buildDll() throws IOException, InterruptedException {
        if (!System.getProperty("os.name").toLowerCase().contains("win")) {
            throw new GradleException("buildDll is only supported on Windows because it produces a Windows DLL.");
        }
        String platform = getPlatform().get();
        String configuration = getConfiguration().get();
        if (!(platform.equals("x64") || platform.equals("Win32"))) throw new GradleException("jar2dll.platform must be x64 or Win32");
        if (!(configuration.equals("Release") || configuration.equals("Debug"))) throw new GradleException("jar2dll.configuration must be Release or Debug");

        EntryPoint entry = discoverEntryPoint();
        Path output = getOutputDll();
        Files.createDirectories(output.getParent());
        Path builder = getConfiguredBuilder().isPresent()
                ? getConfiguredBuilder().get().getAsFile().toPath()
                : extractBundledBuilder();
        if (!Files.isRegularFile(builder)) throw new GradleException("Jar2Dll builder not found: " + builder);

        List<String> command = new ArrayList<>(List.of(builder.toString(), "-i", getInputJar().get().getAsFile().getAbsolutePath(),
                "-run", entry.className, "-method", entry.methodName, "-o", output.toString(),
                "-signature", entry.methodSignature,
                "-platform", platform, "-configuration", configuration,
                "-candump", getCanDump().get().toString()));
        List<String> classLoaders = getClassLoaders().get().stream()
                .filter(name -> !name.isBlank()).distinct().toList();
        if (!classLoaders.isEmpty()) {
            command.add("-cld");
            command.add(String.join(";", classLoaders));
        }
        for (var library : getLibraries().getFiles()) {
            command.add("-lib");
            command.add(library.getAbsolutePath());
        }
        getLogger().lifecycle("Building {} from {}#{}()", output, entry.className, entry.methodName);
        Process process = new ProcessBuilder(command).inheritIO().start();
        if (process.waitFor() != 0) throw new GradleException("Jar2Dll builder failed with exit code " + process.exitValue());
    }

    private Path extractBundledBuilder() throws IOException {
        Path target = getTemporaryDir().toPath().resolve("j2d.exe");
        if (Files.isRegularFile(target)) return target;
        try (InputStream source = BuildDllTask.class.getResourceAsStream("/jar2dll/j2d.exe")) {
            if (source == null) throw new GradleException("The plugin JAR does not contain j2d.exe; configure jar2dll.builderExecutable.");
            Files.copy(source, target, StandardCopyOption.REPLACE_EXISTING);
        }
        return target;
    }

    private EntryPoint discoverEntryPoint() throws IOException {
        List<EntryPoint> entries = new ArrayList<>();
        for (Path directory : getClassDirectories().getFiles().stream().map(file -> file.toPath()).toList()) {
            if (!Files.isDirectory(directory)) continue;
            try (var files = Files.walk(directory)) {
                files.filter(path -> path.toString().endsWith(".class")).forEach(path -> scan(path, entries));
            }
        }
        if (entries.isEmpty()) throw new GradleException("No @DllMain method found in main source set.");
        if (entries.size() != 1) throw new GradleException("Exactly one @DllMain method is required; found: " + entries);
        return entries.get(0);
    }

    private void scan(Path path, List<EntryPoint> entries) {
        try (InputStream input = Files.newInputStream(path)) {
            new ClassFileScanner(input).scan(entries);
        } catch (IOException exception) { throw new GradleException("Cannot inspect class " + path, exception); }
    }

    private record EntryPoint(String className, String methodName, String methodSignature) { }

    /** Reads only the class-file metadata needed to find a CLASS-retained method annotation. */
    private static final class ClassFileScanner {
        private static final int ACC_STATIC = 0x0008;
        private final DataInputStream input;
        private Object[] constantPool;

        ClassFileScanner(InputStream input) {
            this.input = new DataInputStream(input);
        }

        void scan(List<EntryPoint> entries) throws IOException {
            if (input.readInt() != 0xCAFEBABE) throw new IOException("Not a Java class file");
            input.readUnsignedShort(); // minor version
            input.readUnsignedShort(); // major version
            readConstantPool();
            input.readUnsignedShort(); // class access flags
            String className = className(input.readUnsignedShort()).replace('/', '.');
            input.readUnsignedShort(); // super class
            skipUnsignedShorts(); // interfaces
            skipMembers(); // fields

            int methodCount = input.readUnsignedShort();
            for (int index = 0; index < methodCount; index++) {
                int access = input.readUnsignedShort();
                String name = utf8(input.readUnsignedShort());
                String descriptor = utf8(input.readUnsignedShort());
                boolean marked = readAttributes();
                if (marked) {
                    if ((access & ACC_STATIC) == 0
                            || !("()V".equals(descriptor) || "([Ljava/lang/String;)V".equals(descriptor))) {
                        throw new GradleException("@DllMain must mark static void " + className + "." + name + "() or " + name + "(String[])");
                    }
                    entries.add(new EntryPoint(className, name, descriptor));
                }
            }
        }

        private void readConstantPool() throws IOException {
            constantPool = new Object[input.readUnsignedShort()];
            for (int index = 1; index < constantPool.length; index++) {
                switch (input.readUnsignedByte()) {
                    case 1 -> constantPool[index] = input.readUTF();
                    case 3, 4 -> input.readInt();
                    case 5, 6 -> { input.readLong(); index++; }
                    case 7 -> constantPool[index] = new ClassInfo(input.readUnsignedShort());
                    case 8, 16, 19, 20 -> input.readUnsignedShort();
                    case 9, 10, 11, 12, 17, 18 -> { input.readUnsignedShort(); input.readUnsignedShort(); }
                    case 15 -> { input.readUnsignedByte(); input.readUnsignedShort(); }
                    default -> throw new IOException("Unknown constant-pool entry");
                }
            }
        }

        private void skipMembers() throws IOException {
            int count = input.readUnsignedShort();
            for (int index = 0; index < count; index++) {
                input.readUnsignedShort();
                input.readUnsignedShort();
                input.readUnsignedShort();
                skipAttributes();
            }
        }

        private boolean readAttributes() throws IOException {
            boolean marked = false;
            int count = input.readUnsignedShort();
            for (int index = 0; index < count; index++) {
                String name = utf8(input.readUnsignedShort());
                long length = Integer.toUnsignedLong(input.readInt());
                if ("RuntimeVisibleAnnotations".equals(name) || "RuntimeInvisibleAnnotations".equals(name)) {
                    if (length > Integer.MAX_VALUE) throw new IOException("Annotation attribute is too large");
                    byte[] bytes = input.readNBytes((int) length);
                    if (bytes.length != length) throw new IOException("Unexpected end of annotation attribute");
                    marked |= containsDllMainAnnotation(new DataInputStream(new java.io.ByteArrayInputStream(bytes)));
                } else {
                    skipFully(length);
                }
            }
            return marked;
        }

        private boolean containsDllMainAnnotation(DataInputStream attribute) throws IOException {
            boolean marked = false;
            int count = attribute.readUnsignedShort();
            for (int index = 0; index < count; index++) {
                if (DLL_MAIN_DESCRIPTOR.equals(utf8(attribute.readUnsignedShort()))) marked = true;
                int pairs = attribute.readUnsignedShort();
                for (int pair = 0; pair < pairs; pair++) {
                    attribute.readUnsignedShort();
                    skipElementValue(attribute);
                }
            }
            return marked;
        }

        private static void skipElementValue(DataInputStream value) throws IOException {
            int tag = value.readUnsignedByte();
            if ("BCDFIJSZs".indexOf(tag) >= 0) { value.readUnsignedShort(); return; }
            if (tag == 'e') { value.readUnsignedShort(); value.readUnsignedShort(); return; }
            if (tag == 'c') { value.readUnsignedShort(); return; }
            if (tag == '@') { skipAnnotation(value); return; }
            if (tag == '[') {
                int count = value.readUnsignedShort();
                for (int index = 0; index < count; index++) skipElementValue(value);
                return;
            }
            throw new IOException("Unknown annotation element tag");
        }

        private static void skipAnnotation(DataInputStream annotation) throws IOException {
            annotation.readUnsignedShort();
            int pairs = annotation.readUnsignedShort();
            for (int index = 0; index < pairs; index++) {
                annotation.readUnsignedShort();
                skipElementValue(annotation);
            }
        }

        private void skipAttributes() throws IOException {
            int count = input.readUnsignedShort();
            for (int index = 0; index < count; index++) {
                input.readUnsignedShort();
                skipFully(Integer.toUnsignedLong(input.readInt()));
            }
        }

        private void skipUnsignedShorts() throws IOException {
            int count = input.readUnsignedShort();
            for (int index = 0; index < count; index++) input.readUnsignedShort();
        }

        private void skipFully(long length) throws IOException {
            while (length > 0) {
                int skipped = input.skipBytes((int) Math.min(length, Integer.MAX_VALUE));
                if (skipped == 0) { input.readByte(); skipped = 1; }
                length -= skipped;
            }
        }

        private String utf8(int index) throws IOException {
            if (index <= 0 || index >= constantPool.length || !(constantPool[index] instanceof String value)) {
                throw new IOException("Invalid UTF-8 constant-pool index");
            }
            return value;
        }

        private String className(int index) throws IOException {
            if (index <= 0 || index >= constantPool.length || !(constantPool[index] instanceof ClassInfo info)) {
                throw new IOException("Invalid class constant-pool index");
            }
            return utf8(info.nameIndex);
        }

        private record ClassInfo(int nameIndex) { }
    }
}
