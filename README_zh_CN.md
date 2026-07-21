# jar2dll

[English README](README.md) · [GPLv3 许可证](LICENSE) · [贡献指南](CONTRIBUTING.md) · [文档目录](docs/README.md)

`jar2dll` 将 Java JAR、原生库和启动元数据嵌入 Windows DLL。JVM 加载该 DLL 后，
它会定义嵌入的 Java 类并调用配置的入口方法。

## 目录结构

- `cpp/`：JNI 启动 DLL 与 PE 内存映射器。
- `csharp/`：`j2d.exe` 构建器和 WPF 图形界面。
- `java/`：注解、Gradle 插件、JAR 工具和 ASM 重写器。
- `cpp/jni-tests/`：原生映射器回归测试项目。

## 构建与打包

需要 Windows、Visual Studio C++ Build Tools/MSBuild，以及已设置 `JAVA_HOME` 的
目标架构 JDK。

```bat
dist\builder\j2d.exe -i "app.jar" -run com.example.Main -o "app.dll"
package-builder.bat
```

构建器在 EXE 内包含身份标记，GUI 会检查这个标记，而不是依据文件名判断。

## 原生库模式

`-native-mode stable` 是默认模式，将显式指定的原生库打包为 ZIP。

`-native-mode memory` 为实验性模式，目前只支持 x64：

```bat
j2d.exe -i "app.jar" -run com.example.Main -native-mode memory ^
  -native-lib "native\example64.dll" -o "app.dll"
```

### JNI 加载调用重写

在嵌入 JAR 前，memory 模式会递归扫描输入 JAR 中的所有 `.dll` 资源。每个资源按
不区分大小写的文件名登记，包含带 `.dll` 和不带后缀两种名称。例如
`native/win64/bridge.dll` 会登记为 `bridge.dll` 与 `bridge`。

ASM 重写器只删除直接且字符串字面量匹配这些资源的加载序列：

```java
System.load("native/win64/bridge.dll");
System.loadLibrary("bridge");
Runtime.getRuntime().load("bridge.dll");
Runtime.getRuntime().loadLibrary("bridge");
```

字节码层面，字符串必须是紧挨调用的 `LDC` 常量；标签、栈帧和行号节点不影响匹配。
对于 `Runtime` 形式，匹配 Java 编译器产生的顺序：`Runtime.getRuntime()`、字符串常量、
`Runtime.load` 或 `Runtime.loadLibrary`。匹配后会删除完整的三条指令；对于 `System`
形式则删除字符串常量和静态调用。

这是删除调用而不是替换为另一个 Java API。之后由启动 DLL 从
`memory-modules.n2dm` 直接在内存中映射原生 PE 模块。

匹配范围有意保持严格。下列情况不会被改写：JAR 内没有对应 DLL、动态生成的库名、
来自变量或方法返回值的字符串、反射、第三方库内部调用，或保存到变量中的 `Runtime`
实例。这些调用会维持 JVM 原行为，仍可能需要 `java.library.path` 或磁盘绝对路径。

匹配到的 JAR DLL 资源与显式 `-native-lib` 输入会存入
`META-INF/jar2dll/memory-modules.n2dm`。原始 DLL 资源仍保留在嵌入 JAR 中。

x64 映射器支持 PE32+、普通导入、延迟导入、内存模块间依赖，以及在 `ClassPrepare` 后
延迟注册 JNI。x64 进程不能执行 x86 DLL；x86 库会在 x64 memory 构建中报告并跳过。
Win32 内存映射目前尚未实现。

将 `memory-rewriter.jar` 放在 `j2d.exe` 同目录。`package-builder.bat` 会将其放入
`dist\builder` 和 `dist\gui`。

## 检查、导出与运行

```bat
j2d.exe -inspect "app.dll"
j2d.exe -inspect "app.dll" -detail
j2d.exe -dump "app.dll" -o "recovered.jar"
j2d.exe -dump-config "app.dll" -o "app.dll.json"
j2d.exe -run "app.dll"
```

`-candump true` 启用 dump 与 inspect。memory 模式的 inspect 会报告 N2DM 模块名称和
大小；dump 会排除内部 N2DM 包，因此恢复的 JAR 只含类和普通资源。

`-run` 是通用恢复/冒烟运行器。它会将 stable 与 memory 原生包提取到临时目录，但不会
复现应用自身启动器的外部环境、资源、认证或启动参数；此类应用应使用原始启动命令。

## 缓存与输出差异

提取出的原生项目缓存于 `Temp\native-project-cache`。每次构建的临时载荷位于
`Temp\jar2dll-build-*`，VC++ 中间文件位于 `Temp\obj`。

每个构建会在元数据载荷后加入随机 nonce，使相同输入的 DLL 二进制不同而不改变行为。
这只是弱随机化，不是加密保护。
