# Jar2Dll integration test

[Repository README](../../README.md) · [Native test guide](../../cpp/jni-tests/README.md)

This consumer project validates the local annotation library, Gradle plugin,
builder, dump support, and native fixture classes.

From the repository root:

```bat
java\readBytesJar\gradlew.bat -p java\Test buildDll
dist\builder\j2d.exe -dump java\Test\build\libs\j2d-test.dll -o java\Test\build\libs\j2d-test-recovered.jar
java -cp java\Test\build\libs\j2d-test-recovered.jar xh.xianhhh.test.J2dTestMain
```

The last command prints:

```text
J2D_DUMP_JAR_OK
```

For memory-mode verification, use the native fixtures under `cpp/jni-tests` and
run their dedicated mapper suite. Memory mode currently requires an x64 JDK and
x64 native libraries.
