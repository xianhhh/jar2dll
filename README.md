# jar2dll

[中文文档](README_zh_CN.md) · [License](LICENSE) · [Contributing](CONTRIBUTING.md)

`jar2dll` packages a Java JAR, optional native libraries, and startup metadata
inside a Windows DLL. Loading that DLL in a JVM defines the packaged classes and
invokes a configured static entry method.

> Status: Windows-focused. Stable packaging is the default. The in-memory native
> loader is experimental and currently supports x64 PE32+ modules only.

## Features

- Build a Windows DLL from a JAR and invoke a `@DllMain` method or Java `main`.
- Bundle additional Java JARs, argument files, and JVM options.
- Embed native DLLs in stable ZIP or experimental in-memory (`N2DM`) mode.
- Inspect metadata, recover dump-enabled JARs, and run a generic smoke runner.
- Use the WPF GUI or the `j2d.exe` command-line builder.

## Quick start

Requirements: Windows, Visual Studio C++ Build Tools/MSBuild, and a JDK whose
architecture matches the native target. Set `JAVA_HOME` before building.

```bat
package-builder.bat
dist\builder\j2d.exe -i "app.jar" -run com.example.Main -o "app.dll"
```

For local development, the legacy-compatible PowerShell entry point remains
available:

```powershell
.\build.ps1 -i app.jar -run com.example.Main -o build\app.dll
```

## Repository layout

| Path | Purpose |
| --- | --- |
| `cpp/maindll/` | Native JNI bootstrap DLL and in-memory PE mapper. |
| `cpp/jni-tests/` | CMake native/JNI regression fixtures and integration tests. |
| `csharp/builder/` | `j2d.exe` command-line builder. |
| `csharp/gui/` | WPF desktop GUI. |
| `java/readBytesJar/` | JAR tools and ASM memory-load rewriter. |
| `java/gradle-library/` | Gradle plugin and annotations. |
| `java/Test/` | Java/JNI example and test fixtures. |
| `docs/` | Architecture, build, and release documentation. |
| `tools/` | Small helper scripts used by development workflows. |

Generated directories such as `dist/`, `build/`, `Temp/`, and language-specific
`bin/`, `obj/`, `.gradle/` output are intentionally ignored by Git.

## Native modes

`-native-mode stable` is the default: explicitly supplied native libraries are
stored in a ZIP bundle and extracted by the normal loader flow.

`-native-mode memory` embeds native modules in `META-INF/jar2dll/memory-modules.n2dm`.
It targets x64 only and supports PE32+, normal imports, delay-load imports, and
memory-to-memory native dependencies.

```bat
j2d.exe -i "app.jar" -run com.example.Main -native-mode memory ^
  -native-lib "native\example64.dll" -o "app.dll"
```

Read [the architecture guide](docs/ARCHITECTURE.md) for the exact memory-mode
load rewriting rules and limitations.

## CLI reference

```bat
j2d.exe -inspect "app.dll"
j2d.exe -inspect "app.dll" -detail
j2d.exe -dump "app.dll" -o "recovered.jar"
j2d.exe -dump-config "app.dll" -o "app.dll.json"
j2d.exe -run "app.dll"
```

`-run` is a recovery/smoke runner. It does not recreate an application's own
launcher environment, assets, authentication, or launcher arguments.

## Development

- [Build guide](docs/BUILDING.md)
- [Architecture and memory mode](docs/ARCHITECTURE.md)
- [Release checklist](docs/RELEASING.md)
- [Contributing guide](CONTRIBUTING.md)
- [Security policy](SECURITY.md)

## License

The project source is licensed under the [GNU GPL v3.0 only](LICENSE). This
means distributed modified versions and derivative works must also be provided
under GPLv3 with their corresponding source code. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for bundled and build-time
third-party components.
