package xh.xianhhh;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;

public class Main {
   public static void main(String[] args) throws IOException {
        if (args.length != 2) {
            System.out.println("Usage: java -jar reader.jar <jar> <output>");
            return;
        }
        File jar = new File(args[0]);
        File output = new File(args[1]);
        if (!jar.isFile()) throw new IOException("JAR file not found: " + jar.getAbsolutePath());
        Files.createDirectories(output.toPath());
        if (!output.isDirectory()) throw new IOException("Output path is not a directory: " + output.getAbsolutePath());

        File file = new File(output, jar.getName() + ".j2df");
        JarBlobBuilder.writeFromJarFile(jar, file);
        System.out.println("Build success: " + file.getAbsolutePath());
    }
}
