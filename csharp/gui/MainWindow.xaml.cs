using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using Microsoft.Win32;

namespace Jar2Dll.Gui;

public partial class MainWindow : Window
{
    private List<MethodCandidate> entryCandidates = [];
    private readonly Dictionary<(DependencyObject, DependencyProperty), string> originalLabels = [];
    private readonly HashSet<string> missingTranslationKeys = [];
    private Dictionary<string, string> translations = LoadLocale("en_us");
    private static readonly Dictionary<string, string> Chinese = new()
    {
        ["Build"] = "构建", ["Inspect & Recover"] = "检查与恢复", ["Run"] = "运行",
        ["Browse"] = "浏览", ["Build DLL"] = "构建 DLL", ["Run embedded JAR"] = "运行内嵌 JAR",
        ["Inspect details"] = "查看详情", ["Recover JAR"] = "恢复 JAR", ["Clear"] = "清空",
        ["Save as…"] = "另存为…", ["Add JARs…"] = "添加 JAR…", ["Add DLLs…"] = "添加 DLL…",
        ["Scan folder"] = "扫描目录", ["Remove"] = "移除", ["File…"] = "文件…", ["Workspace…"] = "工作目录…",
        ["stable (recommended)"] = "稳定模式（推荐）", ["memory (experimental)"] = "内存模式（实验性）",
        ["Platform"] = "平台", ["Configuration"] = "配置", ["Source DLL"] = "源 DLL",
        ["Recovered JAR"] = "恢复的 JAR", ["Arguments"] = "参数", ["Argument file"] = "参数文件",
        ["Activity log"] = "活动日志", ["Output"] = "输出", ["Native DLLs"] = "原生 DLL"
        , ["Build Java payloads, inspect output, and control experimental native packaging."] = "构建 Java 载荷、检查输出并管理实验性原生打包。"
        , ["1 · Java entry"] = "1 · Java 入口", ["2 · Java dependencies"] = "2 · Java 依赖", ["3 · Output"] = "3 · 输出", ["4 · Native libraries"] = "4 · 原生库", ["5 · Build workspace"] = "5 · 构建工作区"
        , ["Drop a JAR anywhere in this window to scan entry points."] = "将 JAR 拖入窗口即可扫描入口点。", ["JAR…"] = "JAR…", ["Main class"] = "主类", ["Method"] = "方法", ["Detected entry"] = "检测到的入口", ["Signature"] = "签名", ["ClassLoaders (optional)"] = "类加载器（可选）", ["Argument text"] = "参数文本"
        , ["Allow DLL → JAR recovery"] = "允许 DLL → JAR 恢复", ["Memory mode is experimental · x64 only"] = "内存模式为实验性功能 · 仅 x64", ["Enable detailed PE diagnostics"] = "启用详细 PE 诊断", ["Build output and diagnostics"] = "构建输出与诊断", ["Memory mode · experimental"] = "内存模式 · 实验性", ["x64 only"] = "仅 x64"
        , ["Route JAR-contained DLL resources to Memory modules"] = "将 JAR 内 DLL 资源路由到内存模块", ["Extra JVM arguments"] = "额外 JVM 参数", ["English"] = "英语"
        , ["JAR DLL resources referenced by direct load calls are rewritten, embedded, and mapped in memory."] = "由直接加载调用引用的 JAR DLL 资源会被重写、嵌入并映射到内存中。"
    };
    public MainWindow()
    {
        InitializeComponent();
        BuilderPathTextBox.Text = FindBundledBuilder();
        TempDirectoryTextBox.Text = Path.Combine(Environment.CurrentDirectory, "Temp");
        if (MemoryOptionsPanel.Child is StackPanel panel && panel.Children.Count > 1
            && panel.Children[1] is TextBlock description) {
            description.Text = "JAR DLL resources referenced by direct load calls are rewritten, embedded, and mapped in memory.";
        }
    }

    private void LanguageSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!IsLoaded || LanguageComboBox.SelectedIndex < 0) return;
        bool chinese = LanguageComboBox.SelectedIndex == 1;
        translations = LoadLocale(chinese ? "zh_cn" : "en_us");
        // Some ComboBox and Tab visuals are virtualized while closed. Restore or
        // translate every label we have previously seen before walking the current
        // visual tree, so switching back never leaves stale translated text behind.
        foreach (var entry in originalLabels.ToArray()) {
            string value = translations.TryGetValue(entry.Value, out string? translated) ? translated : entry.Value;
            entry.Key.Item1.SetValue(entry.Key.Item2, value);
        }
        ApplyLanguage(this, chinese, []);
        Title = translations.TryGetValue("Jar2Dll Studio", out string? title) ? title : "Jar2Dll Studio";
    }

    private void ApplyLanguage(DependencyObject root, bool chinese, HashSet<DependencyObject> visited)
    {
        if (!visited.Add(root)) return;
        Localize(root, TextBlock.TextProperty, chinese);
        if (root is Button) Localize(root, ContentControl.ContentProperty, chinese);
        if (root is CheckBox) Localize(root, ContentControl.ContentProperty, chinese);
        if (root is ComboBoxItem) Localize(root, ContentControl.ContentProperty, chinese);
        if (root is TabItem) Localize(root, HeaderedContentControl.HeaderProperty, chinese);
        if (root is TabControl tabs) foreach (TabItem tab in tabs.Items.OfType<TabItem>()) ApplyLanguage(tab, chinese, visited);
        if (root is ComboBox combo) foreach (ComboBoxItem item in combo.Items.OfType<ComboBoxItem>()) ApplyLanguage(item, chinese, visited);
        if (root is FrameworkElement element) {
            foreach (object child in LogicalTreeHelper.GetChildren(element)) {
                if (child is DependencyObject logicalChild) ApplyLanguage(logicalChild, chinese, visited);
            }
        }
    }

    private void Localize(DependencyObject element, DependencyProperty property, bool chinese)
    {
        if (element.GetValue(property) is not string value || string.IsNullOrEmpty(value)) return;
        var key = (element, property);
        if (!originalLabels.TryGetValue(key, out string? original)) { original = value; originalLabels[key] = original; }
        if (!translations.TryGetValue(original, out string? translated)) {
            if (LanguageComboBox.SelectedIndex == 1 && missingTranslationKeys.Add(original))
                Debug.WriteLine("[i18n] Missing zh_cn.json key: " + original);
            translated = original;
        }
        element.SetValue(property, translated);
    }

    private static Dictionary<string, string> LoadLocale(string locale)
    {
        string resource = $"Jar2Dll.Gui.Locales.{locale}.json";
        using Stream stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(resource)
            ?? throw new InvalidOperationException("Missing UI locale resource: " + resource);
        return JsonSerializer.Deserialize<Dictionary<string, string>>(stream)
            ?? throw new InvalidOperationException("Invalid UI locale resource: " + resource);
    }

    private static string FindBundledBuilder()
    {
        string[] candidates =
        [
            Path.Combine(Environment.CurrentDirectory, "dist", "builder", "j2d.exe"),
            Path.Combine(AppContext.BaseDirectory, "j2d.exe"),
            Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "dist", "builder", "j2d.exe")
        ];
        return candidates.Select(Path.GetFullPath).FirstOrDefault(IsJar2DllBuilder) ?? candidates[0];
    }

    private void BrowseBuilder_Click(object sender, RoutedEventArgs e) => SelectFile(BuilderPathTextBox, "Jar2Dll builder|*.exe|Executable files|*.exe");
    private void BrowseJar_Click(object sender, RoutedEventArgs e) => SelectFile(InputJarTextBox, "JAR files|*.jar|All files|*.*");
    private void BrowseInspectDll_Click(object sender, RoutedEventArgs e) => SelectFile(InspectDllTextBox, "DLL files|*.dll|All files|*.*");
    private void BrowseRunDll_Click(object sender, RoutedEventArgs e) => SelectFile(RunDllTextBox, "DLL files|*.dll|All files|*.*");
    private void BrowseNativeLibraries_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Filter = "DLL files|*.dll|All files|*.*", Multiselect = true, CheckFileExists = true };
        if (dialog.ShowDialog(this) == true) NativeLibrariesTextBox.Text = string.Join(';', dialog.FileNames);
    }
    private void ScanNativeLibraryDirectory_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog { Title = "Select a directory containing native DLL dependencies" };
        if (dialog.ShowDialog(this) != true) return;
        try {
            string[] files = Directory.EnumerateFiles(dialog.FolderName, "*.dll", SearchOption.AllDirectories).ToArray();
            string[] existing = NativeLibrariesTextBox.Text.Split([';', '\r', '\n'], StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
            NativeLibrariesTextBox.Text = string.Join(';', existing.Concat(files).Distinct(StringComparer.OrdinalIgnoreCase));
            Log($"Selected {files.Length} native DLL file(s) under {dialog.FolderName}; they will be compressed into the output DLL.");
        }
        catch (Exception exception) { Log("Could not scan native DLL directory: " + exception.Message); }
    }
    private void BrowseTempDirectory_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog { Title = "Select a non-C drive workspace" };
        if (dialog.ShowDialog(this) == true) TempDirectoryTextBox.Text = dialog.FolderName;
    }
    private void BrowseArgumentFile_Click(object sender, RoutedEventArgs e) => SelectFile(ArgumentFileTextBox, "Text files|*.txt;*.json;*.cfg;*.properties|All files|*.*");
    private void BrowseLibraries_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Filter = "JAR files|*.jar|All files|*.*", Multiselect = true, CheckFileExists = true };
        if (dialog.ShowDialog(this) == true) AddLibraries(dialog.FileNames);
    }

    private void ScanLibraryDirectory_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog { Title = "Select a directory containing dependency JARs" };
        if (dialog.ShowDialog(this) != true) return;
        try {
            string[] files = Directory.EnumerateFiles(dialog.FolderName, "*.jar", SearchOption.AllDirectories).ToArray();
            AddLibraries(files);
            Log($"Scanned {files.Length} JAR file(s) under {dialog.FolderName}");
        }
        catch (Exception exception) { Log("Could not scan library directory: " + exception.Message); }
    }

    private void AddLibraries(IEnumerable<string> paths)
    {
        var known = LibraryListBox.Items.OfType<string>().ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (string path in paths.Select(Path.GetFullPath).Where(File.Exists)) {
            if (known.Add(path)) LibraryListBox.Items.Add(path);
        }
    }

    private void RemoveLibrary_Click(object sender, RoutedEventArgs e)
    {
        if (LibraryListBox.SelectedItem is not null) LibraryListBox.Items.Remove(LibraryListBox.SelectedItem);
    }

    private void ClearLibraries_Click(object sender, RoutedEventArgs e) => LibraryListBox.Items.Clear();

    private void BrowseRunArgumentFile_Click(object sender, RoutedEventArgs e) => SelectFile(RunArgumentFileTextBox, "Text files|*.txt;*.json;*.properties;*.cfg|All files|*.*");
    private void BrowseOutput_Click(object sender, RoutedEventArgs e) => SaveFile(OutputDllTextBox, "DLL files|*.dll", "jar2dll.dll");
    private void BrowseRecoveredJar_Click(object sender, RoutedEventArgs e) => SaveFile(RecoveredJarTextBox, "JAR files|*.jar", "recovered.jar");

    private async void LoadBuildConfig_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Filter = "Jar2Dll configuration|*.dll.json;*.json|JSON files|*.json|All files|*.*", CheckFileExists = true };
        if (dialog.ShowDialog(this) != true) return;
        try {
            using JsonDocument document = JsonDocument.Parse(await File.ReadAllTextAsync(dialog.FileName));
            ApplyBuildConfig(document.RootElement, dialog.FileName);
            Log("Loaded build configuration: " + dialog.FileName);
            if (File.Exists(InputJarTextBox.Text)) await DiscoverEntryPointAsync(InputJarTextBox.Text);
        }
        catch (Exception exception) {
            Log("Could not load configuration JSON: " + exception.Message);
            MessageBox.Show(this, exception.Message, "Jar2Dll Studio", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    private async void SaveLoadConfig_Click(object sender, RoutedEventArgs e)
    {
        if (!RequireText(InputJarTextBox.Text, "Input JAR")) return;
        var dialog = new SaveFileDialog {
            Filter = "Jar2Dll load configuration|*.json|All files|*.*",
            FileName = Path.GetFileNameWithoutExtension(InputJarTextBox.Text) + ".jar2dll.json",
            AddExtension = true
        };
        if (dialog.ShowDialog(this) != true) return;
        try {
            object config = CreateLoadConfig();
            await File.WriteAllTextAsync(dialog.FileName, JsonSerializer.Serialize(config, new JsonSerializerOptions { WriteIndented = true }), new UTF8Encoding(false));
            Log("Saved reusable load configuration: " + dialog.FileName);
        }
        catch (Exception exception) {
            Log("Could not save configuration JSON: " + exception.Message);
        }
    }

    private object CreateLoadConfig() => new {
        schema = "jar2dll.gui-load-config/v1",
        inputJar = AbsoluteOrOriginal(InputJarTextBox.Text),
        outputDll = AbsoluteOrOriginal(OutputDllTextBox.Text),
        entryPoint = new {
            className = MainClassTextBox.Text.Trim(), methodName = MainMethodTextBox.Text.Trim(),
            descriptor = Selected(MethodSignatureComboBox).Split(' ', 2)[0],
            hasArgument = !string.IsNullOrWhiteSpace(ArgumentFileTextBox.Text) || !string.IsNullOrEmpty(ArgumentTextBox.Text),
            argument = ArgumentTextBox.Text, argumentFile = AbsoluteOrOriginal(ArgumentFileTextBox.Text)
        },
        classLoaders = SplitValues(ClassLoadersTextBox.Text),
        libraries = CollectLibraryPaths().Select(AbsoluteOrOriginal).ToArray(),
        nativeLibraries = SplitValues(NativeLibrariesTextBox.Text).Select(AbsoluteOrOriginal).ToArray(),
        platform = Selected(PlatformComboBox), configuration = Selected(ConfigurationComboBox),
        nativeMode = SelectedTag(NativeModeComboBox), nativeDebug = NativeDebugCheckBox.IsChecked == true,
        memoryJarNatives = MemoryJarNativesCheckBox.IsChecked != false, canDump = CanDumpCheckBox.IsChecked == true,
        jvmArguments = SplitLines(JvmArgumentsTextBox.Text), workspace = AbsoluteOrOriginal(TempDirectoryTextBox.Text)
    };

    private void ApplyBuildConfig(JsonElement root, string configPath)
    {
        string schema = ReadString(root, "schema");
        if (schema is not ("jar2dll.gui-load-config/v1" or "jar2dll.build-config/v1"))
            throw new InvalidDataException("Unsupported configuration schema: " + (string.IsNullOrEmpty(schema) ? "(missing)" : schema));
        string baseDirectory = Path.GetDirectoryName(Path.GetFullPath(configPath))!;
        InputJarTextBox.Text = ResolveConfiguredPath(baseDirectory, ReadString(root, "inputJar"));
        if (schema == "jar2dll.gui-load-config/v1") {
            OutputDllTextBox.Text = ResolveConfiguredPath(baseDirectory, ReadString(root, "outputDll"));
            ArgumentFileTextBox.Text = ResolveConfiguredPath(baseDirectory, ReadString(root, "entryPoint", "argumentFile"));
            TempDirectoryTextBox.Text = ResolveConfiguredPath(baseDirectory, ReadString(root, "workspace"));
            NativeDebugCheckBox.IsChecked = ReadBool(root, "nativeDebug", true);
        }
        JsonElement entry = ReadObject(root, "entryPoint");
        MainClassTextBox.Text = ReadString(entry, "className");
        MainMethodTextBox.Text = ReadString(entry, "methodName");
        string descriptor = ReadString(entry, "descriptor");
        SetComboValue(MethodSignatureComboBox, descriptor);
        ArgumentTextBox.Text = ReadString(entry, "argument");
        ClassLoadersTextBox.Text = string.Join(';', ReadArray(root, "classLoaders"));
        ApplyLibraries(baseDirectory, ReadArray(root, "libraries"));
        NativeLibrariesTextBox.Text = string.Join(';', ReadArray(root, "nativeLibraries").Select(path => ResolveConfiguredPath(baseDirectory, path)));
        SetComboValue(PlatformComboBox, ReadString(root, "platform"));
        SetComboValue(ConfigurationComboBox, ReadString(root, "configuration"));
        SetComboTag(NativeModeComboBox, ReadString(root, "nativeMode"));
        MemoryJarNativesCheckBox.IsChecked = ReadBool(root, "memoryJarNatives", true);
        CanDumpCheckBox.IsChecked = ReadBool(root, "canDump", true);
        JvmArgumentsTextBox.Text = string.Join(Environment.NewLine, ReadArray(root, "jvmArguments"));
        MemoryOptionsPanel.Visibility = SelectedTag(NativeModeComboBox) == "memory" ? Visibility.Visible : Visibility.Collapsed;
        if (schema == "jar2dll.build-config/v1")
            Log("Imported a DLL dump configuration. File names were resolved relative to the JSON file; update any paths that do not exist.");
    }

    private void ApplyLibraries(string baseDirectory, IEnumerable<string> values)
    {
        LibraryListBox.Items.Clear();
        var unresolved = new List<string>();
        foreach (string value in values) {
            string path = ResolveConfiguredPath(baseDirectory, value);
            if (File.Exists(path)) AddLibraries([path]); else unresolved.Add(path);
        }
        LibraryJarsTextBox.Text = string.Join(';', unresolved);
    }

    private static JsonElement ReadObject(JsonElement root, string name) => root.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.Object ? value : default;
    private static string ReadString(JsonElement root, string name) => root.ValueKind == JsonValueKind.Object && root.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";
    private static string ReadString(JsonElement root, string objectName, string name) => ReadString(ReadObject(root, objectName), name);
    private static bool ReadBool(JsonElement root, string name, bool fallback) => root.ValueKind == JsonValueKind.Object && root.TryGetProperty(name, out JsonElement value) && (value.ValueKind is JsonValueKind.True or JsonValueKind.False) ? value.GetBoolean() : fallback;
    private static string[] ReadArray(JsonElement root, string name) => root.ValueKind == JsonValueKind.Object && root.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.Array ? value.EnumerateArray().Where(item => item.ValueKind == JsonValueKind.String).Select(item => item.GetString()!).ToArray() : [];
    private static string[] SplitValues(string text) => text.Split([';', '\r', '\n'], StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
    private static string[] SplitLines(string text) => text.Split(["\r\n", "\n"], StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
    private static string AbsoluteOrOriginal(string path) => string.IsNullOrWhiteSpace(path) ? "" : Path.GetFullPath(path);
    private static string ResolveConfiguredPath(string baseDirectory, string value) => string.IsNullOrWhiteSpace(value) ? "" : Path.IsPathFullyQualified(value) ? value : Path.GetFullPath(Path.Combine(baseDirectory, value));
    private List<string> CollectLibraryPaths() => LibraryListBox.Items.OfType<string>().Concat(SplitValues(LibraryJarsTextBox.Text)).Distinct(StringComparer.OrdinalIgnoreCase).ToList();

    private static void SetComboValue(ComboBox comboBox, string value)
    {
        if (string.IsNullOrWhiteSpace(value)) return;
        ComboBoxItem? item = comboBox.Items.OfType<ComboBoxItem>().FirstOrDefault(candidate => candidate.Content?.ToString()?.StartsWith(value, StringComparison.Ordinal) == true);
        if (item is not null) comboBox.SelectedItem = item;
    }

    private static void SetComboTag(ComboBox comboBox, string value)
    {
        if (string.IsNullOrWhiteSpace(value)) return;
        ComboBoxItem? item = comboBox.Items.OfType<ComboBoxItem>().FirstOrDefault(candidate => string.Equals(candidate.Tag?.ToString(), value, StringComparison.OrdinalIgnoreCase));
        if (item is not null) comboBox.SelectedItem = item;
    }

    private void SelectFile(TextBox target, string filter)
    {
        var dialog = new OpenFileDialog { Filter = filter, CheckFileExists = true };
        if (dialog.ShowDialog(this) == true) target.Text = dialog.FileName;
    }

    private void SaveFile(TextBox target, string filter, string name)
    {
        var dialog = new SaveFileDialog { Filter = filter, FileName = name, AddExtension = true };
        if (dialog.ShowDialog(this) == true) target.Text = dialog.FileName;
    }

    private void Window_DragOver(object sender, DragEventArgs e)
    {
        e.Effects = e.Data.GetDataPresent(DataFormats.FileDrop) ? DragDropEffects.Copy : DragDropEffects.None;
        e.Handled = true;
    }

    private async void Window_Drop(object sender, DragEventArgs e)
    {
        if (e.Data.GetData(DataFormats.FileDrop) is not string[] files || files.Length == 0) return;
        string file = files[0];
        if (file.EndsWith(".jar", StringComparison.OrdinalIgnoreCase)) {
            InputJarTextBox.Text = file;
            OutputDllTextBox.Text = Path.ChangeExtension(file, ".dll");
            await DiscoverEntryPointAsync(file);
        }
        else if (file.EndsWith(".dll", StringComparison.OrdinalIgnoreCase)) {
            InspectDllTextBox.Text = file;
            RunDllTextBox.Text = file;
            RecoveredJarTextBox.Text = Path.Combine(Path.GetDirectoryName(file)!, Path.GetFileNameWithoutExtension(file) + "-dump.jar");
            Log("DLL selected for inspect or dump: " + file);
        }
        else {
            Log("Drop a .jar to scan an entry point, or a .dll to inspect/dump.");
        }
    }

    private async Task DiscoverEntryPointAsync(string jarPath)
    {
        Log("Scanning JAR for @DllMain or public static main(String[]) …");
        try {
            List<MethodCandidate> candidates = await Task.Run(() => ScanJar(jarPath));
            entryCandidates = candidates.Where(candidate => candidate.IsDllMain || candidate.IsJavaMain).ToList();
            EntryPointComboBox.ItemsSource = entryCandidates;
            if (entryCandidates.Count == 0) {
                Log("No @DllMain or public static main(String[]) entry point was found.");
                return;
            }
            if (entryCandidates.Count == 1) {
                EntryPointComboBox.SelectedIndex = 0;
                Log("✓ Detected one entry point and selected it automatically.");
            }
            else {
                EntryPointComboBox.SelectedIndex = -1;
                Log($"Found {entryCandidates.Count} entry points. Choose one from Entry candidates.");
            }
        }
        catch (Exception exception) {
            Log("Could not scan JAR: " + exception.Message);
        }
    }

    private static List<MethodCandidate> ScanJar(string jarPath)
    {
        var candidates = new List<MethodCandidate>();
        using ZipArchive archive = ZipFile.OpenRead(jarPath);
        foreach (ZipArchiveEntry entry in archive.Entries.Where(entry => entry.FullName.EndsWith(".class", StringComparison.Ordinal))) {
            using Stream stream = entry.Open();
            using var data = new MemoryStream();
            stream.CopyTo(data);
            candidates.AddRange(ClassScanner.Scan(data.ToArray()));
        }
        return candidates;
    }

    private async void Build_Click(object sender, RoutedEventArgs e)
    {
        if (!RequireBuilder(SelectedTag(NativeModeComboBox) == "memory") || !RequireFile(InputJarTextBox.Text, "Input JAR")
            || !RequireText(MainClassTextBox.Text, "Main class") || !RequireText(OutputDllTextBox.Text, "Output DLL")) return;

        bool hasArgument = !string.IsNullOrWhiteSpace(ArgumentFileTextBox.Text) || !string.IsNullOrEmpty(ArgumentTextBox.Text);
        if (hasArgument && Selected(MethodSignatureComboBox).StartsWith("()V", StringComparison.Ordinal)) {
            MethodSignatureComboBox.SelectedIndex = 1;
            Log("Argument input selected: switched entry signature to String[].");
        }

        var arguments = new List<string>
        {
            "-i", InputJarTextBox.Text, "-run", MainClassTextBox.Text.Trim(),
            "-method", MainMethodTextBox.Text.Trim(), "-o", OutputDllTextBox.Text,
            "-signature", Selected(MethodSignatureComboBox).Split(' ', 2)[0],
            "-platform", Selected(PlatformComboBox), "-configuration", Selected(ConfigurationComboBox),
            "-native-mode", SelectedTag(NativeModeComboBox),
            "-candump", (CanDumpCheckBox.IsChecked == true).ToString().ToLowerInvariant()
        };
        if (NativeDebugCheckBox.IsChecked == true) arguments.Add("-native-debug");
        arguments.Add("-memory-jar-natives");
        arguments.Add((MemoryJarNativesCheckBox.IsChecked != false).ToString().ToLowerInvariant());
        AddJvmArguments(arguments, JvmArgumentsTextBox.Text);
        if (!string.IsNullOrWhiteSpace(ArgumentFileTextBox.Text)) {
            arguments.Add("-argument-file");
            arguments.Add(ArgumentFileTextBox.Text);
        }
        else if (!string.IsNullOrEmpty(ArgumentTextBox.Text)) {
            arguments.Add("-argument");
            arguments.Add(ArgumentTextBox.Text);
        }
        List<string> libraryPaths = LibraryListBox.Items.OfType<string>()
            .Concat(LibraryJarsTextBox.Text.Split([';', '\r', '\n'], StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
            .Distinct(StringComparer.OrdinalIgnoreCase).ToList();
        string? libraryListFile = null;
        string? nativeLibraryListFile = null;
        try {
            string workspace = GetWorkspaceDirectory();
            arguments.Add("-temp");
            arguments.Add(workspace);
            if (libraryPaths.Count > 12) {
                libraryListFile = Path.Combine(workspace, "jar2dll-libraries-" + Guid.NewGuid().ToString("N") + ".txt");
                await File.WriteAllLinesAsync(libraryListFile, libraryPaths, new UTF8Encoding(false));
                arguments.Add("-lib-list");
                arguments.Add(libraryListFile);
                Log($"Passing {libraryPaths.Count} library JARs through a temporary list file.");
            }
            else {
                foreach (string library in libraryPaths) { arguments.Add("-lib"); arguments.Add(library); }
            }
            List<string> nativeLibraries = NativeLibrariesTextBox.Text.Split([';', '\r', '\n'], StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).ToList();
            if (nativeLibraries.Count > 12) {
                nativeLibraryListFile = Path.Combine(workspace, "jar2dll-native-libraries-" + Guid.NewGuid().ToString("N") + ".txt");
                await File.WriteAllLinesAsync(nativeLibraryListFile, nativeLibraries, new UTF8Encoding(false));
                arguments.Add("-native-lib-list");
                arguments.Add(nativeLibraryListFile);
            }
            else {
                foreach (string nativeLibrary in nativeLibraries) { arguments.Add("-native-lib"); arguments.Add(nativeLibrary); }
            }
            await RunAsync(arguments, ClassLoadersTextBox.Text);
        }
        finally {
            if (libraryListFile is not null) try { File.Delete(libraryListFile); } catch { }
            if (nativeLibraryListFile is not null) try { File.Delete(nativeLibraryListFile); } catch { }
        }
    }

    private void EntryPointSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (EntryPointComboBox.SelectedItem is not MethodCandidate selected) return;
        MainClassTextBox.Text = selected.ClassName;
        MainMethodTextBox.Text = selected.MethodName;
        MethodSignatureComboBox.SelectedIndex = selected.Signature == "()V" ? 0 : 1;
        Log($"Selected: {selected.Display}");
    }

    private void NativeModeSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (MemoryOptionsPanel is null || NativeModeComboBox.SelectedItem is null) return;
        bool memory = SelectedTag(NativeModeComboBox) == "memory";
        MemoryOptionsPanel.Visibility = memory ? Visibility.Visible : Visibility.Collapsed;
        if (memory) Log("Memory mode: direct JAR DLL load calls are ASM-rewritten; matching DLL resources are embedded and mapped in memory (x64 only).");
    }

    private async void Inspect_Click(object sender, RoutedEventArgs e)
    {
        if (!RequireBuilder() || !RequireFile(InspectDllTextBox.Text, "Source DLL")) return;
        await RunAsync(["-inspect", InspectDllTextBox.Text, "-detail"]);
    }

    private async void Recover_Click(object sender, RoutedEventArgs e)
    {
        if (!RequireBuilder() || !RequireFile(InspectDllTextBox.Text, "Source DLL")
            || !RequireText(RecoveredJarTextBox.Text, "Recovered JAR")) return;
        await RunAsync(["-dump", InspectDllTextBox.Text, "-o", RecoveredJarTextBox.Text]);
    }

    private async void DumpConfig_Click(object sender, RoutedEventArgs e)
    {
        if (!RequireBuilder() || !RequireFile(InspectDllTextBox.Text, "Source DLL")) return;
        await RunAsync(["-dump-config", InspectDllTextBox.Text]);
    }

    private async void Run_Click(object sender, RoutedEventArgs e)
    {
        if (!RequireBuilder() || !RequireFile(RunDllTextBox.Text, "Source DLL")) return;
        var arguments = new List<string> { "-run", RunDllTextBox.Text };
        AddJvmArguments(arguments, RunJvmArgumentsTextBox.Text);
        foreach (string value in RunArgumentsTextBox.Text.Split(["\r\n", "\n"], StringSplitOptions.RemoveEmptyEntries)) {
            arguments.Add("-args");
            arguments.Add(value);
        }
        if (!string.IsNullOrWhiteSpace(RunArgumentFileTextBox.Text)) {
            if (!RequireFile(RunArgumentFileTextBox.Text, "Argument file")) return;
            arguments.Add("-args-file");
            arguments.Add(RunArgumentFileTextBox.Text);
        }
        await RunAsync(arguments);
    }

    private async Task RunAsync(List<string> arguments, string? classLoaders = null)
    {
        if (!string.IsNullOrWhiteSpace(classLoaders))
        {
            arguments.Add("-cld");
            arguments.Add(classLoaders);
        }
        if (!arguments.Contains("-temp", StringComparer.OrdinalIgnoreCase)) {
            arguments.Add("-temp");
            arguments.Add(GetWorkspaceDirectory());
        }

        SetBusy(true);
        Log($"> {Path.GetFileName(BuilderPathTextBox.Text)} {string.Join(' ', arguments.Select(Quote))}");
        try
        {
            var info = new ProcessStartInfo(BuilderPathTextBox.Text) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true };
            foreach (string argument in arguments) info.ArgumentList.Add(argument);
            using Process process = Process.Start(info) ?? throw new InvalidOperationException("Could not start j2d.exe");
            bool showLiveOutput = LiveOutputCheckBox.IsChecked == true;
            var standardOutput = new StringBuilder();
            var standardError = new StringBuilder();
            object outputLock = new();

            void ReceiveOutputLine(string? line, bool isError)
            {
                if (line is null) return;
                lock (outputLock)
                {
                    (isError ? standardError : standardOutput).AppendLine(line);
                }
                if (showLiveOutput)
                    _ = Dispatcher.InvokeAsync(() => Log(isError ? "[err] " + line : "[out] " + line));
            }

            process.OutputDataReceived += (_, eventArgs) => ReceiveOutputLine(eventArgs.Data, false);
            process.ErrorDataReceived += (_, eventArgs) => ReceiveOutputLine(eventArgs.Data, true);
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            await process.WaitForExitAsync();
            if (!showLiveOutput)
            {
                string output;
                string error;
                lock (outputLock)
                {
                    output = standardOutput.ToString().Trim();
                    error = standardError.ToString().Trim();
                }
                if (!string.IsNullOrEmpty(output)) Log(output);
                if (!string.IsNullOrEmpty(error)) Log(error);
            }
            Log(process.ExitCode == 0 ? "✓ Completed successfully." : $"✗ j2d.exe exited with code {process.ExitCode}.");
        }
        catch (Exception exception)
        {
            Log("✗ " + exception.Message);
        }
        finally
        {
            SetBusy(false);
        }
    }

    private static string Selected(ComboBox comboBox) => ((ComboBoxItem)comboBox.SelectedItem).Content.ToString()!;
    private static string SelectedTag(ComboBox comboBox) => ((ComboBoxItem)comboBox.SelectedItem).Tag?.ToString() ?? Selected(comboBox).Split(' ', 2)[0];
    private static string Quote(string value) => value.Contains(' ') ? '"' + value + '"' : value;

    private string GetWorkspaceDirectory()
    {
        string path = string.IsNullOrWhiteSpace(TempDirectoryTextBox.Text)
            ? Path.Combine(Environment.CurrentDirectory, "Temp") : Path.GetFullPath(TempDirectoryTextBox.Text);
        Directory.CreateDirectory(path);
        return path;
    }

    private static void AddJvmArguments(List<string> target, string text)
    {
        foreach (string value in text.Split(["\r\n", "\n"], StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)) {
            target.Add("-jvmarg");
            target.Add(value);
        }
    }

    private bool RequireFile(string path, string label)
    {
        if (File.Exists(path)) return true;
        MessageBox.Show(this, $"{label} was not found:\n{path}", "Jar2Dll Studio", MessageBoxButton.OK, MessageBoxImage.Warning);
        return false;
    }

    private bool RequireBuilder(bool requireMemoryRewriter = false)
    {
        string path = BuilderPathTextBox.Text;
        if (!IsJar2DllBuilder(path)) {
            MessageBox.Show(this,
                $"The selected executable is not a Jar2Dll builder (its embedded identity marker is missing):\n{path}",
                "Jar2Dll Studio", MessageBoxButton.OK, MessageBoxImage.Warning);
            return false;
        }
        if (requireMemoryRewriter) {
            string rewriter = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(path))!, "memory-rewriter.jar");
            if (!File.Exists(rewriter)) {
                MessageBox.Show(this,
                    $"Memory mode requires memory-rewriter.jar beside the builder:\n{rewriter}\n\nRe-run package-builder.bat or select a complete builder package.",
                    "Jar2Dll Studio", MessageBoxButton.OK, MessageBoxImage.Warning);
                return false;
            }
        }
        return true;
    }

    private static bool IsJar2DllBuilder(string path)
    {
        ReadOnlySpan<byte> marker = "J2D-BUILDER-IDENTITY-V1"u8;
        if (!File.Exists(path)) return false;
        try {
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
            byte[] buffer = new byte[1024 * 1024 + marker.Length - 1];
            int carried = 0;
            while (true) {
                int read = stream.Read(buffer, carried, 1024 * 1024);
                if (read == 0) return false;
                int length = carried + read;
                if (buffer.AsSpan(0, length).IndexOf(marker) >= 0) return true;
                carried = Math.Min(marker.Length - 1, length);
                buffer.AsSpan(length - carried, carried).CopyTo(buffer);
            }
        }
        catch (IOException) { return false; }
        catch (UnauthorizedAccessException) { return false; }
    }

    private bool RequireText(string value, string label)
    {
        if (!string.IsNullOrWhiteSpace(value)) return true;
        MessageBox.Show(this, $"{label} is required.", "Jar2Dll Studio", MessageBoxButton.OK, MessageBoxImage.Warning);
        return false;
    }

    private void SetBusy(bool busy)
    {
        BuildButton.IsEnabled = !busy;
        InspectButton.IsEnabled = !busy;
        RecoverButton.IsEnabled = !busy;
        RunButton.IsEnabled = !busy;
        LiveOutputCheckBox.IsEnabled = !busy;
        Mouse.OverrideCursor = busy ? Cursors.Wait : null;
    }

    private void Log(string message)
    {
        LogTextBox.AppendText($"[{DateTime.Now:HH:mm:ss}] {message}{Environment.NewLine}");
        LogTextBox.ScrollToEnd();
    }

    private void ClearLog_Click(object sender, RoutedEventArgs e) => LogTextBox.Clear();

    private sealed record MethodCandidate(string ClassName, string MethodName, string Signature, bool IsDllMain)
    {
        public bool IsJavaMain => MethodName == "main" && Signature == "([Ljava/lang/String;)V";
        public string Display => ClassName.Replace('.', '/') + ".class "
            + (IsDllMain ? "@DllMain " + MethodName + "()" : "main(String[] args)");
    }

    private static class ClassScanner
    {
        private const string DllMain = "Lxh/xianhhh/jar2dll/DllMain;";

        public static IEnumerable<MethodCandidate> Scan(byte[] bytes)
        {
            var reader = new BigEndianReader(bytes);
            if (reader.U4() != 0xCAFEBABE) return [];
            reader.U2(); reader.U2();
            int count = reader.U2();
            var strings = new string?[count];
            var classes = new int[count];
            for (int index = 1; index < count; index++) {
                switch (reader.U1()) {
                    case 1: strings[index] = Encoding.UTF8.GetString(reader.Bytes(reader.U2())); break;
                    case 3: case 4: reader.Skip(4); break;
                    case 5: case 6: reader.Skip(8); index++; break;
                    case 7: classes[index] = reader.U2(); break;
                    case 8: case 16: case 19: case 20: reader.Skip(2); break;
                    case 9: case 10: case 11: case 12: case 17: case 18: reader.Skip(4); break;
                    case 15: reader.Skip(3); break;
                    default: throw new InvalidDataException("Unknown class constant-pool entry");
                }
            }
            reader.U2();
            string className = strings[classes[reader.U2()]]?.Replace('/', '.') ?? throw new InvalidDataException("Invalid class name");
            reader.U2();
            reader.Skip(reader.U2() * 2);
            SkipMembers(reader);
            var result = new List<MethodCandidate>();
            int methods = reader.U2();
            for (int index = 0; index < methods; index++) {
                int access = reader.U2();
                string name = strings[reader.U2()] ?? "";
                string signature = strings[reader.U2()] ?? "";
                bool annotated = ReadAttributes(reader, strings);
                bool javaMain = name == "main" && signature == "([Ljava/lang/String;)V" && (access & 0x0009) == 0x0009;
                bool supportedAnnotated = annotated && (signature is "()V" or "([Ljava/lang/String;)V");
                if (supportedAnnotated || javaMain) result.Add(new MethodCandidate(className, name, signature, annotated));
            }
            return result;
        }

        private static void SkipMembers(BigEndianReader reader)
        {
            int count = reader.U2();
            for (int index = 0; index < count; index++) { reader.Skip(6); SkipAttributes(reader); }
        }

        private static void SkipAttributes(BigEndianReader reader)
        {
            int count = reader.U2();
            for (int index = 0; index < count; index++) { reader.Skip(2); reader.Skip(checked((int)reader.U4())); }
        }

        private static bool ReadAttributes(BigEndianReader reader, string?[] strings)
        {
            bool found = false;
            int count = reader.U2();
            for (int index = 0; index < count; index++) {
                string name = strings[reader.U2()] ?? "";
                int length = checked((int)reader.U4());
                int end = reader.Position + length;
                if (name is "RuntimeVisibleAnnotations" or "RuntimeInvisibleAnnotations") {
                    int annotations = reader.U2();
                    for (int annotation = 0; annotation < annotations; annotation++) {
                        if ((strings[reader.U2()] ?? "") == DllMain) found = true;
                        int pairs = reader.U2();
                        for (int pair = 0; pair < pairs; pair++) { reader.U2(); SkipElement(reader); }
                    }
                }
                reader.Position = end;
            }
            return found;
        }

        private static void SkipElement(BigEndianReader reader)
        {
            int tag = reader.U1();
            if ("BCDFIJSZsc".Contains((char)tag)) { reader.Skip(2); return; }
            if (tag == 'e') { reader.Skip(4); return; }
            if (tag == '@') { reader.Skip(2); int pairs = reader.U2(); for (int i = 0; i < pairs; i++) { reader.Skip(2); SkipElement(reader); } return; }
            if (tag == '[') { int count = reader.U2(); for (int i = 0; i < count; i++) SkipElement(reader); return; }
            throw new InvalidDataException("Unknown annotation element");
        }

        private sealed class BigEndianReader(byte[] data)
        {
            public int Position { get; set; }
            public int U1() => data[Position++];
            public int U2() => (U1() << 8) | U1();
            public uint U4() => ((uint)U2() << 16) | (uint)U2();
            public byte[] Bytes(int length) { var value = data.AsSpan(Position, length).ToArray(); Position += length; return value; }
            public void Skip(int length) { if (length < 0 || Position + length > data.Length) throw new InvalidDataException("Truncated class file"); Position += length; }
        }
    }
}
