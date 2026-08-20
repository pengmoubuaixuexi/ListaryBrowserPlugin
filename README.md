# Listary Plugin Suite

[![Latest release](https://img.shields.io/github/v/release/pengmoubuaixuexi/ListaryBrowserPlugin)](https://github.com/pengmoubuaixuexi/ListaryBrowserPlugin/releases/latest)
[![Windows 10/11](https://img.shields.io/badge/Windows-10%20%7C%2011-0078D4)](https://github.com/pengmoubuaixuexi/ListaryBrowserPlugin/releases/latest)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C)](https://github.com/pengmoubuaixuexi/ListaryBrowserPlugin)

一个 Windows 10/11 x64 原生 Listary 插件集合。程序只常驻一个轻量宿主：输入 `g` 查询 Chrome 历史，输入 `e` 查询 Edge 历史，输入 `ly` 按需列出已配对蓝牙设备。浏览器结果始终由来源浏览器和 Profile 打开；蓝牙没有独立页面，也不会后台扫描。

当前稳定版本为 v2.0.0，图形安装器和便携 ZIP 均包含浏览器历史与 `ly` 蓝牙模块。v1.0.0 仅包含浏览器历史功能。

当前实现严格限于 Chromium 浏览历史，不搜索文件、应用、书签、标签页、下载、密码、Cookie 或网页内容，也不提供联网建议、遥测、账户、云同步和自动更新。

> 这是社区开发的非官方 Listary 扩展，与 Listary 官方无隶属或背书关系。本机已验证 Listary 6.3.5.94，Listary V7 已由实际用户验证兼容。

## 为什么做这个

Listary 可以快速找到文件和应用，但浏览器内部访问过的页面通常还要切回浏览器搜索。本工具把 Chrome、Edge 及其他兼容 Chromium 浏览器的本地历史记录直接放进 Listary 下拉列表：输入关键字即可筛选，回车后仍由记录所属浏览器和 Profile 打开。

- 原生 C++20 + Win32，一个启动项、一个托盘、一个本地建议服务；蓝牙只启动同一 EXE 的短生命周期 worker。
- 历史记录只在本机按需读取，不上传网址和查询词，不建立后台完整索引。
- 自动发现 Windows 已注册且历史库结构兼容的 Chromium 浏览器。
- 可一次配置多个浏览器、关键字和 ICO 图标，只重启一次 Listary。
- 没有历史匹配时，第一条结果直接交给对应浏览器地址栏处理网址或搜索词。

## 快速开始

### 安装版 v2.0.0

1. 从项目 [Releases 页面](https://github.com/pengmoubuaixuexi/ListaryBrowserPlugin/releases/latest) 下载 `ListaryBrowserPlugin-Setup-x64.exe`。
2. 完成当前用户安装；默认可选择开机启动，不需要管理员权限。
3. 安装结束后，在自动打开的“配置 Listary”页面设置浏览器和蓝牙关键字。
4. 保存后呼出 Listary：输入 `g`/`e` 搜索浏览器历史，输入 `ly` 列出已配对蓝牙设备。

安装器未经 Authenticode 商业证书签名，Windows 可能显示“未知发布者”。请只从本项目 Releases 页面下载，并用同一版本的 `SHA256SUMS.txt` 核对文件。

### 源码构建

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-release.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\run-tests.ps1
.\build\Release\BrowserHistoryLauncher.exe
```

从托盘或浏览器历史窗口打开“配置 Listary”，保存后会同时写入已启用浏览器和 `ly` 蓝牙项。

## 运行

推荐使用图形安装器：

```text
ListaryBrowserPlugin-Setup-x64.exe
```

安装向导只负责选择安装位置、开机自动启动和桌面快捷方式，采用当前用户安装，不需要管理员权限。安装完成后会打开主程序的“配置 Listary”页面；浏览器与关键字不是一次性的安装选项，以后新装浏览器时可随时重新配置。

1. 使用 `build\Release\BrowserHistoryLauncher.exe`，并让 `BrowserHistoryLauncher.ini` 与 EXE 位于同一目录。
2. 启动后程序隐藏在托盘中。
3. 按 `Ctrl+Shift+Space`，输入 `g github`、`e baidu`，或只输入 `g`/`e` 查看最近记录；蓝牙直接在 Listary 中输入 `ly` 或 `ly air`。
4. 使用 `↑`/`↓` 选择，`Enter` 打开；也可单击选择、双击打开。
5. `Esc` 隐藏并清空本次结果和会话快照。

主窗口右上角的“配置 Listary”会重新读取 Windows 已注册浏览器，并验证其本地历史库是否符合当前 Chromium 适配器。通过验证的浏览器会自动加入下拉框，不要求预先写入 INI；可在下拉框中依次选择多个浏览器，分别设置启用状态、Listary 关键字和 ICO 图标。切换浏览器会保留本页修改，“保存并应用全部”会原子替换 INI、统一同步 `Preferences.json`，因此无论本次配置几个浏览器都只关闭并重启一次 Listary。检测到 Listary 正在显示搜索或设置窗口时才提示重试/取消。

也可以把浏览器历史直接显示为 Listary 的网页搜索提示：本工具通过仅监听 loopback 的按需接口返回结果，并用当前用户的 `bhl://` 协议固定交回来源浏览器和 Profile 打开。完整设置值见 `docs\LISTARY_INTEGRATION.md`。普通 Listary 命令调用 `BrowserHistoryLauncher.exe --query "g github"` 仍作为原生窗口回退方式保留。

Listary 集成依赖本工具进程保持运行；可将 EXE 快捷方式放入当前用户“启动”目录。仅使用 Listary 时不会创建原生 Edit/ListView，也不会加载输入法模块。

开机启动、Listary 配置备份与恢复见 `docs\LISTARY_INTEGRATION.md`；Listary、uTools、Snipaste 的键盘工作流分工及后续扩展建议见 `docs\DESKTOP_KEYBOARD_WORKFLOW.md`。

交付给其他人时优先发送安装器。`build\portable` 目录仍作为免安装版本保留，不能只发送单个 EXE。

托盘菜单包含“显示”“配置 Listary”“退出”。再次运行 EXE 会唤醒已有实例，不会启动第二个常驻实例。

## 构建和测试

需要 Visual Studio 2022 Build Tools、MSVC v143 和 Windows 10/11 SDK。CMake 不是构建依赖。仅生成图形安装器时需要 Inno Setup 6/7；它不是程序运行依赖。

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-release.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\run-tests.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\build-installer.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\package-release.ps1
```

Release 使用 C++20、静态 MSVC CRT 和 Windows SDK 自带 `winsqlite3.lib`。程序只依赖 Windows 系统 DLL，不捆绑第三方运行时。资源实测及当前未达标项见 `docs\RESOURCE_REPORT.md`。

## 查询规则

格式为 `<前缀><空格><查询词>`，前缀不区分大小写。

- `g` 或 `g `：Chrome 最近记录。
- `e` 或 `e `：Edge 最近记录。
- `g github`：第一行原始输入固定表示“交给 Chrome 地址栏搜索”；下面继续显示 Chrome 标题和 URL 的历史匹配，选择任一历史行会打开该行自己的网址。
- `q baidu.com`：识别为地址并直接交给夸克打开，不先构造搜索引擎 URL。
- `q 百度`：第一行以 Chromium 的 `? <搜索词>` 命令行约定交给夸克自己的地址栏搜索逻辑；下面仍可选择夸克历史匹配。
- `e https://example.com/a`：生成一条由 Edge 直接打开的结果。
- 未知前缀：只显示提示，不执行其他搜索。

结果按标题精确匹配、标题前缀、URL 前缀、最近访问时间、`typed_count`、`visit_count` 排序，最多返回 `MaxResults` 条。数据库查询在单独工作线程执行；新请求替换待处理请求，已开始的旧请求通过代次检查被忽略。

## 配置

INI 的 `[app]` 支持：

- `Hotkey`：修饰键加 Space、字母或数字，例如当前使用的 `Ctrl+Shift+Space`。
- `Hotkey=none`：禁用本工具自己的全局快捷键，适合只通过 Listary 调用。
- `MaxResults`：1–100，默认 20。
- `DebounceMs`：限制为 60–120，默认 90。

INI 的 `[bluetooth]` 支持：

- `Enabled`：是否把蓝牙模块写入 Listary。
- `Keyword`：默认 `ly`，必须与浏览器关键字唯一。
- `CacheSeconds`：设备枚举短缓存，限制为 1–30 秒，默认 8。

每个 `[browser.<id>]` 描述一个 Chromium 浏览器：

- `Name`：显示名称。
- `Prefix`：唯一查询前缀。
- `Engine`：首版只能为 `chromium`。
- `Enabled`：`true`/`false`。
- `IconPath`：Listary 中显示的 `.ico` 图标路径；配置页会从检测到的浏览器 EXE 资源中导出真正的 ICO 到 `%LocalAppData%\BrowserHistoryLauncher\Icons`。
- `ExecutableCandidates`：用 `|` 分隔的 EXE 候选路径，支持环境变量。
- `UserDataCandidates`：用 `|` 分隔的 User Data 候选路径。
- `HistoryRelativePath`：配置文件目录下的历史库相对路径，默认 `History`。
- `ProfileArgument`：启动参数模板，`{profile}` 替换为来源配置文件目录名。
- `EnabledProfiles`：`*` 表示所有发现的配置文件，也可用 `|` 指定，例如 `Default|Profile 1`。

默认目录仍声明 Chrome、Edge、Brave、Vivaldi 和 Chromium。配置页还会枚举 Windows 的 `StartMenuInternet` 注册项，读取真实启动命令和图标，并在当前用户目录中浅层定位 `Local State`/`User Data`；只有实际找到且通过 `History.urls` 必需字段校验的 Chromium 浏览器才会作为新项目加入。Firefox、Internet Explorer 等已注册但尚无适配器的浏览器不会错误显示为可配置项。本机已在没有夸克 INI 定义的情况下自动发现并验证夸克浏览器。前缀冲突或不支持的引擎会拒绝应用并显示错误。

## 数据读取和隐私

- 只打开每个配置文件的 `History`，不访问其他浏览器数据库。
- 优先直接以 SQLite 只读模式查询；数据库忙时创建会话级临时快照。
- 快照以 64 KiB 缓冲流式复制，并在同一搜索会话复用；源文件大小或修改时间变化时刷新。
- 隐藏窗口、正常退出和下次启动清理本程序自己的快照，不删除其他程序临时文件。
- 不上传网址、历史或查询词，默认不记录完整查询和历史；第一行地址栏动作只把原始内容交给来源浏览器，实际地址修复或默认搜索由浏览器完成。
- 打开结果使用 `CreateProcessW` 和明确的浏览器 EXE，不经过 `cmd.exe`、PowerShell、ShellExecute 或系统默认浏览器。

## 已冻结的首版边界

详细边界见 `docs\MVP_SCOPE.md`。主程序只增加了 Listary/浏览器接入所需的窄配置页和用户明确要求的浏览器地址栏动作；仍不包含实时联网建议、通用设置中心、Firefox 适配器和需求文档明确排除的功能。

## English

Listary Plugin Suite is an unofficial, privacy-focused Listary extension for Windows 10/11 x64. It shows local Chromium browser history and paired Bluetooth devices directly in Listary's dropdown. Bluetooth enumeration and audio-device connection requests run only on demand in short-lived worker processes; there is no background device scan or separate Bluetooth UI.

The runtime is native C++20/Win32, does not upload browsing data, does not build a background full-history index, and uses about 2.24 MiB private working set in the measured Listary-only idle scenario. Listary 6.3.5.94 was verified locally, and a real user has confirmed compatibility with Listary V7.

Download `ListaryBrowserPlugin-Setup-x64.exe` from [GitHub Releases](https://github.com/pengmoubuaixuexi/ListaryBrowserPlugin/releases/latest), install it without administrator privileges, then use **Configure Listary** to select detected browsers and keywords. This is a community project and is not affiliated with or endorsed by Listary.

## License

本项目采用 [MIT License](LICENSE)。The project is licensed under the [MIT License](LICENSE).
