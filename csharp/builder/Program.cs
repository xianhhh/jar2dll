using System.Diagnostics;
using System.Buffers.Binary;
using System.IO.Compression;
using System.Reflection;
using System.Text;
using System.Text.Json;

internal static class Program
{
    private sealed class Options
    {
        public string Input = "";
        public string MainClass = "";
        public string Method = "load";
        public string MethodSignature = "()V";
        public string Argument = "";
        public bool HasArgument;
        public string Output = "";
        public string DumpInput = "";
        public string InspectInput = "";
        public string DumpConfigInput = "";
        public string RunDll = "";
        public readonly List<string> RunArguments = [];
        public string Configuration = "Release";
        public string Platform = "x64";
        public readonly List<string> Loaders = [];
        public readonly List<string> Libraries = [];
        public readonly List<string> NativeLibraries = [];
        public string NativeMode = "stable";
        public bool NativeDebug;
        public bool MemoryJarNatives = true;
        public readonly List<string> JvmArguments = [];
        // Keep all short-lived build/run files beside the invoking project by default.
        // J2D_TEMP and -temp remain explicit overrides for CI or a different drive.
        public string TempRoot = Environment.GetEnvironmentVariable("J2D_TEMP")
            ?? Path.Combine(Environment.CurrentDirectory, "Temp");
        public bool KeepTemp;
        public bool CanDump = true;
        public bool Detail;
    }

    private static readonly (string Resource, string RelativePath)[] NativeFiles =
    [
        ("Native.maindll.slnx", "maindll/maindll.slnx"),
        ("Native.maindll.dllmain.cpp", "maindll/maindll/dllmain.cpp"),
        ("Native.maindll.jar2dll.hpp", "maindll/maindll/jar2dll.hpp"),
        ("Native.maindll.jni_exports.hpp", "maindll/maindll/jni_exports.hpp"),
        ("Native.maindll.memory_pe.hpp", "maindll/maindll/memory_pe.hpp"),
        ("Native.maindll.memory_bundle.hpp", "maindll/maindll/memory_bundle.hpp"),
        ("Native.maindll.jniheader.h", "maindll/maindll/jniheader.h"),
        ("Native.maindll.framework.h", "maindll/maindll/framework.h"),
        ("Native.maindll.pch.h", "maindll/maindll/pch.h"),
        ("Native.maindll.pch.cpp", "maindll/maindll/pch.cpp"),
        ("Native.maindll.maindll.vcxproj", "maindll/maindll/maindll.vcxproj")
    ];
    private static readonly byte[] DumpMagic = Encoding.ASCII.GetBytes("J2D-CANDUMP-V2!!");
    private static readonly byte[] RunMagic = Encoding.ASCII.GetBytes("J2D-RUNNER-V2!!!");
    private static readonly byte[] BuildNonceMagic = Encoding.ASCII.GetBytes("J2D-NONCE-V1");
    // GUI and integrations verify this marker inside the executable rather than
    // trusting a mutable filename such as j2d.exe.
    private static readonly byte[] BuilderIdentityMagic = "J2D-BUILDER-IDENTITY-V1"u8.ToArray();
    private const string NativeBundleEntry = "META-INF/jar2dll/native-libraries.zip";
    private const string MemoryBundleEntry = "META-INF/jar2dll/memory-modules.n2dm";
    private sealed record MemoryModuleInput(string Name, Action<Stream> Write);
    private sealed record JarMemoryModule(string EntryName, string Name, byte[] Data);
    private sealed record MemoryBundleResult(string BundlePath);

    public static int Main(string[] args)
    {
        GC.KeepAlive(BuilderIdentityMagic);
        try
        {
            if (args.Length == 0 || args.Any(x => x is "-h" or "-help" or "--help" or "-?"))
            {
                Help();
                return 0;
            }

            Options options = Parse(args);
            if (!string.IsNullOrEmpty(options.DumpInput)) Dump(options);
            else if (!string.IsNullOrEmpty(options.InspectInput)) Inspect(options);
            else if (!string.IsNullOrEmpty(options.DumpConfigInput)) DumpConfig(options);
            else if (!string.IsNullOrEmpty(options.RunDll)) RunDll(options);
            else Build(options);
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine("[ERROR] " + exception.Message);
            return 1;
        }
    }

    private static Options Parse(string[] args)
    {
        var options = new Options();
        for (int i = 0; i < args.Length; ++i)
        {
            string key = args[i].ToLowerInvariant();
            string Next() => ++i < args.Length ? args[i] : throw new ArgumentException($"Missing value for {key}");
            switch (key)
            {
                case "-i": options.Input = Path.GetFullPath(Next()); break;
                case "-run":
                    string runTarget = Next().Trim();
                    if (runTarget.EndsWith(".dll", StringComparison.OrdinalIgnoreCase)) options.RunDll = Path.GetFullPath(runTarget);
                    else options.MainClass = runTarget;
                    break;
                case "-method": case "-m": options.Method = Next().Trim(); break;
                case "-signature": options.MethodSignature = Next().Trim(); break;
                case "-argument": options.Argument = Next(); options.HasArgument = true; break;
                case "-argument-file": options.Argument = File.ReadAllText(Path.GetFullPath(Next()), Encoding.UTF8); options.HasArgument = true; break;
                case "-o": options.Output = Path.GetFullPath(Next()); break;
                case "-dump": options.DumpInput = Path.GetFullPath(Next()); break;
                case "-inspect": options.InspectInput = Path.GetFullPath(Next()); break;
                case "-dump-config": options.DumpConfigInput = Path.GetFullPath(Next()); break;
                case "-args": options.RunArguments.Add(Next()); break;
                case "-args-file":
                    string argumentFile = Path.GetFullPath(Next());
                    if (!File.Exists(argumentFile)) throw new FileNotFoundException("Argument file not found", argumentFile);
                    options.RunArguments.Add(File.ReadAllText(argumentFile, Encoding.UTF8));
                    break;
                case "-jvmarg": options.JvmArguments.Add(Next()); break;
                case "-candump":
                    if (!bool.TryParse(Next(), out options.CanDump))
                        throw new ArgumentException("-candump must be true or false");
                    break;
                case "-detail": options.Detail = true; break;
                case "-configuration": options.Configuration = Next(); break;
                case "-platform": options.Platform = Next(); break;
                case "-cld":
                    foreach (string value in Next().Split([',', ';', '|'], StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
                        if (!options.Loaders.Contains(value)) options.Loaders.Add(value);
                    break;
                case "-lib": options.Libraries.Add(Path.GetFullPath(Next())); break;
                case "-lib-list":
                    string libraryList = Path.GetFullPath(Next());
                    if (!File.Exists(libraryList)) throw new FileNotFoundException("Library list not found", libraryList);
                    foreach (string library in File.ReadLines(libraryList, Encoding.UTF8)) {
                        if (!string.IsNullOrWhiteSpace(library)) options.Libraries.Add(Path.GetFullPath(library.Trim()));
                    }
                    break;
                case "-lib-dir":
                    string libraryDirectory = Path.GetFullPath(Next());
                    if (!Directory.Exists(libraryDirectory)) throw new DirectoryNotFoundException("Library directory not found: " + libraryDirectory);
                    options.Libraries.AddRange(Directory.EnumerateFiles(libraryDirectory, "*.jar", SearchOption.AllDirectories));
                    break;
                case "-native-lib": options.NativeLibraries.Add(Path.GetFullPath(Next())); break;
                case "-native-lib-list":
                    string nativeLibraryList = Path.GetFullPath(Next());
                    if (!File.Exists(nativeLibraryList)) throw new FileNotFoundException("Native library list not found", nativeLibraryList);
                    foreach (string nativeLibrary in File.ReadLines(nativeLibraryList, Encoding.UTF8)) {
                        if (!string.IsNullOrWhiteSpace(nativeLibrary)) options.NativeLibraries.Add(Path.GetFullPath(nativeLibrary.Trim()));
                    }
                    break;
                case "-native-lib-dir":
                    string nativeLibraryDirectory = Path.GetFullPath(Next());
                    if (!Directory.Exists(nativeLibraryDirectory)) throw new DirectoryNotFoundException("Native library directory not found: " + nativeLibraryDirectory);
                    options.NativeLibraries.AddRange(Directory.EnumerateFiles(nativeLibraryDirectory, "*.dll", SearchOption.AllDirectories));
                    break;
                case "-native-mode": options.NativeMode = Next().Trim().ToLowerInvariant(); break;
                case "-native-debug": options.NativeDebug = true; break;
                case "-memory-jar-natives":
                    if (!bool.TryParse(Next(), out options.MemoryJarNatives)) throw new ArgumentException("-memory-jar-natives must be true or false");
                    break;
                case "-temp": options.TempRoot = Path.GetFullPath(Next()); break;
                case "-keep-temp": options.KeepTemp = true; break;
                default: throw new ArgumentException("Unknown option: " + args[i]);
            }
        }

        if (options.Detail && string.IsNullOrEmpty(options.InspectInput))
            throw new ArgumentException("-detail can only be used with -inspect");
        if (!string.IsNullOrEmpty(options.RunDll))
        {
            if (!File.Exists(options.RunDll)) throw new FileNotFoundException("Input DLL not found", options.RunDll);
            if (!string.IsNullOrEmpty(options.Input) || !string.IsNullOrEmpty(options.DumpInput) || !string.IsNullOrEmpty(options.InspectInput))
                throw new ArgumentException("-run <dll> cannot be combined with build, dump, or inspect options");
            return options;
        }
        if (options.RunArguments.Count > 0)
            throw new ArgumentException("-args and -args-file can only be used with -run <dll>");
        if (!string.IsNullOrEmpty(options.DumpConfigInput)) {
            if (!File.Exists(options.DumpConfigInput)) throw new FileNotFoundException("Input DLL not found", options.DumpConfigInput);
            if (string.IsNullOrEmpty(options.Output)) options.Output = Path.ChangeExtension(options.DumpConfigInput, ".dll.json");
            return options;
        }
        if (!string.IsNullOrEmpty(options.DumpInput) || !string.IsNullOrEmpty(options.InspectInput))
        {
            string dll = string.IsNullOrEmpty(options.DumpInput) ? options.InspectInput : options.DumpInput;
            if (!string.IsNullOrEmpty(options.DumpInput) && !string.IsNullOrEmpty(options.InspectInput))
                throw new ArgumentException("-dump and -inspect cannot be combined");
            if (!File.Exists(dll)) throw new FileNotFoundException("Input DLL not found", dll);
            if (!string.IsNullOrEmpty(options.Input) || !string.IsNullOrEmpty(options.MainClass))
                throw new ArgumentException("-dump or -inspect cannot be combined with -i or -run");
            if (!string.IsNullOrEmpty(options.DumpInput) && string.IsNullOrEmpty(options.Output))
                options.Output = Path.Combine(
                    Path.GetDirectoryName(dll)!,
                    Path.GetFileNameWithoutExtension(dll) + "-dump.jar");
            return options;
        }

        if (string.IsNullOrEmpty(options.Output)) options.Output = Path.GetFullPath("jar2dll.dll");
        if (!File.Exists(options.Input)) throw new FileNotFoundException("Input JAR not found", options.Input);
        foreach (string library in options.Libraries) {
            if (!File.Exists(library)) throw new FileNotFoundException("Library JAR not found", library);
        }
        foreach (string nativeLibrary in options.NativeLibraries) {
            if (!File.Exists(nativeLibrary)) throw new FileNotFoundException("Native library DLL not found", nativeLibrary);
        }
        if (options.NativeMode is not ("stable" or "memory"))
            throw new ArgumentException("-native-mode must be stable or memory");
        if (options.NativeMode == "memory" && options.Platform != "x64")
            throw new ArgumentException("-native-mode memory supports only -platform x64");
        if (string.IsNullOrWhiteSpace(options.MainClass)) throw new ArgumentException("-run is required");
        if (string.IsNullOrWhiteSpace(options.Method)) throw new ArgumentException("-method cannot be empty");
        if (options.MethodSignature is not ("()V" or "([Ljava/lang/String;)V"))
            throw new ArgumentException("-signature must be ()V or ([Ljava/lang/String;)V");
        if (options.HasArgument && options.MethodSignature != "([Ljava/lang/String;)V")
            throw new ArgumentException("-argument and -argument-file require the ([Ljava/lang/String;)V signature");
        if (options.Configuration is not ("Debug" or "Release")) throw new ArgumentException("-configuration must be Debug or Release");
        if (options.Platform is not ("x64" or "Win32")) throw new ArgumentException("-platform must be x64 or Win32");
        return options;
    }

    private static void Build(Options options)
    {
        if (options.NativeMode == "memory")
        {
            WriteMemoryDiagnostics(options);
        }
        string? javaHome = Environment.GetEnvironmentVariable("JAVA_HOME");
        if (string.IsNullOrWhiteSpace(javaHome) || !Directory.Exists(javaHome))
            throw new InvalidOperationException("JAVA_HOME is not configured");
        javaHome = Path.GetFullPath(javaHome);
        if (!File.Exists(Path.Combine(javaHome, "include", "jni.h")))
            throw new InvalidOperationException("JAVA_HOME does not point to a JDK (include\\jni.h is missing)");
        if (!File.Exists(Path.Combine(javaHome, "lib", "jvm.lib")))
            throw new InvalidOperationException("JAVA_HOME does not contain lib\\jvm.lib for the selected JDK");

        string tempRoot = string.IsNullOrWhiteSpace(options.TempRoot) ? Path.Combine(Environment.CurrentDirectory, "Temp") : options.TempRoot;
        Directory.CreateDirectory(tempRoot);
        string root = Path.Combine(tempRoot, "jar2dll-build-" + Guid.NewGuid().ToString("N"));
        string nativeProjectRoot = Path.Combine(tempRoot, "native-project-cache");
        Directory.CreateDirectory(root);
        try
        {
            Console.WriteLine("[1/3] Preparing embedded project...");
            EnsureNativeProjectCache(nativeProjectRoot);
            string nativeDir = Path.Combine(nativeProjectRoot, "maindll", "maindll");
            string blob = Path.Combine(root, "readJar.j2df");
            string effectiveInput = options.Input;
            if (options.NativeMode == "memory" && options.MemoryJarNatives) {
                effectiveInput = RewriteMemoryLoadCalls(root, options.Input, javaHome);
            }
            string? nativeBundle = options.NativeMode == "stable"
                ? CreateNativeBundle(root, options.NativeLibraries)
                : null;
            MemoryBundleResult? memoryBundle = options.NativeMode == "memory"
                ? CreateMemoryNativeBundle(root, options, options.Input)
                : null;
            if (memoryBundle is not null) {
                int moduleCount = ValidateMemoryNativeBundle(memoryBundle.BundlePath);
                Console.Error.WriteLine($"[memory] Packed and validated {moduleCount} in-memory native module(s), {new FileInfo(memoryBundle.BundlePath).Length:N0} bytes.");
            }
            BuildJ2df(
                [effectiveInput, .. options.Libraries.Distinct(StringComparer.OrdinalIgnoreCase)],
                blob,
                nativeBundle,
                BuildConfigJson(options),
                memoryBundle is null ? null : [(MemoryBundleEntry, memoryBundle.BundlePath)]);
            string payload = Path.Combine(root, "readJar.payload");
            WriteResourcePayload(blob, payload, options.CanDump, options);
            string mainClass = WriteConfig(root, "mainClass.cfg", options.MainClass);
            string mainMethod = WriteConfig(root, "mainMethod.cfg", options.Method);
            string mainSignature = WriteConfig(root, "mainSignature.cfg", options.MethodSignature);
            string mainArgument = WriteConfig(root, "mainArgument.cfg", options.Argument);
            string hasArgument = WriteConfig(root, "mainHasArgument.cfg", options.HasArgument ? "true" : "false");
            string classLoaders = WriteConfig(root, "classLoaders.cfg", string.Join(';', options.Loaders));
            GenerateResourceScript(Path.Combine(nativeDir, "readJar_resources.rc"),
                (payload, 101), (mainClass, 102), (mainMethod, 103), (mainSignature, 104),
                (mainArgument, 105), (hasArgument, 106), (classLoaders, 107));

            Console.WriteLine("[2/3] Building native DLL...");
            string msbuild = FindMsBuild();
            RunMsBuild(msbuild, nativeProjectRoot, options, javaHome);

            string dll = options.Platform == "x64"
                ? Path.Combine(nativeProjectRoot, "maindll", "x64", options.Configuration, "maindll.dll")
                : Path.Combine(nativeProjectRoot, "maindll", options.Configuration, "maindll.dll");
            if (!File.Exists(dll)) throw new FileNotFoundException("MSBuild did not produce the DLL", dll);

            Console.WriteLine("[3/3] Writing output...");
            Directory.CreateDirectory(Path.GetDirectoryName(options.Output)!);
            File.Copy(dll, options.Output, true);
            if (options.NativeLibraries.Count > 0) {
                string strategy = options.NativeMode == "memory" ? "memory-mapped module bundle" : "compressed bundle";
                Console.WriteLine("[INFO] Embedded " + options.NativeLibraries.Count + " native DLL(s) as a " + strategy + "; no DLLs were copied beside the output.");
            }
            Console.WriteLine("[OK] " + options.Output);
            if (options.KeepTemp) Console.WriteLine("[TEMP] " + root);
        }
        finally
        {
            if (!options.KeepTemp)
                try { Directory.Delete(root, true); } catch { }
        }
    }

    private static void WriteMemoryDiagnostics(Options options)
    {
        Console.Error.WriteLine("[memory] Experimental in-memory native mode selected.");
        Console.Error.WriteLine("[memory] Native modules will be packed into the DLL and mapped by the experimental x64 runtime loader.");
        if (!options.NativeDebug) return;

        Console.Error.WriteLine("[memory][debug] Input DLL diagnostics:");
        if (options.NativeLibraries.Count == 0) {
            Console.Error.WriteLine("[memory][debug]   No -native-lib inputs were supplied.");
            return;
        }
        foreach (string path in options.NativeLibraries.Distinct(StringComparer.OrdinalIgnoreCase))
        {
            try {
                using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
                using var reader = new BinaryReader(stream, Encoding.UTF8, true);
                if (stream.Length < 0x40 || reader.ReadUInt16() != 0x5A4D) throw new InvalidDataException("missing MZ header");
                stream.Position = 0x3C;
                int peOffset = reader.ReadInt32();
                if (peOffset < 0 || peOffset > stream.Length - 24) throw new InvalidDataException("invalid PE header offset");
                stream.Position = peOffset;
                if (reader.ReadUInt32() != 0x00004550) throw new InvalidDataException("missing PE signature");
                ushort machine = reader.ReadUInt16();
                ushort sections = reader.ReadUInt16();
                stream.Position += 12; // timestamp, symbols and optional-header size follows
                ushort optionalSize = reader.ReadUInt16();
                stream.Position += 2; // characteristics
                if (optionalSize < 60 || stream.Position + optionalSize > stream.Length) throw new InvalidDataException("invalid optional header");
                long optionalStart = stream.Position;
                ushort magic = reader.ReadUInt16();
                stream.Position = optionalStart + 56;
                uint imageSize = reader.ReadUInt32();
                string architecture = machine switch { 0x8664 => "x64", 0x014C => "x86", 0xAA64 => "ARM64", _ => "0x" + machine.ToString("X4") };
                Console.Error.WriteLine($"[memory][debug]   {Path.GetFileName(path)}: {architecture}, PE{(magic == 0x20B ? "32+" : magic == 0x10B ? "32" : "?" )}, {sections} section(s), image={imageSize:N0} bytes, file={stream.Length:N0} bytes");
                long sectionTable = optionalStart + optionalSize;
                for (int index = 0; index < sections && sectionTable + (index + 1L) * 40 <= stream.Length; ++index) {
                    stream.Position = sectionTable + index * 40L;
                    string name = Encoding.ASCII.GetString(reader.ReadBytes(8)).TrimEnd('\0');
                    uint virtualSize = reader.ReadUInt32();
                    uint rva = reader.ReadUInt32();
                    uint rawSize = reader.ReadUInt32();
                    uint rawOffset = reader.ReadUInt32();
                    Console.Error.WriteLine($"[memory][debug]     {name,-8} rva=0x{rva:X8} virtual={virtualSize:N0} raw={rawSize:N0}@0x{rawOffset:X8}");
                }
            }
            catch (Exception exception) {
                Console.Error.WriteLine($"[memory][debug]   {Path.GetFileName(path)}: invalid/unreadable PE: {exception.Message}");
            }
        }
        if (options.MemoryJarNatives) {
            Console.Error.WriteLine("[memory][debug] Embedded JAR native candidates:");
            using ZipArchive archive = ZipFile.OpenRead(options.Input);
            foreach (ZipArchiveEntry entry in archive.Entries.Where(entry => entry.FullName.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))) {
                Console.Error.WriteLine($"[memory][debug]   {entry.FullName}: {entry.Length:N0} bytes → reserved for memory mapping (not resource extraction)");
            }
        }
        List<string> nativeLoadCallsites = FindNativeLoadCallsites([options.Input, .. options.Libraries]);
        Console.Error.WriteLine("[memory][debug] Native load callsite candidates: " + nativeLoadCallsites.Count);
        foreach (string callsite in nativeLoadCallsites.Take(64)) Console.Error.WriteLine("[memory][debug]   " + callsite);
        if (nativeLoadCallsites.Count > 64) Console.Error.WriteLine("[memory][debug]   … " + (nativeLoadCallsites.Count - 64) + " more");
    }

    private static List<string> FindNativeLoadCallsites(IEnumerable<string> jarPaths)
    {
        var matches = new List<string>();
        foreach (string jarPath in jarPaths.Distinct(StringComparer.OrdinalIgnoreCase)) {
            using ZipArchive archive = ZipFile.OpenRead(jarPath);
            foreach (ZipArchiveEntry entry in archive.Entries.Where(entry => entry.FullName.EndsWith(".class", StringComparison.Ordinal))) {
                using Stream input = entry.Open(); using var data = new MemoryStream(); input.CopyTo(data);
                ReadOnlySpan<byte> bytes = data.GetBuffer().AsSpan(0, checked((int)data.Length));
                bool runtime = bytes.IndexOf("java/lang/Runtime"u8) >= 0;
                bool system = bytes.IndexOf("java/lang/System"u8) >= 0;
                bool load = bytes.IndexOf("loadLibrary"u8) >= 0 || bytes.IndexOf("\0load\0"u8) >= 0;
                if ((runtime || system) && load) matches.Add(Path.GetFileName(jarPath) + "!" + entry.FullName);
            }
        }
        return matches;
    }

    private static IReadOnlyList<JarMemoryModule> FindLoadedJarNativeModules(string jarPath)
    {
        using ZipArchive archive = ZipFile.OpenRead(jarPath);
        ZipArchiveEntry[] dllEntries = archive.Entries
            .Where(entry => !string.IsNullOrEmpty(entry.Name) && entry.FullName.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
            .ToArray();
        if (dllEntries.Length == 0) return [];

        var referencedNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (ZipArchiveEntry classEntry in archive.Entries.Where(entry => entry.FullName.EndsWith(".class", StringComparison.Ordinal))) {
            HashSet<string> constants = ReadClassUtf8Constants(classEntry);
            if (!constants.Contains("java/lang/System") && !constants.Contains("java/lang/Runtime")) continue;
            if (!constants.Contains("load") && !constants.Contains("loadLibrary")) continue;
            foreach (ZipArchiveEntry dll in dllEntries) {
                string fileName = Path.GetFileName(dll.FullName);
                string libraryName = Path.GetFileNameWithoutExtension(fileName);
                foreach (string constant in constants) {
                    if (IsNativeLibraryReference(constant, fileName, libraryName)) {
                        referencedNames.Add(dll.FullName);
                        break;
                    }
                }
            }
        }

        var selected = new List<JarMemoryModule>();
        foreach (ZipArchiveEntry entry in dllEntries) {
            if (!referencedNames.Contains(entry.FullName)) continue;
            if (entry.Length > uint.MaxValue) throw new InvalidDataException("JAR native DLL is too large: " + entry.FullName);
            using Stream input = entry.Open();
            using var data = new MemoryStream(checked((int)Math.Min(entry.Length, 16 * 1024 * 1024)));
            input.CopyTo(data, 1024 * 1024);
            selected.Add(new JarMemoryModule(entry.FullName, Path.GetFileName(entry.FullName), data.ToArray()));
        }
        return selected;
    }

    private static bool IsNativeLibraryReference(string value, string fileName, string libraryName)
    {
        if (string.Equals(value, libraryName, StringComparison.OrdinalIgnoreCase)
            || string.Equals(value, fileName, StringComparison.OrdinalIgnoreCase)) return true;
        return value.EndsWith("/" + fileName, StringComparison.OrdinalIgnoreCase)
            || value.EndsWith("\\" + fileName, StringComparison.OrdinalIgnoreCase);
    }

    private static HashSet<string> ReadClassUtf8Constants(ZipArchiveEntry entry)
    {
        try {
            using Stream input = entry.Open(); using var data = new MemoryStream(); input.CopyTo(data);
            ReadOnlySpan<byte> bytes = data.GetBuffer().AsSpan(0, checked((int)data.Length));
            if (bytes.Length < 10 || BinaryPrimitives.ReadUInt32BigEndian(bytes) != 0xCAFEBABE) return [];
            int position = 8;
            int count = BinaryPrimitives.ReadUInt16BigEndian(bytes[position..]); position += 2;
            var values = new HashSet<string>(StringComparer.Ordinal);
            for (int index = 1; index < count; ++index) {
                if (position >= bytes.Length) return [];
                byte tag = bytes[position++];
                int size = tag switch { 3 or 4 => 4, 5 or 6 => 8, 7 or 8 or 16 or 19 or 20 => 2, 9 or 10 or 11 or 12 or 17 or 18 => 4, 15 => 3, _ => -1 };
                if (tag == 1) {
                    if (bytes.Length - position < 2) return [];
                    int length = BinaryPrimitives.ReadUInt16BigEndian(bytes[position..]); position += 2;
                    if (length > bytes.Length - position) return [];
                    values.Add(Encoding.UTF8.GetString(bytes.Slice(position, length))); position += length;
                }
                else {
                    if (size < 0 || size > bytes.Length - position) return [];
                    position += size;
                    if (tag is 5 or 6) ++index;
                }
            }
            return values;
        }
        catch (InvalidDataException) { return []; }
    }

    private static string WriteConfig(string root, string fileName, string value)
    {
        string path = Path.Combine(root, fileName);
        // A zero-length RCDATA resource can make LockResource return null and
        // be mistaken for a missing configuration value by the native loader.
        File.WriteAllText(path, string.IsNullOrEmpty(value) ? "\n" : value, new UTF8Encoding(false));
        return path;
    }

    private static void ExtractNativeProject(string root)
    {
        Assembly assembly = Assembly.GetExecutingAssembly();
        foreach ((string resource, string relative) in NativeFiles)
        {
            string output = Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar));
            Directory.CreateDirectory(Path.GetDirectoryName(output)!);
            using Stream input = assembly.GetManifestResourceStream(resource)
                ?? throw new InvalidOperationException("Missing embedded resource: " + resource);
            using FileStream file = File.Create(output);
            input.CopyTo(file);
        }
    }

    private static void EnsureNativeProjectCache(string root)
    {
        string marker = Path.Combine(root, ".jar2dll-native-cache-version");
        string version = Assembly.GetExecutingAssembly().ManifestModule.ModuleVersionId.ToString("N");
        string solution = Path.Combine(root, "maindll", "maindll.slnx");
        if (File.Exists(solution) && File.Exists(marker)
            && string.Equals(File.ReadAllText(marker).Trim(), version, StringComparison.Ordinal)) return;
        Directory.CreateDirectory(root);
        ExtractNativeProject(root);
        File.WriteAllText(marker, version, new UTF8Encoding(false));
    }

    private static string RewriteMemoryLoadCalls(string root, string input, string javaHome)
    {
        string rewriter = Path.Combine(AppContext.BaseDirectory, "memory-rewriter.jar");
        if (!File.Exists(rewriter))
            throw new FileNotFoundException("memory mode requires memory-rewriter.jar beside jar2dll-builder.exe", rewriter);
        string output = Path.Combine(root, "memory-rewritten-input.jar");
        var info = new ProcessStartInfo(Path.Combine(javaHome, "bin", "java.exe")) {
            UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true
        };
        info.ArgumentList.Add("-jar"); info.ArgumentList.Add(rewriter);
        info.ArgumentList.Add("--input"); info.ArgumentList.Add(input);
        info.ArgumentList.Add("--output"); info.ArgumentList.Add(output);
        using Process process = Process.Start(info) ?? throw new InvalidOperationException("Could not start the memory bytecode rewriter");
        string standardOutput = process.StandardOutput.ReadToEnd();
        string standardError = process.StandardError.ReadToEnd();
        process.WaitForExit();
        if (process.ExitCode != 0 || !File.Exists(output))
            throw new InvalidOperationException("Memory bytecode rewriting failed" + (string.IsNullOrWhiteSpace(standardError) ? "" : ": " + standardError.Trim()));
        if (!string.IsNullOrWhiteSpace(standardOutput)) Console.Error.WriteLine("[memory] " + standardOutput.Trim());
        return output;
    }

    private static string? CreateNativeBundle(string root, IReadOnlyList<string> nativeLibraries)
    {
        string[] files = nativeLibraries.Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
        if (files.Length == 0) return null;
        foreach (IGrouping<string, string> collision in files.GroupBy(path => Path.GetFileName(path) ?? path, StringComparer.OrdinalIgnoreCase)) {
            if (collision.Count() > 1)
                throw new InvalidDataException("Native DLL names must be unique when bundled: " + collision.Key);
        }
        string bundle = Path.Combine(root, "native-libraries.zip");
        using var archive = ZipFile.Open(bundle, ZipArchiveMode.Create);
        foreach (string file in files) {
            ZipArchiveEntry entry = archive.CreateEntry(Path.GetFileName(file), CompressionLevel.SmallestSize);
            using Stream input = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read, 1024 * 1024, FileOptions.SequentialScan);
            using Stream output = entry.Open();
            input.CopyTo(output, 1024 * 1024);
        }
        return bundle;
    }

    private static MemoryBundleResult? CreateMemoryNativeBundle(string root, Options options, string sourceJar)
    {
        var modules = new List<MemoryModuleInput>();
        foreach (string file in options.NativeLibraries.Distinct(StringComparer.OrdinalIgnoreCase)) {
            if (!ShouldMapMemoryModule(file, Path.GetFileName(file))) continue;
            string name = Path.GetFileName(file);
            modules.Add(new MemoryModuleInput(name, output => {
                using var input = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read, 1024 * 1024, FileOptions.SequentialScan);
                input.CopyTo(output, 1024 * 1024);
            }));
        }
        if (options.MemoryJarNatives) {
            IReadOnlyList<JarMemoryModule> jarModules = FindLoadedJarNativeModules(sourceJar);
            foreach (JarMemoryModule module in jarModules) {
                if (!ShouldMapMemoryModule(module.Data, module.EntryName)) continue;
                modules.Add(new MemoryModuleInput(module.Name, output => output.Write(module.Data)));
                Console.Error.WriteLine($"[memory] Selected JAR native resource {module.EntryName} for {module.Name}.");
            }
        }
        if (modules.Count == 0) return null;
        foreach (IGrouping<string, MemoryModuleInput> collision in modules.GroupBy(module => module.Name, StringComparer.OrdinalIgnoreCase))
            if (collision.Count() > 1) throw new InvalidDataException("Memory module names must be unique: " + collision.Key);
        string path = Path.Combine(root, "memory-modules.n2dm");
        using var stream = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None, 1024 * 1024);
        using var writer = new BinaryWriter(stream, Encoding.UTF8, true);
        writer.Write("N2DM"u8); writer.Write((byte)1); writer.Write((uint)modules.Count);
        foreach (MemoryModuleInput module in modules) {
            string name = module.Name;
            byte[] nameBytes = Encoding.UTF8.GetBytes(name);
            writer.Write((uint)nameBytes.Length); writer.Write(nameBytes);
            long lengthPosition = stream.Position; writer.Write(0U);
            long start = stream.Position; module.Write(stream); long end = stream.Position;
            if (end - start > uint.MaxValue) throw new InvalidDataException("Memory module is too large: " + name);
            stream.Position = lengthPosition; writer.Write((uint)(end - start)); stream.Position = end;
        }
        return new MemoryBundleResult(path);
    }

    private static bool ShouldMapMemoryModule(string path, string displayName)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        return ShouldMapMemoryModule(stream, displayName);
    }

    private static bool ShouldMapMemoryModule(byte[] data, string displayName)
    {
        using var stream = new MemoryStream(data, writable: false);
        return ShouldMapMemoryModule(stream, displayName);
    }

    private static bool ShouldMapMemoryModule(Stream stream, string displayName)
    {
        using var reader = new BinaryReader(stream, Encoding.UTF8, true);
        if (stream.Length < 0x40) throw new InvalidDataException("Memory module is too small to be a PE DLL: " + displayName);
        stream.Position = 0;
        if (reader.ReadUInt16() != 0x5A4D) throw new InvalidDataException("Memory module is missing its MZ header: " + displayName);
        stream.Position = 0x3C;
        int peOffset = reader.ReadInt32();
        if (peOffset < 0 || peOffset > stream.Length - 26) throw new InvalidDataException("Memory module has an invalid PE header offset: " + displayName);
        stream.Position = peOffset;
        if (reader.ReadUInt32() != 0x00004550) throw new InvalidDataException("Memory module is missing its PE signature: " + displayName);
        ushort machine = reader.ReadUInt16();
        stream.Position = peOffset + 24;
        ushort magic = reader.ReadUInt16();
        if (machine == 0x8664 && magic == 0x20B) return true;
        if (machine == 0x014C && magic == 0x10B) {
            Console.Error.WriteLine($"[memory] Skipping x86 DLL {displayName}: an x64 JVM cannot load or map 32-bit native code.");
            return false;
        }
        throw new InvalidDataException($"Memory module has an unsupported architecture (machine=0x{machine:X4}, magic=0x{magic:X}): {displayName}");
    }

    private static int ValidateMemoryNativeBundle(string path)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        using var reader = new BinaryReader(stream, Encoding.UTF8, true);
        if (stream.Length < 9 || Encoding.ASCII.GetString(reader.ReadBytes(4)) != "N2DM" || reader.ReadByte() != 1)
            throw new InvalidDataException("Generated memory native bundle has an invalid header");
        uint count = reader.ReadUInt32();
        if (count > (stream.Length - stream.Position) / 8)
            throw new InvalidDataException("Generated memory native bundle has an invalid entry count");
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        for (uint index = 0; index < count; ++index) {
            uint nameLength = reader.ReadUInt32();
            if (nameLength == 0 || nameLength > stream.Length - stream.Position || nameLength > int.MaxValue)
                throw new InvalidDataException("Generated memory native bundle has an invalid module name");
            string name = Encoding.UTF8.GetString(reader.ReadBytes((int)nameLength));
            if (!names.Add(name)) throw new InvalidDataException("Generated memory native bundle has a duplicate module name: " + name);
            uint payloadLength = reader.ReadUInt32();
            if (payloadLength < 2 || payloadLength > stream.Length - stream.Position)
                throw new InvalidDataException("Generated memory native bundle has an invalid payload: " + name);
            long payloadStart = stream.Position;
            ValidateMemoryModulePe(stream, payloadStart, payloadLength, name);
            stream.Position = payloadStart + payloadLength;
        }
        if (stream.Position != stream.Length) throw new InvalidDataException("Generated memory native bundle has trailing data");
        return checked((int)count);
    }

    private static void ValidateMemoryModulePe(Stream stream, long payloadStart, uint payloadLength, string name)
    {
        if (payloadLength < 0x100) throw new InvalidDataException("Memory module is too small to be a PE DLL: " + name);
        using var reader = new BinaryReader(stream, Encoding.UTF8, true);
        stream.Position = payloadStart;
        if (reader.ReadUInt16() != 0x5A4D) throw new InvalidDataException("Memory module is missing its MZ header: " + name);
        stream.Position = payloadStart + 0x3C;
        int peOffset = reader.ReadInt32();
        if (peOffset < 0 || peOffset > payloadLength - 24) throw new InvalidDataException("Memory module has an invalid PE header offset: " + name);
        stream.Position = payloadStart + peOffset;
        if (reader.ReadUInt32() != 0x00004550) throw new InvalidDataException("Memory module is missing its PE signature: " + name);
        if (reader.ReadUInt16() != 0x8664) throw new InvalidDataException("Memory module is not x64: " + name);
        ushort sectionCount = reader.ReadUInt16();
        stream.Position += 12;
        ushort optionalSize = reader.ReadUInt16();
        stream.Position += 2;
        const int optionalHeader64Size = 240;
        if (sectionCount == 0 || optionalSize < optionalHeader64Size || peOffset > payloadLength - 24 - optionalSize)
            throw new InvalidDataException("Memory module has an invalid PE32+ optional header: " + name);
        long optionalStart = stream.Position;
        if (reader.ReadUInt16() != 0x20B) throw new InvalidDataException("Memory module is not PE32+: " + name);
        stream.Position = optionalStart + 56;
        if (reader.ReadUInt32() == 0) throw new InvalidDataException("Memory module has an empty image: " + name);
        const int dataDirectoryOffset = 112;
        bool HasDirectory(int index) {
            stream.Position = optionalStart + dataDirectoryOffset + index * 8;
            uint rva = reader.ReadUInt32();
            uint size = reader.ReadUInt32();
            return rva != 0 || size != 0;
        }
        if (HasDirectory(14)) throw new InvalidDataException("Managed memory modules are not supported: " + name);
    }

    private static void BuildJ2df(IReadOnlyList<string> jarPaths, string outputPath, string? nativeBundle = null, byte[]? buildConfig = null, IReadOnlyList<(string EntryName, string FilePath)>? extraResources = null)
    {
        // Stream entries directly to disk. The previous implementation retained every
        // ZipArchive and entry descriptor until the end, which made large dependency
        // sets consume unnecessary memory and file handles.
        var seenClasses = new HashSet<string>(StringComparer.Ordinal);
        var seenResources = new HashSet<string>(StringComparer.Ordinal);
        int skippedDuplicateClasses = 0;
        const int bufferSize = 1024 * 1024;
        using var stream = new FileStream(outputPath, new FileStreamOptions
        {
            Mode = FileMode.Create,
            Access = FileAccess.Write,
            Share = FileShare.None,
            BufferSize = bufferSize,
            Options = FileOptions.SequentialScan
        });
        using var output = new BinaryWriter(stream, Encoding.UTF8, true);
        output.Write(new byte[] { (byte)'J', (byte)'2', (byte)'D', (byte)'F', 1 });
        output.Write(0U); // Replaced with the final count after the streaming pass.
        uint entryCount = 0;

        foreach (string jarPath in jarPaths)
        {
            using ZipArchive archive = ZipFile.OpenRead(jarPath);
            foreach (ZipArchiveEntry entry in archive.Entries.Where(entry => !string.IsNullOrEmpty(entry.Name)))
            {
                bool isClass = entry.FullName.EndsWith(".class", StringComparison.Ordinal);
                if (isClass) {
                    // Match normal classpath semantics: the primary JAR and then the
                    // first declared dependency win. Choosing the largest class was
                    // non-deterministic with regard to intended dependency versions.
                    if (!seenClasses.Add(entry.FullName)) {
                        ++skippedDuplicateClasses;
                        continue;
                    }
                }
                else if (!seenResources.Add(entry.FullName)) {
                    continue;
                }
                byte[] name = Encoding.UTF8.GetBytes(entry.FullName);
                if (entry.Length > uint.MaxValue) throw new InvalidDataException("JAR entry is too large: " + entry.FullName + " in " + jarPath);
                output.Write((byte)(isClass ? 1 : 2));
                output.Write((uint)name.Length);
                output.Write(name);
                output.Write((uint)entry.Length);
                using Stream input = entry.Open();
                input.CopyTo(output.BaseStream, bufferSize);
                checked { ++entryCount; }
            }
        }
        if (nativeBundle is not null) {
            byte[] name = Encoding.UTF8.GetBytes(NativeBundleEntry);
            long length = new FileInfo(nativeBundle).Length;
            if (length > uint.MaxValue) throw new InvalidDataException("Native DLL bundle is too large");
            output.Write((byte)2);
            output.Write((uint)name.Length);
            output.Write(name);
            output.Write((uint)length);
            using Stream input = new FileStream(nativeBundle, FileMode.Open, FileAccess.Read, FileShare.Read, bufferSize, FileOptions.SequentialScan);
            input.CopyTo(output.BaseStream, bufferSize);
            checked { ++entryCount; }
        }
        if (buildConfig is not null) {
            byte[] name = Encoding.UTF8.GetBytes("META-INF/jar2dll/build-config.json");
            output.Write((byte)2); output.Write((uint)name.Length); output.Write(name); output.Write((uint)buildConfig.Length); output.Write(buildConfig);
            checked { ++entryCount; }
        }
        foreach ((string EntryName, string FilePath) resource in extraResources ?? []) {
            byte[] name = Encoding.UTF8.GetBytes(resource.EntryName);
            long length = new FileInfo(resource.FilePath).Length;
            if (length > uint.MaxValue) throw new InvalidDataException("Embedded resource is too large: " + resource.EntryName);
            output.Write((byte)2); output.Write((uint)name.Length); output.Write(name); output.Write((uint)length);
            using Stream input = new FileStream(resource.FilePath, FileMode.Open, FileAccess.Read, FileShare.Read, bufferSize, FileOptions.SequentialScan);
            input.CopyTo(output.BaseStream, bufferSize);
            checked { ++entryCount; }
        }
        output.Flush();
        stream.Position = 5;
        output.Write(entryCount);
        if (skippedDuplicateClasses > 0) {
            Console.WriteLine("[INFO] Kept the first classpath occurrence; skipped "
                + skippedDuplicateClasses + " duplicate class file(s)");
        }
    }

    private static byte[] BuildConfigJson(Options options) => JsonSerializer.SerializeToUtf8Bytes(new {
        schema = "jar2dll.build-config/v1", inputJar = Path.GetFileName(options.Input), entryPoint = new { className = options.MainClass, methodName = options.Method, descriptor = options.MethodSignature, hasArgument = options.HasArgument, argument = options.Argument },
        classLoaders = options.Loaders, libraries = options.Libraries.Select(Path.GetFileName).ToArray(), nativeLibraries = options.NativeLibraries.Select(Path.GetFileName).ToArray(),
        platform = options.Platform, configuration = options.Configuration, nativeMode = options.NativeMode, memoryJarNatives = options.MemoryJarNatives, canDump = options.CanDump, jvmArguments = options.JvmArguments
    }, new JsonSerializerOptions { WriteIndented = true });

    private static void GenerateResourceScript(string outputPath, params (string Path, int Id)[] resources)
    {
        using var writer = new StreamWriter(outputPath, false, new UTF8Encoding(false));
        writer.WriteLine("#include <windows.h>");
        foreach ((string path, int id) in resources)
        {
            string escaped = Path.GetFullPath(path).Replace("\\", "\\\\").Replace("\"", "\\\"");
            writer.WriteLine($"{id} RCDATA \"{escaped}\"");
        }
    }

    private static void WriteResourcePayload(string blobPath, string payloadPath, bool canDump, Options options)
    {
        using var output = new FileStream(payloadPath, FileMode.Create, FileAccess.Write, FileShare.None, 1024 * 1024);
        using (var input = new FileStream(blobPath, FileMode.Open, FileAccess.Read, FileShare.Read, 1024 * 1024, FileOptions.SequentialScan)) {
            input.CopyTo(output, 1024 * 1024);
        }
        uint payloadLength = checked((uint)new FileInfo(blobPath).Length);
        if (canDump) {
            output.Write(DumpMagic);
            Span<byte> length = stackalloc byte[sizeof(uint)];
            BinaryPrimitives.WriteUInt32LittleEndian(length, payloadLength);
            output.Write(length);
        }
        output.Write(RunMagic);
        Span<byte> payloadLengthBytes = stackalloc byte[sizeof(uint)];
        BinaryPrimitives.WriteUInt32LittleEndian(payloadLengthBytes, payloadLength);
        output.Write(payloadLengthBytes);
        WriteFooterString(output, options.MainClass);
        WriteFooterString(output, options.Method);
        WriteFooterString(output, options.MethodSignature);
        WriteFooterString(output, options.Argument);
        output.WriteByte(options.HasArgument ? (byte)1 : (byte)0);
        output.Write(BitConverter.GetBytes(options.JvmArguments.Count));
        foreach (string jvmArgument in options.JvmArguments) WriteFooterString(output, jvmArgument);
        Span<byte> nonce = stackalloc byte[16];
        System.Security.Cryptography.RandomNumberGenerator.Fill(nonce);
        output.Write(BuildNonceMagic);
        output.Write(nonce);
    }

    private static void WriteFooterString(Stream output, string value)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(value);
        Span<byte> length = stackalloc byte[sizeof(uint)];
        BinaryPrimitives.WriteUInt32LittleEndian(length, checked((uint)bytes.Length));
        output.Write(length);
        output.Write(bytes);
    }

    private static void Dump(Options options)
    {
        byte[] dll = File.ReadAllBytes(options.DumpInput);
        ReadOnlySpan<byte> blob = FindDumpBlob(dll);
        MemoryBundleInfo? memory = FindMemoryBundle(blob);
        DumpStatistics statistics = WriteJar(blob, options.Output);
        Console.WriteLine($"[OK] Rebuilt JAR with {statistics.ClassCount} class file(s) and {statistics.ResourceCount} resource(s): {options.Output}");
        if (memory is not null) Console.WriteLine($"[OK] Omitted {memory.ModuleCount} internal memory native module(s) from the recovered JAR.");
    }

    private static void DumpConfig(Options options)
    {
        ReadOnlySpan<byte> blob = FindDumpBlob(File.ReadAllBytes(options.DumpConfigInput));
        int position = 9; uint count = BinaryPrimitives.ReadUInt32LittleEndian(blob[5..]);
        for (uint index = 0; index < count; ++index) {
            byte type = blob[position++]; uint nameLength = BinaryPrimitives.ReadUInt32LittleEndian(blob.Slice(position, 4)); position += 4;
            string name = Encoding.UTF8.GetString(blob.Slice(position, checked((int)nameLength))); position += checked((int)nameLength);
            uint length = BinaryPrimitives.ReadUInt32LittleEndian(blob.Slice(position, 4)); position += 4;
            if (type == 2 && name == "META-INF/jar2dll/build-config.json") { File.WriteAllBytes(options.Output, blob.Slice(position, checked((int)length)).ToArray()); Console.WriteLine("[OK] " + options.Output); return; }
            position += checked((int)length);
        }
        throw new InvalidDataException("This DLL was built before build-config metadata was introduced.");
    }

    private static void Inspect(Options options)
    {
        byte[] dll = File.ReadAllBytes(options.InspectInput);
        ReadOnlySpan<byte> blob = FindDumpBlob(dll);
        MemoryBundleInfo? memory = FindMemoryBundle(blob);
        DumpStatistics statistics = AnalyzeBlob(
            blob,
            options.Detail
                ? (type, name) => Console.WriteLine(name == MemoryBundleEntry ? "  [memory bundle] " + name : type == 1 ? "  [class] " + name : "  [resource] " + name)
                : null);
        Console.WriteLine("[OK] Valid jar2dll dump marker");
        Console.WriteLine("  J2DF payload: " + statistics.PayloadBytes + " bytes");
        Console.WriteLine("  Classes: " + statistics.ClassCount);
        Console.WriteLine("  Resources: " + statistics.ResourceCount);
        Console.WriteLine("  Class bytes: " + statistics.ClassBytes);
        Console.WriteLine("  Resource bytes: " + statistics.ResourceBytes);
        if (memory is not null) {
            Console.WriteLine("  Memory native modules: " + memory.ModuleCount + " (" + memory.TotalBytes + " bytes)");
            if (options.Detail) foreach ((string name, uint size) in memory.Modules) Console.WriteLine("    [memory dll] " + name + " (" + size + " bytes)");
        }
    }

    private static void RunDll(Options options)
    {
        byte[] dll = File.ReadAllBytes(options.RunDll);
        RunMetadata metadata = ReadRunMetadata(dll);
        string tempRoot = string.IsNullOrWhiteSpace(options.TempRoot) ? Path.Combine(Environment.CurrentDirectory, "Temp") : options.TempRoot;
        Directory.CreateDirectory(tempRoot);
        string directory = Path.Combine(tempRoot, "jar2dll-run-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        string runner = Path.Combine(directory, "J2dInMemoryRunner.java");
        try
        {
            File.WriteAllText(runner, InMemoryRunnerSource, new UTF8Encoding(false));
            string? javaHome = Environment.GetEnvironmentVariable("JAVA_HOME");
            string java = !string.IsNullOrWhiteSpace(javaHome)
                ? Path.Combine(javaHome, "bin", "java.exe") : "java.exe";
            if (Path.IsPathFullyQualified(java) && !File.Exists(java))
                throw new FileNotFoundException("JAVA_HOME does not contain bin\\java.exe", java);
            var info = new ProcessStartInfo(java)
            {
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            foreach (string jvmArgument in metadata.JvmArguments.Concat(options.JvmArguments).Distinct()) info.ArgumentList.Add(jvmArgument);
            info.ArgumentList.Add(runner);
            info.ArgumentList.Add(options.RunDll);
            info.ArgumentList.Add(metadata.ClassName);
            info.ArgumentList.Add(metadata.MethodName);
            info.ArgumentList.Add(metadata.Signature);
            info.ArgumentList.Add(metadata.HasArgument ? "true" : "false");
            info.ArgumentList.Add(metadata.Argument);
            info.ArgumentList.Add(directory);
            foreach (string argument in options.RunArguments) info.ArgumentList.Add(argument);
            using Process process = Process.Start(info) ?? throw new InvalidOperationException("Could not start Java");
            Task<string> standardOutput = process.StandardOutput.ReadToEndAsync();
            Task<string> standardError = process.StandardError.ReadToEndAsync();
            process.WaitForExit();
            Task.WaitAll(standardOutput, standardError);
            if (!string.IsNullOrEmpty(standardOutput.Result)) Console.Out.Write(standardOutput.Result);
            if (!string.IsNullOrEmpty(standardError.Result)) Console.Error.Write(standardError.Result);
            if (process.ExitCode != 0)
                throw new InvalidOperationException("Running the recovered JAR failed with exit code " + process.ExitCode
                    + ". Check the embedded class path and dependencies.");
        }
        finally
        {
            try { Directory.Delete(directory, true); } catch { }
        }
    }

    private static ReadOnlySpan<byte> FindDumpBlob(byte[] dll)
    {
        for (int footerStart = dll.Length - DumpMagic.Length - sizeof(uint); footerStart >= 0; --footerStart)
        {
            if (!dll.AsSpan(footerStart, DumpMagic.Length).SequenceEqual(DumpMagic)) continue;
            uint length = BinaryPrimitives.ReadUInt32LittleEndian(dll.AsSpan(footerStart + DumpMagic.Length));
            if (length > footerStart) continue;
            int blobStart = footerStart - checked((int)length);
            ReadOnlySpan<byte> blob = dll.AsSpan(blobStart, (int)length);
            if (blob.Length >= 9 && blob[..5].SequenceEqual("J2DF\x01"u8)) return blob;
        }
        throw new InvalidDataException("This DLL does not contain a jar2dll dump marker. Rebuild it with -candump true.");
    }

    private static RunMetadata ReadRunMetadata(byte[] dll)
    {
        for (int start = dll.Length - RunMagic.Length - sizeof(uint); start >= 0; --start) {
            if (!dll.AsSpan(start, RunMagic.Length).SequenceEqual(RunMagic)) continue;
            int position = start + RunMagic.Length + sizeof(uint);
            try {
                string className = ReadFooterString(dll, ref position);
                string methodName = ReadFooterString(dll, ref position);
                string signature = ReadFooterString(dll, ref position);
                string argument = ReadFooterString(dll, ref position);
                if (position >= dll.Length) continue;
                bool hasArgument = dll[position] != 0;
                ++position;
                if (dll.Length - position < sizeof(int)) continue;
                int jvmArgumentCount = BinaryPrimitives.ReadInt32LittleEndian(dll.AsSpan(position));
                position += sizeof(int);
                if (jvmArgumentCount is < 0 or > 1024) continue;
                var jvmArguments = new List<string>(jvmArgumentCount);
                for (int i = 0; i < jvmArgumentCount; ++i) jvmArguments.Add(ReadFooterString(dll, ref position));
                if (string.IsNullOrWhiteSpace(className) || string.IsNullOrWhiteSpace(methodName)
                    || signature is not ("()V" or "([Ljava/lang/String;)V")) continue;
                return new RunMetadata(className, methodName, signature, hasArgument, argument, jvmArguments);
            }
            catch (InvalidDataException) { }
        }
        throw new InvalidDataException("This DLL lacks run metadata. Rebuild it with the current j2d.exe.");
    }

    private static string ReadFooterString(byte[] data, ref int position)
    {
        if (data.Length - position < sizeof(uint)) throw new InvalidDataException("Dump metadata is truncated");
        uint length = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(position));
        position += sizeof(uint);
        if (length > data.Length - position) throw new InvalidDataException("Dump metadata is truncated");
        string value = Encoding.UTF8.GetString(data, position, (int)length);
        position += (int)length;
        return value;
    }

    private static DumpStatistics WriteJar(ReadOnlySpan<byte> blob, string outputPath)
    {
        int position = 0;
        if (blob.Length < 9 || !blob[..5].SequenceEqual("J2DF\x01"u8))
            throw new InvalidDataException("Embedded J2DF data is invalid");
        position = 5;
        uint count = ReadUInt32(blob, ref position);
        if (count > (blob.Length - position) / 9)
            throw new InvalidDataException("Embedded J2DF entry count is invalid");

        string? outputDirectory = Path.GetDirectoryName(Path.GetFullPath(outputPath));
        if (!string.IsNullOrEmpty(outputDirectory)) Directory.CreateDirectory(outputDirectory);
        var names = new HashSet<string>(StringComparer.Ordinal);
        var statistics = new DumpStatistics(blob.Length);
        using var output = new FileStream(outputPath, FileMode.Create, FileAccess.Write, FileShare.None);
        using var archive = new ZipArchive(output, ZipArchiveMode.Create, false, Encoding.UTF8);
        for (uint index = 0; index < count; ++index)
        {
            byte type = ReadByte(blob, ref position);
            uint nameLength = ReadUInt32(blob, ref position);
            string name = Encoding.UTF8.GetString(ReadBytes(blob, ref position, nameLength));
            uint dataLength = ReadUInt32(blob, ref position);
            ReadOnlySpan<byte> data = ReadBytes(blob, ref position, dataLength);
            if (type is not (1 or 2) || !IsSafeJarEntryName(name) || !names.Add(name))
                throw new InvalidDataException("Embedded J2DF contains an invalid or duplicate entry: " + name);
            if (type == 2 && name == MemoryBundleEntry) continue;
            ZipArchiveEntry entry = archive.CreateEntry(name, CompressionLevel.Optimal);
            using Stream entryStream = entry.Open();
            entryStream.Write(data);
            if (type == 1) { ++statistics.ClassCount; statistics.ClassBytes += dataLength; }
            else { ++statistics.ResourceCount; statistics.ResourceBytes += dataLength; }
        }
        if (position != blob.Length) throw new InvalidDataException("Embedded J2DF contains trailing data");
        return statistics;
    }

    private static bool IsSafeJarEntryName(string name)
    {
        if (string.IsNullOrEmpty(name) || name[0] == '/' || name.Contains('\\') || name.IndexOf('\0') >= 0)
            return false;
        return name.Split('/').All(component => component is not ("" or "." or ".."));
    }

    private static MemoryBundleInfo? FindMemoryBundle(ReadOnlySpan<byte> blob)
    {
        if (blob.Length < 9 || !blob[..5].SequenceEqual("J2DF\x01"u8)) throw new InvalidDataException("Embedded J2DF data is invalid");
        int position = 5; uint count = ReadUInt32(blob, ref position);
        if (count > (blob.Length - position) / 9) throw new InvalidDataException("Embedded J2DF entry count is invalid");
        for (uint index = 0; index < count; ++index) {
            byte type = ReadByte(blob, ref position); uint nameLength = ReadUInt32(blob, ref position);
            string name = Encoding.UTF8.GetString(ReadBytes(blob, ref position, nameLength));
            uint dataLength = ReadUInt32(blob, ref position); ReadOnlySpan<byte> data = ReadBytes(blob, ref position, dataLength);
            if (type == 2 && name == MemoryBundleEntry) return ParseMemoryBundle(data);
        }
        return null;
    }

    private static MemoryBundleInfo ParseMemoryBundle(ReadOnlySpan<byte> data)
    {
        if (data.Length < 9 || !data[..5].SequenceEqual("N2DM\x01"u8)) throw new InvalidDataException("Embedded memory native bundle is invalid");
        int position = 5; uint count = ReadUInt32(data, ref position);
        if (count > (data.Length - position) / 8) throw new InvalidDataException("Embedded memory native bundle entry count is invalid");
        var modules = new List<(string Name, uint Size)>(); long total = 0;
        for (uint index = 0; index < count; ++index) {
            uint nameLength = ReadUInt32(data, ref position);
            string name = Encoding.UTF8.GetString(ReadBytes(data, ref position, nameLength));
            uint size = ReadUInt32(data, ref position); _ = ReadBytes(data, ref position, size);
            if (string.IsNullOrWhiteSpace(name) || size == 0) throw new InvalidDataException("Embedded memory native bundle contains an invalid module");
            modules.Add((name, size)); total += size;
        }
        if (position != data.Length) throw new InvalidDataException("Embedded memory native bundle has trailing data");
        return new MemoryBundleInfo(count, total, modules);
    }

    private static DumpStatistics AnalyzeBlob(ReadOnlySpan<byte> blob, Action<byte, string>? entryVisitor = null)
    {
        if (blob.Length < 9 || !blob[..5].SequenceEqual("J2DF\x01"u8))
            throw new InvalidDataException("Embedded J2DF data is invalid");
        int position = 5;
        uint count = ReadUInt32(blob, ref position);
        if (count > (blob.Length - position) / 9)
            throw new InvalidDataException("Embedded J2DF entry count is invalid");

        var statistics = new DumpStatistics(blob.Length);
        for (uint index = 0; index < count; ++index)
        {
            byte type = ReadByte(blob, ref position);
            uint nameLength = ReadUInt32(blob, ref position);
            ReadOnlySpan<byte> nameBytes = ReadBytes(blob, ref position, nameLength);
            uint dataLength = ReadUInt32(blob, ref position);
            _ = ReadBytes(blob, ref position, dataLength);
            if (type == 1) { ++statistics.ClassCount; statistics.ClassBytes += dataLength; }
            else if (type == 2) { ++statistics.ResourceCount; statistics.ResourceBytes += dataLength; }
            else throw new InvalidDataException("Embedded J2DF contains an unknown entry type");
            entryVisitor?.Invoke(type, Encoding.UTF8.GetString(nameBytes));
        }
        if (position != blob.Length) throw new InvalidDataException("Embedded J2DF contains trailing data");
        return statistics;
    }

    private static byte ReadByte(ReadOnlySpan<byte> data, ref int position)
    {
        if (position >= data.Length) throw new InvalidDataException("Embedded J2DF is truncated");
        return data[position++];
    }

    private static uint ReadUInt32(ReadOnlySpan<byte> data, ref int position)
    {
        if (data.Length - position < sizeof(uint)) throw new InvalidDataException("Embedded J2DF is truncated");
        uint value = BinaryPrimitives.ReadUInt32LittleEndian(data[position..]);
        position += sizeof(uint);
        return value;
    }

    private static ReadOnlySpan<byte> ReadBytes(ReadOnlySpan<byte> data, ref int position, uint length)
    {
        if (length > data.Length - position) throw new InvalidDataException("Embedded J2DF is truncated");
        ReadOnlySpan<byte> result = data.Slice(position, (int)length);
        position += (int)length;
        return result;
    }

    private static string FindMsBuild()
    {
        string? path = FindOnPath("msbuild.exe");
        if (path != null) return path;
        string x86 = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86);
        string vswhere = Path.Combine(x86, "Microsoft Visual Studio", "Installer", "vswhere.exe");
        if (File.Exists(vswhere))
        {
            var info = new ProcessStartInfo(vswhere) { RedirectStandardOutput = true, UseShellExecute = false };
            info.ArgumentList.Add("-latest"); info.ArgumentList.Add("-products"); info.ArgumentList.Add("*");
            info.ArgumentList.Add("-requires"); info.ArgumentList.Add("Microsoft.Component.MSBuild");
            info.ArgumentList.Add("-find"); info.ArgumentList.Add("MSBuild\\**\\Bin\\MSBuild.exe");
            using Process process = Process.Start(info)
                ?? throw new InvalidOperationException("Could not start vswhere.exe");
            string result = process.StandardOutput.ReadToEnd().Trim().Split('\n', StringSplitOptions.RemoveEmptyEntries).FirstOrDefault()?.Trim() ?? "";
            process.WaitForExit();
            if (File.Exists(result)) return result;
        }
        throw new FileNotFoundException("MSBuild.exe was not found");
    }

    private static string? FindOnPath(string file)
    {
        foreach (string folder in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(';'))
        {
            string candidate = Path.Combine(folder.Trim(), file);
            if (File.Exists(candidate)) return candidate;
        }
        return null;
    }

    private static void RunMsBuild(string msbuild, string root, Options options, string javaHome)
    {
        var info = new ProcessStartInfo(msbuild)
        {
            WorkingDirectory = root,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        info.ArgumentList.Add(Path.Combine(root, "maindll", "maindll.slnx"));
        // Resource IDs 101-107 point at a fresh per-build payload. Rebuild is
        // required here: MSBuild's VC resource tracking can otherwise retain a
        // previous .res while the cached project itself is correctly reused.
        info.ArgumentList.Add("/nologo"); info.ArgumentList.Add("/verbosity:minimal"); info.ArgumentList.Add("/t:Rebuild");
        info.ArgumentList.Add("/p:Configuration=" + options.Configuration);
        info.ArgumentList.Add("/p:Platform=" + options.Platform);
        string normalizedPath = info.Environment.FirstOrDefault(x => x.Key.Equals("PATH", StringComparison.OrdinalIgnoreCase)).Value ?? "";
        foreach (string key in info.Environment.Keys.Where(x => x.Equals("PATH", StringComparison.OrdinalIgnoreCase)).ToArray())
            info.Environment.Remove(key);
        info.Environment["Path"] = normalizedPath;
        info.Environment["TEMP"] = root;
        info.Environment["TMP"] = root;
        using Process process = Process.Start(info)
            ?? throw new InvalidOperationException("Could not start MSBuild.exe");
        Task<string> standardOutput = process.StandardOutput.ReadToEndAsync();
        Task<string> standardError = process.StandardError.ReadToEndAsync();
        process.WaitForExit();
        Task.WaitAll(standardOutput, standardError);
        if (process.ExitCode != 0)
        {
            string details = string.Join(Environment.NewLine, [standardOutput.Result, standardError.Result])
                .Trim();
            throw new InvalidOperationException(
                "MSBuild failed with exit code " + process.ExitCode
                + (string.IsNullOrEmpty(details) ? "" : Environment.NewLine + details));
        }
    }

    private static void Help() => Console.WriteLine("""
jar2dll-builder.exe -i <jar> -run <class> [options]
  -method <name>             Default: load
  -signature <descriptor>    ()V or ([Ljava/lang/String;)V; default: ()V
  -argument <text>           Pass text as args[0] to a String[] entry method
  -argument-file <file>      Pass UTF-8 file content as args[0]
  -cld <a,b,c>               Omit to use current thread context ClassLoader
  -lib <jar>                  Embed a dependency JAR; repeat for multiple libraries
  -lib-dir <directory>        Recursively embed every .jar under a directory
  -lib-list <file>            UTF-8 text file containing one library JAR path per line
  -native-lib <dll>           Embed a native DLL in a compressed bundle (no output-side copy)
  -native-lib-list <file>     UTF-8 text file containing one native DLL path per line
  -native-lib-dir <directory> Recursively embed every DLL under a directory
  -native-mode stable|memory  Native-library strategy; default: stable. Memory is experimental.
  -native-debug               Print experimental memory-mode PE diagnostics to stderr.
  -memory-jar-natives <bool>  In memory mode, reserve DLL resources in the input JAR for memory mapping (default: true).
  -jvmarg <argument>          JVM argument; repeatable (for example --add-opens or -Xmx2G)
  -temp <directory>           Working directory (default: .\\Temp; J2D_TEMP also overrides it)
  -o <dll>                   Default: jar2dll.dll
  -configuration Debug|Release
  -platform x64|Win32
  -candump true|false        Embed an opt-in class dump marker (default: true)
  -keep-temp

jar2dll-builder.exe -dump <dll> [-o <jar>]
  Rebuilds a JAR from a DLL built with -candump true.

jar2dll-builder.exe -inspect <dll> [-detail]
  Verifies a dump marker and prints embedded J2DF statistics without writing files.
  -detail also lists embedded class and resource names.

jar2dll-builder.exe -dump-config <dll> [-o <json>]
  Exports the embedded build configuration as JSON.

jar2dll-builder.exe -run <dll>
  Runs in memory; bundled native DLLs are extracted only to the selected Temp workspace.
  JVM arguments saved at build time and additional -jvmarg values are passed to Java.
  -args <text> may be repeated; -args-file <file> adds the complete file content.
""");

    private sealed class DumpStatistics(long payloadBytes)
    {
        public long PayloadBytes { get; } = payloadBytes;
        public uint ClassCount { get; set; }
        public uint ResourceCount { get; set; }
        public ulong ClassBytes { get; set; }
        public ulong ResourceBytes { get; set; }
    }

    private sealed record MemoryBundleInfo(uint ModuleCount, long TotalBytes, IReadOnlyList<(string Name, uint Size)> Modules);
    private sealed record RunMetadata(string ClassName, string MethodName, string Signature, bool HasArgument, string Argument, IReadOnlyList<string> JvmArguments);

    private const string InMemoryRunnerSource = """
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.lang.reflect.*;
import java.util.zip.*;

public class J2dInMemoryRunner {
  static final byte[] MAGIC = "J2D-RUNNER-V2!!!".getBytes(StandardCharsets.US_ASCII);
  static final String NATIVE_BUNDLE = "META-INF/jar2dll/native-libraries.zip";
  static final String MEMORY_BUNDLE = "META-INF/jar2dll/memory-modules.n2dm";
  static int u32(byte[] b, int p) { return (b[p]&255)|((b[p+1]&255)<<8)|((b[p+2]&255)<<16)|((b[p+3]&255)<<24); }
  static String text(byte[] b, int[] p) { int n=u32(b,p[0]); p[0]+=4; String s=new String(b,p[0],n,StandardCharsets.UTF_8); p[0]+=n; return s; }
  static class Loader extends ClassLoader { final Map<String,byte[]> classes; final Path nativeDir; Loader(Map<String,byte[]> c,Path d){super(ClassLoader.getSystemClassLoader());classes=c;nativeDir=d;} protected Class<?> findClass(String n)throws ClassNotFoundException{byte[] b=classes.get(n);if(b==null)throw new ClassNotFoundException(n);return defineClass(n,b,0,b.length);} protected String findLibrary(String n){Path f=nativeDir.resolve(System.mapLibraryName(n));return Files.isRegularFile(f)?f.toAbsolutePath().toString():null;} }
  static void extractNative(byte[] zip, Path dir) throws Exception { if(zip==null)return; Files.createDirectories(dir); try(ZipInputStream in=new ZipInputStream(new java.io.ByteArrayInputStream(zip))){for(ZipEntry e;(e=in.getNextEntry())!=null;){if(e.isDirectory())continue;Path out=dir.resolve(Path.of(e.getName()).getFileName().toString());try(java.io.OutputStream o=Files.newOutputStream(out)){in.transferTo(o);}}} }
  static void extractMemoryNative(byte[] bundle, Path dir) throws Exception { if(bundle==null)return; if(bundle.length<9||bundle[0]!='N'||bundle[1]!='2'||bundle[2]!='D'||bundle[3]!='M'||bundle[4]!=1)throw new IllegalArgumentException("Invalid memory native bundle"); Files.createDirectories(dir); int p=5,count=u32(bundle,p);p+=4;for(int i=0;i<count;i++){if(p>bundle.length-4)throw new IllegalArgumentException("Truncated memory native bundle");int n=u32(bundle,p);p+=4;if(n<=0||n>bundle.length-p)throw new IllegalArgumentException("Invalid memory module name");String name=new String(bundle,p,n,StandardCharsets.UTF_8);p+=n;if(p>bundle.length-4)throw new IllegalArgumentException("Truncated memory native bundle");int size=u32(bundle,p);p+=4;if(size<=0||size>bundle.length-p)throw new IllegalArgumentException("Invalid memory module payload");Path out=dir.resolve(Path.of(name).getFileName().toString());try(java.io.OutputStream o=Files.newOutputStream(out)){o.write(bundle,p,size);}p+=size;}if(p!=bundle.length)throw new IllegalArgumentException("Trailing memory native bundle data"); }
  public static void main(String[] a) throws Exception {
    byte[] dll=Files.readAllBytes(Path.of(a[0])); int mark=-1;
    outer: for(int i=dll.length-MAGIC.length-4;i>=0;i--){for(int j=0;j<MAGIC.length;j++)if(dll[i+j]!=MAGIC[j])continue outer;mark=i;break;}
    if(mark<0) throw new IllegalArgumentException("No jar2dll run metadata; rebuild the DLL with the current j2d.exe");
    int payload=u32(dll,mark+MAGIC.length), start=mark-payload;
    byte[] dump="J2D-CANDUMP-V2!!".getBytes(StandardCharsets.US_ASCII);
    int dumpStart=mark-dump.length-4;
    if(dumpStart>=0 && Arrays.equals(Arrays.copyOfRange(dll,dumpStart,dumpStart+dump.length),dump) && u32(dll,dumpStart+dump.length)==payload) start=dumpStart-payload;
    if(start<0) throw new IllegalArgumentException("Invalid embedded J2DF offset");
    int p=start; if(dll[p++]!='J'||dll[p++]!='2'||dll[p++]!='D'||dll[p++]!='F'||dll[p++]!=1)throw new IllegalArgumentException("Invalid J2DF");
    int count=u32(dll,p);p+=4; Map<String,byte[]> classes=new HashMap<>();byte[] nativeZip=null,memoryBundle=null;
    for(int i=0;i<count;i++){int type=dll[p++]&255,n=u32(dll,p);p+=4;String name=new String(dll,p,n,StandardCharsets.UTF_8);p+=n;int size=u32(dll,p);p+=4;if(type==1)classes.put(name.substring(0,name.length()-6).replace('/','.'),Arrays.copyOfRange(dll,p,p+size));else if(name.equals(NATIVE_BUNDLE))nativeZip=Arrays.copyOfRange(dll,p,p+size);else if(name.equals(MEMORY_BUNDLE))memoryBundle=Arrays.copyOfRange(dll,p,p+size);p+=size;}
    String cls=a[1], method=a[2], sig=a[3]; boolean has=Boolean.parseBoolean(a[4]); Path nativeDir=Path.of(a[6]);extractNative(nativeZip,nativeDir);extractMemoryNative(memoryBundle,nativeDir);List<String> args=new ArrayList<>();if(has)args.add(a[5]);for(int i=7;i<a.length;i++)args.add(a[i]);
    Loader loader=new Loader(classes,nativeDir); Thread.currentThread().setContextClassLoader(loader); Class<?> type=Class.forName(cls,true,loader);
    Method target=sig.equals("()V")?type.getDeclaredMethod(method):type.getDeclaredMethod(method,String[].class);target.setAccessible(true);if(sig.equals("()V"))target.invoke(null);else target.invoke(null,(Object)args.toArray(String[]::new));
  }
}
""";
}
