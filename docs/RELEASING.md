# Release checklist

1. Update user-facing documentation and the version/release notes.
2. Build the ASM rewriter and release package:

   ```bat
   package-builder.bat
   ```

3. Verify `dist/builder/j2d.exe` and `dist/gui/jar2dll-gui.exe` start, and that
   `memory-rewriter.jar` is beside both relevant executables.
4. Run the builder, GUI, and native/JNI regression checks applicable to the
   release.
5. Publish only source and intentional release archives. Do not commit local
   `dist/`, `Temp/`, `bin/`, `obj/`, or test build products to the source branch.
