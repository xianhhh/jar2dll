# Contributing

Thanks for contributing to jar2dll.

## Before opening a pull request

1. Keep changes focused and explain user-visible behavior in the PR body.
2. Do not commit generated content: `dist/`, `build/`, `Temp/`, `bin/`, `obj/`,
   `.gradle/`, CMake output, DLLs, EXEs, or PDBs.
3. Add or update a focused test when changing the native mapper, the builder,
   or the ASM rewriter.
4. Build the touched component and include the command/result in the PR.

## Development rules

- Maintain compatibility with the existing `build.ps1` and `package-builder.bat`
  entry points unless a migration is documented.
- Keep stable native packaging as the default mode.
- Treat memory mode as x64 experimental code. Do not claim x86 in-memory PE
  mapping support until it is implemented and tested.
- Avoid application-specific compatibility logic in the generic builder.

## Useful checks

```powershell
dotnet build csharp\builder\Jar2Dll.Builder.csproj --no-restore
dotnet build csharp\gui\Jar2Dll.Gui.csproj --no-restore
```

Native/JNI test instructions are in `cpp/jni-tests/README.md`.
