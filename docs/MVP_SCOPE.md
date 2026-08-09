# MVP 边界冻结

冻结日期：2026-08-08

2026-08-09 补充：按用户确认增加主程序内的窄范围 Listary/浏览器接入配置页；并增加“历史零匹配时生成一条网页搜索结果”的配置驱动兜底。它不是实时联网建议，也不是通用设置中心。

本文把 `REQUIREMENTS.md` 转换为首版实现边界。若两者冲突，以需求文档为准。

## 本次必须交付

- Windows 10/11 x64，原生 C++20 + Win32，Release 便携 EXE。
- 单实例、可配置全局快捷键（当前为 `Ctrl+Shift+Space`）、搜索窗口、托盘的显示/配置 Listary/退出菜单。
- 严格前缀路由：`g` 查询 Chrome，随后由同一个 Chromium 适配器支持 `e` 查询 Edge。
- `prefix`、浏览器名称、启用状态、EXE 候选路径、User Data 候选路径和历史相对路径由 INI 配置驱动。
- 自动发现 `Default` 与 `Profile N`；优先读取 `Local State` 的 `profile.info_cache`，失败时扫描目录。
- 只读查询 Chromium `History` 的 `urls` 表，只使用 `url`、`title`、`last_visit_time` 以及可选的 `typed_count`、`visit_count`。
- UI 线程外执行查询；90 ms 默认防抖；以查询代次忽略旧结果。
- 优先直接只读打开数据库；失败时用 64 KiB 缓冲流式复制会话快照；隐藏窗口和退出时释放并清理。
- 下拉最多显示 20 条结果，支持键盘选择、Enter 打开、鼠标选择与双击打开。
- 结果保留浏览器与配置文件来源，并通过 `CreateProcessW` 明确调用来源浏览器；不经过 shell 或默认浏览器。
- 每阶段编译 Release，并记录真实功能、时延、CPU、私有工作集、产物大小和测量口径。
- 主窗口提供 Listary/浏览器接入配置页，自动检测配置目录中已安装的 Chromium 浏览器，并维护关键字、图标、INI、`bhl://` 和兼容的 Listary 网页搜索项。
- 非空查询没有历史匹配时，按浏览器 `SearchUrlTemplate` 生成一条本地网页搜索结果；只有用户确认打开后才由来源浏览器访问网络。

## 本次明确不做

- 文件、应用、系统设置、计算器、剪贴板或命令搜索。
- 书签、标签页、下载、密码、Cookie、表单及网页内容索引。
- 实时联网建议、遥测、账户、云同步、AI、插件、自动更新。
- 嵌入网页、复刻 Omnibox 排序、Firefox 适配器及其他非 Chromium 浏览器。
- 通用图形化设置中心；当前页面仅处理浏览器与 Listary 接入，不管理搜索类别之外的系统功能。
- 后台索引、轮询、服务、管理员权限或强制修剪工作集。

## 本机工具链基线

- 操作系统：Windows 11 家庭中文版 10.0.22621，x64。
- MSVC：19.38.33133，x64 Host/Target。
- Visual Studio Build Tools：2022 17.8.3。
- Windows SDK：10.0.22621.0（另安装 10.0.26100.0）。
- SQLite：系统 `winsqlite3.dll` 3.34.1；SDK 自带头文件和 x64 导入库。
- CMake：PATH 与 VS 安装目录均未发现；本项目使用 MSBuild 工程，不额外安装构建依赖。
- Git：已初始化本地仓库。

## 依赖边界

仅链接 Windows SDK/系统组件：Win32、Common Controls、Shell、PSAPI 与 `winsqlite3.dll`。不捆绑第三方运行时；系统 DLL 不计入便携包体。
