# Jar2Dll Gradle library

[Repository README](../../README.md) · [Build guide](../../docs/BUILDING.md)

The Gradle library publishes:

- `xh.xianhhh:jar2dll:1.0.0`: the `@DllMain` annotation.
- `xh.xianhhh.jar2dll`: the `buildDll` task and builder integration.

## Consumer project

```groovy
plugins {
    id 'java'
    id 'xh.xianhhh.jar2dll' version '1.0.0'
}

dependencies {
    implementation 'xh.xianhhh:jar2dll:1.0.0'
}

jar2dll {
    outputName = 'my-plugin.dll'
    platform = 'x64'
    configuration = 'Release'
    canDump = true
    classLoaders = ['com.example.GameClassLoader']
    libraries.from(fileTree('libs') { include '*.jar' })
}
```

```java
import xh.xianhhh.jar2dll.DllMain;

public final class Bootstrap {
    @DllMain
    public static void start() {
        // Invoked asynchronously after the generated DLL is loaded by a JVM.
    }
}
```

Run `gradlew buildDll`; the output is written to `build/libs`.

`@DllMain` must occur exactly once in the main source set and must mark a static
void method with no arguments. Building needs Windows, MSBuild, and a JDK matching
the configured target platform. The plugin does not enable memory mode; use the
`j2d.exe` command line when memory packaging is required.

When `canDump` is enabled, use `j2d.exe -inspect` or `j2d.exe -dump` to inspect
or recover embedded JAR data.
