# Building jar2dll

## Prerequisites

- Windows 10/11.
- Visual Studio C++ Build Tools with MSBuild and a Windows SDK.
- .NET SDK required by the C# projects.
- A JDK with `JAVA_HOME` set. Its architecture must match the target native
  architecture.
- Gradle dependencies for `java/readBytesJar` (the wrapper is included).

## Build a release package

From the repository root:

```bat
package-builder.bat
```

This creates `dist/builder/` and `dist/gui/`. Both directories contain the
memory rewriter beside the executable; keep those files together.

## Component builds

```powershell
dotnet build csharp\builder\Jar2Dll.Builder.csproj
dotnet build csharp\gui\Jar2Dll.Gui.csproj
```

```bat
cd java\readBytesJar
gradlew.bat test memoryRewriterJar
```

See `cpp/jni-tests/README.md` for native mapper tests. Generated output belongs
in ignored build/cache directories, never in source directories.
