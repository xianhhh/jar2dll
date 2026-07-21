# Architecture

## Packaging flow

```text
Input JAR + libraries
        |
        v
C# builder (j2d.exe) ---- memory mode ----> ASM JAR rewriter
        |                                      |
        v                                      v
metadata + JAR payload + native bundle --> Windows JNI bootstrap DLL
                                                   |
                                                   v
JVM loads DLL -> defines classes -> invokes configured static entry
```

## Memory-mode JNI rewriting

Memory mode recursively finds `.dll` resources in the input JAR. A DLL resource
is indexed by its file name with and without the `.dll` suffix, case-insensitively.

The ASM rewriter removes these direct literal calls only when the string names an
indexed JAR resource:

```java
System.load("native/win64/bridge.dll");
System.loadLibrary("bridge");
Runtime.getRuntime().load("bridge.dll");
Runtime.getRuntime().loadLibrary("bridge");
```

The bytecode pattern is intentionally strict:

- `System`: string `LDC` followed by `System.load` or `System.loadLibrary`.
- `Runtime`: `Runtime.getRuntime()`, string `LDC`, then `Runtime.load` or
  `Runtime.loadLibrary`.
- Labels, frames, and line-number nodes between instructions are ignored.

It does not rewrite dynamic strings, local variables, reflection, a `Runtime`
object saved in a variable, third-party library calls, or names with no matching
DLL resource. This is removal rather than API substitution: matched DLLs are
stored in `META-INF/jar2dll/memory-modules.n2dm` and mapped by native code.

## Native constraints

The in-memory mapper supports x64 PE32+ images, standard imports, delay-load
imports, and memory-to-memory dependencies. A 64-bit process cannot execute x86
code. Win32 in-memory mapping is not implemented.
