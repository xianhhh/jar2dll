# JNI and memory-mapper fixtures

[Repository README](../../README.md) · [Build guide](../../docs/BUILDING.md)

This directory contains native DLL fixtures and regression executables for the
experimental x64 memory loader.

Coverage includes PE validation, relocation, ordinary imports, delay-load
imports, internal DLL dependency ordering, exception tables, lifecycle calls,
N2DM parsing, JNI export naming, and malformed-input rejection.

## Build and run

Set `JAVA_HOME` to an x64 JDK, then run:

```powershell
./build-tests.ps1 -Configuration Debug
./run-memory-tests.ps1 -BuildDirectory ./build/Debug -StressRounds 1000
```

The expected suite marker is:

```text
J2D_MEMORY_TEST_SUITE_OK rounds=1000
```

## Three-DLL integration scenario

`run-memory-complex-integration.ps1` creates a JAR containing three DLL
resources: `j2d_chain_jni.dll` imports `j2d_chain_bridge.dll`, which imports
`j2d_chain_provider.dll`. It builds a memory-mode DLL, loads it into a fresh
JVM, and verifies the JNI call chain.

```powershell
./run-memory-complex-integration.ps1
```

Expected markers are `J2D_MEMORY_COMPLEX_CHAIN_OK` and
`J2D_MEMORY_COMPLEX_INTEGRATION_OK`.

`j2d_basic_native.dll` and `j2d_overload_native.dll` exercise direct JNI
exports. The chain and provider fixtures exercise memory-to-memory imports.
`j2d_lifecycle_fixture.dll` has a deliberately simple entry point for mapper
lifecycle tests; it is not a claim that arbitrary CRT-dependent third-party
DLL entry points are universally compatible.

The mapper currently executes PE32+ / x64 modules. x86 fixture support belongs
to a future Win32 mapper implementation and cannot run inside an x64 JVM.
