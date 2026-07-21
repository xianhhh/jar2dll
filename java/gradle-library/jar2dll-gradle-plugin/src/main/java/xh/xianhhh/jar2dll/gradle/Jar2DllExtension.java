package xh.xianhhh.jar2dll.gradle;

import org.gradle.api.file.RegularFileProperty;
import org.gradle.api.file.ConfigurableFileCollection;
import org.gradle.api.provider.ListProperty;
import org.gradle.api.provider.Property;

/** Configuration exposed as {@code jar2dll { ... }}. */
public abstract class Jar2DllExtension {
    /** Name of the generated DLL, relative to {@code build/libs}, unless {@link #getOutputFile()} is set. */
    public abstract Property<String> getOutputName();

    /** Target native platform accepted by the bundled builder: {@code x64} or {@code Win32}. */
    public abstract Property<String> getPlatform();

    /** Native build configuration: {@code Release} or {@code Debug}. */
    public abstract Property<String> getConfiguration();

    /** Whether the generated DLL includes the opt-in class dump marker. Defaults to {@code true}. */
    public abstract Property<Boolean> getCanDump();

    /** Candidate context ClassLoader implementation class names, in priority order. */
    public abstract ListProperty<String> getClassLoaders();

    /** Additional dependency JARs embedded with the application JAR. */
    public abstract ConfigurableFileCollection getLibraries();

    /** Optional external builder executable. Defaults to the executable bundled in this plugin. */
    public abstract RegularFileProperty getBuilderExecutable();

    /** Optional absolute or project-relative target DLL location. */
    public abstract RegularFileProperty getOutputFile();
}
