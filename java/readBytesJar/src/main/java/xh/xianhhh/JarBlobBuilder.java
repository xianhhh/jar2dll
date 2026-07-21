package xh.xianhhh;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipFile;

/**
 * JarBlobBuilder
 *
 * 将一个 JAR（ZIP）文件的条目序列化为自定义二进制 blob（"J2DF"），
 * 便于通过 JNI 或其它渠道在本地 C++ 代码中解析。
 *
 * 格式（严格小端）：
 * - header:
 *   - 4 bytes: magic = 'J' '2' 'D' 'F'
 *   - 1 byte : version = 0x01
 *   - 4 bytes: entry_count (uint32, little-endian)
 * - entries (repeated entry_count 次):
 *   - 1 byte : type (0x01 = class, 0x02 = resource)
 *   - 4 bytes: name_len (uint32, little-endian) // name 的字节长度（UTF-8）
 *   - N bytes: name (UTF-8, 不带终止符)
 *   - 4 bytes: data_len (uint32, little-endian)
 *   - M bytes: data
 *
 * 说明：
 * - 我们选择 little-endian（小端）以便与 C++ 侧按字节直接解析一致。
 * - Java 默认的 DataOutputStream.writeInt 是大端（big-endian），因此示例中提供了 writeIntLE 方法来写小端。
 * - name 使用 UTF-8 编码，保持 ZIP 内的路径格式（例如 "com/example/Foo.class"）。
 */
public final class JarBlobBuilder {
    private JarBlobBuilder() {}

    // magic + version 常量
    private static final byte[] MAGIC = new byte[] {'J', '2', 'D', 'F'};
    private static final byte VERSION = 1;

    // 类型常量：class 或 resource
    public static final byte TYPE_CLASS = 1;
    public static final byte TYPE_RESOURCE = 2;

    /**
     * 内部简单 POJO：一个条目（type + name + data）
     * - type: 1 字节（TYPE_CLASS 或 TYPE_RESOURCE）
     * - name: UTF-8 字符串（不包含终止符）
     * - data: 原始字节内容（class 文件二进制或任意资源二进制）
     */
    private static class Entry {
        final byte type;
        final String name;
        final byte[] data;

        Entry(byte type, String name, byte[] data) {
            this.type = type;
            this.name = name;
            this.data = data;
        }
    }

    /**
     * 从给定的 JAR 字节数组解析并构建 J2DF blob。
     *
     * 步骤：
     * 1. 使用 ZipInputStream 解压 JAR，遍历每个 ZipEntry。
     * 2. 忽略目录（isDirectory）。
     * 3. 决定 type：以 ".class" 结尾视为 class，否则视为 resource。
     * 4. 收集 entries 并按约定格式写入 ByteArrayOutputStream（注意小端写法）。
     *
     * @param jarBytes JAR 文件的完整字节数组（例如通过 Files.readAllBytes(Path) 或 InputStream.readAllBytes() 获得）
     * @return 符合 J2DF 格式的字节数组（可直接传给 native 层）
     * @throws IOException 读取或写入过程中发生 IO 错误时抛出
     */
    public static byte[] buildFromJarBytes(byte[] jarBytes) throws IOException {
        // 收集解析到的条目
        List<Entry> entries = new ArrayList<>();
        System.out.println("Begin Read Jar");
        System.out.println("Jar size: " + jarBytes.length);
        // 使用 ZipInputStream 从字节数组中遍历 ZIP 条目（JAR 本质上是 ZIP）
        try (ByteArrayInputStream bais = new ByteArrayInputStream(jarBytes);
             ZipInputStream zis = new ZipInputStream(bais)) {
            ZipEntry ze;
            // 读取每个 ZIP 条目
            while ((ze = zis.getNextEntry()) != null) {
                System.out.println(String.format("Reading entry: %s, size: %d", ze.getName(), ze.getSize()));
                // 忽略目录
                if (ze.isDirectory()) {
                    zis.closeEntry();
                    continue;
                }
                // 读取条目完整数据到内存（使用 ByteArrayOutputStream 临时缓存）
                ByteArrayOutputStream tmp = new ByteArrayOutputStream();
                byte[] buf = new byte[8192];
                int r;
                while ((r = zis.read(buf)) != -1) {
                    tmp.write(buf, 0, r);
                }
                byte[] data = tmp.toByteArray();

                // 决定类型：以 .class 结尾视为 class 文件
                String name = ze.getName();
                byte type = name.endsWith(".class") ? TYPE_CLASS : TYPE_RESOURCE;
                entries.add(new Entry(type, name, data));

                zis.closeEntry();
            }
        }

        // 将收集到的 entries 写入二进制 blob（小端）
        try (ByteArrayOutputStream baos = new ByteArrayOutputStream();
             DataOutputStream out = new DataOutputStream(baos)) {

            // 1) 写 magic（4 bytes）
            out.write(MAGIC);
            System.out.println("Write Magic: " + Arrays.toString(MAGIC));
            // 2) 写 version（1 byte）
            out.writeByte(VERSION);
            System.out.println("Write Version: " + VERSION);
            // 3) 写 entry_count（4 bytes, little-endian）
            writeIntLE(out, entries.size());

            // 4) 写每个 entry
            for (Entry e : entries) {
                // type（1 byte）
                System.out.println(String.format("Write Entry: type=%d, name=%s, size=%d", e.type, e.name, e.data.length));
                out.writeByte(e.type);

                // name bytes（UTF-8）
                byte[] nameBytes = e.name.getBytes(StandardCharsets.UTF_8);

                // name length（4 bytes, little-endian）
                writeIntLE(out, nameBytes.length);

                // name 数据（N bytes）
                out.write(nameBytes);

                // data length（4 bytes, little-endian）
                int dataLen = (e.data != null) ? e.data.length : 0;
                writeIntLE(out, dataLen);

                // data 数据（M bytes）
                if (dataLen > 0) out.write(e.data);
            }

            out.flush();
            return baos.toByteArray();
        }
    }

    /**
     * 辅助：从磁盘文件读取 JAR 并构建 blob
     *
     * 直接读取文件到内存并调用 buildFromJarBytes。
     * 对于大型 JAR 或内存敏感场景，建议使用流式处理或把原始 jar bytes 直接传给 native 层再由 native 解压。
     */
    public static byte[] buildFromJarFile(File jarFile) throws IOException {
        try (FileInputStream fis = new FileInputStream(jarFile)) {
            // Java 9+ 可用 readAllBytes(); 若使用更旧的 Java，请替换为循环读取
            byte[] jarBytes = fis.readAllBytes();
            return buildFromJarBytes(jarBytes);
        }
    }

    /**
     * Writes a J2DF file directly from a JAR without retaining either the original
     * archive or all extracted entries in memory. This is the command-line reader's
     * fast path and keeps memory proportional to one ZIP entry instead of the JAR.
     */
    public static void writeFromJarFile(File jarFile, File outputFile) throws IOException {
        try (ZipFile zip = new ZipFile(jarFile);
             RandomAccessFile output = new RandomAccessFile(outputFile, "rw")) {
            output.setLength(0);
            output.write(MAGIC);
            output.writeByte(VERSION);
            output.writeInt(0); // Patched with the count after streaming all entries.
            long count = 0;
            byte[] buffer = new byte[1024 * 1024];
            Enumeration<? extends ZipEntry> entries = zip.entries();
            while (entries.hasMoreElements()) {
                ZipEntry entry = entries.nextElement();
                if (entry.isDirectory()) continue;
                long size = entry.getSize();
                if (size < 0 || size > 0xFFFF_FFFFL) {
                    throw new IOException("Unsupported ZIP entry size: " + entry.getName());
                }
                byte[] name = entry.getName().getBytes(StandardCharsets.UTF_8);
                output.writeByte(entry.getName().endsWith(".class") ? TYPE_CLASS : TYPE_RESOURCE);
                writeIntLE(output, name.length);
                output.write(name);
                writeIntLE(output, (int) size);
                try (InputStream input = zip.getInputStream(entry)) {
                    int read;
                    while ((read = input.read(buffer)) != -1) output.write(buffer, 0, read);
                }
                if (++count > 0xFFFF_FFFFL) throw new IOException("Too many JAR entries");
            }
            output.seek(5);
            writeIntLE(output, (int) count);
        }
    }

    /**
     * 小端写 int（4 字节）
     *
     * 注意：
     * - DataOutputStream.writeInt 是大端（network byte order），
     *   本处要求小端，因此手动按字节写入低位到高位。
     */
    private static void writeIntLE(DataOutputStream out, int v) throws IOException {
        out.writeByte(v & 0xFF);
        out.writeByte((v >>> 8) & 0xFF);
        out.writeByte((v >>> 16) & 0xFF);
        out.writeByte((v >>> 24) & 0xFF);
    }

    private static void writeIntLE(RandomAccessFile out, int v) throws IOException {
        out.writeByte(v & 0xFF);
        out.writeByte((v >>> 8) & 0xFF);
        out.writeByte((v >>> 16) & 0xFF);
        out.writeByte((v >>> 24) & 0xFF);
    }
}
