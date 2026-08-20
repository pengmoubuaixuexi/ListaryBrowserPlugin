# 轻量浏览器历史启动器：需求规格

> 2026-08-13 用户授权的 v2 范围更新：本项目演进为轻量 Listary 插件集合，在保留全部浏览器历史行为的基础上增加 `ly` 蓝牙设备模块。下文“只负责浏览器历史”“不得扩展系统功能”等首版限制仅约束 v1，不再排除本文明确批准的蓝牙模块。蓝牙仍必须遵守原生 C++20、无服务、无管理员权限、无后台扫描、无第三方运行时和真实资源测量等强制约束。最初先交付便携 Release x64 EXE；在实机测试通过后，用户于 2026-08-15 授权制作并重装包含蓝牙模块的 v2 安装包。

## 1. 项目目标

开发一个 Windows 原生、便携、常驻内存极低的浏览器历史搜索工具。

用户通过全局快捷键呼出一个小型搜索框，输入浏览器前缀和关键词后，工具只查询该浏览器的本地历史数据库，在下拉列表中展示匹配结果，并始终使用该浏览器打开选中的网址。

首版支持：

- `e <关键词>`：查询 Microsoft Edge 历史，并使用 Edge 打开。
- `g <关键词>`：查询 Google Chrome 历史，并使用 Chrome 打开。

设计必须允许以后低成本增加 Brave、Vivaldi、Firefox 等浏览器；新增 Chromium 浏览器应尽量只增加配置，不修改搜索窗口和核心业务逻辑。

本项目只负责浏览器历史搜索，不取代 Listary，不实现文件、应用或系统设置搜索。

## 2. 强制技术约束

- 平台：Windows 10/11，首版支持 x64。
- 形态：独立便携程序，优先单个 EXE，可附带一个可选配置文件。
- 推荐技术：C++20 + Win32 原生界面。
- SQLite：优先调用 Windows 自带的 `winsqlite3.dll`，避免捆绑大型运行时。
- 禁止使用 Electron、WebView、Chromium Embedded Framework、Python常驻进程、Java、Qt/QML或完整浏览器内核。
- 不安装Windows服务，不需要管理员权限。
- 不建立全量后台索引，不在后台持续扫描或轮询浏览器数据库。
- 不包含遥测、账户、云同步、AI、插件市场、自动更新器。
- 默认完全离线；不得上传浏览历史、网址或查询词。
- 只读浏览器数据，绝不修改或锁定浏览器原始数据库。

如果开发过程中发现推荐技术无法满足指标，必须先报告实测数据和替代方案，不得未经确认改用明显更重的框架。

## 3. 资源指标

以下指标属于验收条件，而不是参考建议：

- 隐藏空闲时：
  - CPU应接近 `0%`。
  - 私有工作集目标不超过 15 MB，最大不得超过 25 MB。
- 搜索窗口显示并完成查询时：
  - 私有工作集目标不超过 30 MB，最大不得超过 50 MB。
- Release程序及必要依赖总大小目标不超过 10 MB。
- 温启动后，从按下快捷键到输入框可输入：目标不超过 100 ms。
- 对单个包含10万条历史记录的数据库，常见查询返回首批结果：目标不超过 100 ms。
- 隐藏窗口后必须释放结果对象、临时字符串和不再需要的数据库连接。
- 不得通过频繁强制清理工作集伪造低内存；以内存结构简单、按需分配和及时释放为主。

必须在README或测试报告中记录实际测量值、Windows版本、数据库规模和测量口径。

## 4. 核心交互

### 4.1 呼出与关闭

- 提供可配置的全局快捷键，默认值应避免与 Listary 常用快捷键冲突。
- 按快捷键后，在当前显示器或鼠标所在显示器中央显示搜索框。
- 输入框自动获得焦点。
- `Esc` 隐藏窗口并清空本次查询状态。
- 再次按快捷键时复用原进程和窗口，不重复启动实例。
- 支持托盘菜单：显示、设置、退出。托盘功能应保持简单。

### 4.2 查询语法

标准语法：

```text
<浏览器前缀><空格><查询词>
```

示例：

```text
e baidu
g github
```

行为要求：

- 输入 `e` 或 `e `：显示 Edge 最近访问记录。
- 输入 `g` 或 `g `：显示 Chrome 最近访问记录。
- 输入未知前缀：显示简短提示，不执行文件搜索或其他功能。
- 浏览器前缀必须唯一，不区分大小写。
- 前缀、浏览器名称和启用状态必须可配置。
- 输入完整的 `http://` 或 `https://` URL时，提供“用当前前缀浏览器直接打开”结果。

### 4.3 下拉结果

每条历史结果至少显示：

- 页面标题。
- URL。
- 来源浏览器图标或简洁来源标识。
- 可选的最近访问时间；默认不要显示过多信息。

交互要求：

- `↑`、`↓` 切换选择。
- `Enter` 打开选中结果。
- 鼠标单击选择，双击打开。
- 默认最多显示20条结果，数量可配置但应设合理上限。
- 查询输入使用约60至120 ms防抖，避免每个按键都复制数据库或启动新线程。
- 新查询必须取消或忽略上一次尚未完成的结果，避免旧结果覆盖新结果。
- 所有数据库查询在工作线程执行，界面线程不得阻塞。

### 4.4 打开行为

- Edge来源结果必须明确调用 `msedge.exe`，即使Windows默认浏览器是Chrome。
- Chrome来源结果必须明确调用 `chrome.exe`，即使Windows默认浏览器是Edge。
- 不通过 `cmd.exe`、PowerShell或字符串拼接执行。
- 使用安全的 `CreateProcessW` 参数构造，正确处理URL中的空格、引号和特殊字符。
- 如果结果来自特定浏览器配置文件，应使用相应的 `--profile-directory` 参数打开。
- 浏览器不可用时显示错误提示，不回退到错误的浏览器。

## 5. 搜索数据与排序

### 5.1 首版数据范围

首版只实现浏览历史：

- Chromium系浏览器的 `History` SQLite数据库。
- 至少读取 `url`、`title`、`last_visit_time`；如果存在，可使用 `visit_count` 和 `typed_count` 辅助排序。
- 不读取密码、Cookies、表单数据、下载记录或其他敏感数据库。

书签、当前标签页、下载记录和联网联想不属于首版范围。

### 5.2 匹配与排序

- 查询词应同时匹配标题和URL。
- 默认不区分大小写。
- 支持中文、英文和Unicode URL。
- 首版不需要模糊搜索引擎；可采用大小写无关的子串匹配。
- 排序综合考虑：
  1. 标题或域名的精确/前缀匹配。
  2. 最近访问时间。
  3. `typed_count`。
  4. `visit_count`。
- 排序逻辑必须简单、可解释，不引入机器学习模型。
- 不得一次性把整个历史数据库加载到内存；使用SQL筛选和结果上限。

### 5.3 可选网页搜索兜底

首版可以提供一条可关闭的合成结果：

```text
使用当前浏览器搜索“<查询词>”
```

- 搜索URL模板写入浏览器配置，例如 `https://www.google.com/search?q={query}`。
- 必须正确进行URL编码。
- 这只是按Enter后的网页搜索，不请求实时联想接口。
- 默认不得把用户正在输入的内容发送到网络。

## 6. 浏览器与配置文件发现

### 6.1 Chromium首版适配器

Chrome和Edge共用一个 `ChromiumHistoryAdapter`，浏览器差异由配置描述：

- 浏览器ID。
- 展示名称。
- 查询前缀。
- 可执行文件候选路径。
- 用户数据目录候选路径。
- 历史数据库相对路径。
- 浏览器图标。
- 搜索URL模板。
- 启动参数模板。

禁止为Chrome和Edge复制两套几乎相同的查询代码。

### 6.2 多配置文件

- 自动识别 `Default`、`Profile 1`、`Profile 2` 等配置文件。
- 优先读取浏览器 `Local State` 中的配置文件信息；无法读取时再扫描目录。
- 配置中允许选择启用哪些配置文件。
- 同一浏览器多个配置文件的结果可合并，但每条结果必须保留来源配置文件。
- 打开结果时尽量回到产生该记录的配置文件。

### 6.3 添加新浏览器

新增Chromium浏览器时，理想流程为：

1. 在配置中增加浏览器定义。
2. 填写EXE路径和User Data路径。
3. 分配唯一前缀。
4. 无需修改搜索窗口、路由器和SQLite查询代码。

Firefox等非Chromium浏览器通过实现新的 `IBrowserHistoryAdapter` 接入，不得在UI层增加浏览器特判。

建议接口：

```cpp
class IBrowserHistoryAdapter {
public:
    virtual std::vector<BrowserProfile> DiscoverProfiles() = 0;
    virtual std::vector<HistoryResult> Search(
        const BrowserProfile& profile,
        std::wstring_view query,
        size_t limit) = 0;
    virtual bool OpenUrl(
        const BrowserProfile& profile,
        std::wstring_view url) = 0;
    virtual ~IBrowserHistoryAdapter() = default;
};
```

## 7. SQLite读取策略

- 优先尝试只读方式访问数据库。
- 浏览器正在运行导致无法稳定读取时，创建临时快照后查询。
- 快照复制必须采用流式小缓冲区，不得把整个数据库读入内存。
- 同一个搜索会话内复用快照，禁止每次按键都复制数据库。
- 仅在以下情况刷新快照：
  - 搜索窗口重新呼出且原数据库已变化。
  - 原数据库的修改时间或大小发生变化。
  - 用户手动刷新。
- 临时文件名必须避免不同浏览器和配置文件冲突。
- 程序正常退出时删除临时快照。
- 程序启动时清理属于本程序的过期快照，但不得删除其他程序的临时文件。

## 8. 兼容性与容错

- 不假设历史数据库结构永久不变。
- 查询前通过 `sqlite_master` 和 `PRAGMA table_info` 检查所需表和字段。
- 数据库格式不兼容时，显示“浏览器历史格式暂不支持”，不得崩溃。
- 将数据库字段映射和SQL放在适配器内，UI不得直接依赖数据库结构。
- 浏览器未安装、配置文件不存在、数据库为空、数据库损坏或正在迁移时均应安全失败。
- 单个配置文件读取失败不得阻止其他配置文件或浏览器工作。
- 所有错误均使用简短用户提示和可选本地日志；日志不得默认写入完整查询词和完整浏览历史。
- 程序不得因浏览器更新而修改、重建或修复浏览器数据库。

## 9. 推荐模块划分

```text
AppHost
├─ SingleInstanceGuard
├─ GlobalHotkeyManager
├─ SearchWindow
├─ QueryParser
├─ BrowserRegistry
│  ├─ BrowserDefinition
│  └─ ProfileDiscovery
├─ HistorySearchService
│  ├─ SnapshotManager
│  ├─ ChromiumHistoryAdapter
│  └─ IBrowserHistoryAdapter
├─ BrowserLauncher
├─ ConfigStore
└─ Diagnostics
```

要求：

- UI、查询、数据库快照和浏览器启动彼此解耦。
- Chromium浏览器之间共享适配器。
- 新增浏览器不修改现有浏览器逻辑。
- 配置读取失败时使用安全默认值。

## 10. 配置示例

配置格式可以采用JSON，但解析器必须轻量；也可以使用简单INI。示意如下：

```json
{
  "hotkey": "Alt+Space",
  "maxResults": 20,
  "browsers": [
    {
      "id": "edge",
      "name": "Microsoft Edge",
      "prefix": "e",
      "engine": "chromium",
      "enabled": true,
      "executableCandidates": [
        "%ProgramFiles(x86)%\\Microsoft\\Edge\\Application\\msedge.exe",
        "%ProgramFiles%\\Microsoft\\Edge\\Application\\msedge.exe"
      ],
      "userDataCandidates": [
        "%LocalAppData%\\Microsoft\\Edge\\User Data"
      ],
      "searchUrl": "https://www.bing.com/search?q={query}"
    },
    {
      "id": "chrome",
      "name": "Google Chrome",
      "prefix": "g",
      "engine": "chromium",
      "enabled": true,
      "executableCandidates": [
        "%ProgramFiles%\\Google\\Chrome\\Application\\chrome.exe",
        "%LocalAppData%\\Google\\Chrome\\Application\\chrome.exe"
      ],
      "userDataCandidates": [
        "%LocalAppData%\\Google\\Chrome\\User Data"
      ],
      "searchUrl": "https://www.google.com/search?q={query}"
    }
  ]
}
```

示例仅表达配置能力，最终配置字段和解析规则应在README中固定下来。

## 11. 明确不做的功能

首版不得扩展到以下内容：

- 文件、文件内容或应用搜索。
- Windows设置、计算器、剪贴板或命令执行。
- 浏览器密码、Cookies、表单数据。
- 浏览器内容全文抓取。
- AI总结或语义检索。
- 云同步、登录账户和多设备同步。
- 插件市场或通用自动化框架。
- 嵌入网页预览。
- 模拟或复刻Chrome/Edge原生Omnibox排序算法。
- 默认联网搜索建议。

这些功能即使容易实现，也不得在首版加入，以免破坏轻量化目标。

## 12. 验收测试

### 12.1 功能测试

- 输入 `e baidu` 只出现Edge历史，不出现Chrome独有历史。
- 输入 `g baidu` 只出现Chrome历史，不出现Edge独有历史。
- Windows默认浏览器设为Chrome时，Edge结果仍由Edge打开。
- Windows默认浏览器设为Edge时，Chrome结果仍由Chrome打开。
- 输入 `e` 能显示Edge最近访问记录。
- 输入 `g` 能显示Chrome最近访问记录。
- 浏览器运行并持续写历史时仍能搜索，不要求关闭浏览器。
- 支持 `Default` 和至少一个 `Profile N` 配置文件。
- 中文标题、中文查询、包含Unicode字符的URL均正常。
- 浏览器未安装或历史数据库不存在时不崩溃。
- 快速连续输入时不出现旧查询覆盖新查询。

### 12.2 扩展性测试

- 通过新增一段配置接入一个采用标准Chromium目录结构的测试浏览器，不修改UI和查询服务代码。
- 前缀冲突时拒绝加载冲突配置并给出明确提示。
- 模拟缺少某个SQLite字段时，适配器安全降级或报告不兼容。

### 12.3 资源测试

- 验证空闲CPU和内存指标。
- 验证查询期间的峰值内存。
- 验证反复呼出和隐藏100次后内存没有持续增长。
- 验证10万条历史记录数据库上的响应时间。
- 验证退出后没有残留后台进程。
- 验证临时快照能够被正确清理。

## 13. 交付物

- 完整源代码。
- Release x64便携版本。
- 默认配置和配置示例。
- README：安装、快捷键、查询语法、添加浏览器、隐私说明。
- 自动化测试或可重复执行的测试脚本。
- 资源占用实测报告。
- 第三方代码和许可证清单。

## 14. 实施顺序

1. 建立最小Win32窗口、单实例和全局快捷键，先测空闲内存基线。
2. 实现配置、前缀路由和浏览器可执行文件发现。
3. 实现Chromium数据库快照与只读查询。
4. 接入Chrome和Edge，确保严格使用来源浏览器打开。
5. 增加多配置文件识别。
6. 完成异步查询、防抖、取消和容错。
7. 执行功能、性能、内存和长期反复呼出测试。
8. 在通过首版验收前，不增加书签、标签页或联网联想。

## 15. 给开发代理的执行要求

- 先阅读本需求并冻结MVP边界，再开始编码。
- 优先完成能运行、能测量的最小垂直链路：`g 查询 → Chrome历史下拉 → Chrome打开`。
- 每引入一个依赖，说明它的二进制大小、运行时内存成本、许可证和必要性。
- 每个阶段都编译Release版本并记录真实内存、CPU和响应时间。
- 如果性能或内存指标不达标，先定位实际占用来源，不得通过隐藏进程、延迟启动或强制修剪工作集掩盖问题。
- 保持改动可审查；不要在首版堆叠与目标无关的功能。
