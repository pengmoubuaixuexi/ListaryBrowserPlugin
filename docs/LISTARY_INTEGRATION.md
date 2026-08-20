# Listary 原生结果集成

本机 Listary 6.3.5.94 可以把自定义网页搜索提示显示为 Listary 自己的下拉结果。本工具仅在 `127.0.0.1:32119` 提供按需建议接口；它不建立后台索引，也不持续扫描浏览器数据库。

v2 同一建议宿主还提供 `ly` 蓝牙设备结果。蓝牙设备只在 Listary 请求到达时通过短生命周期 worker 枚举，空闲时不扫描、不轮询，也没有独立蓝牙页面。

选择结果时使用当前用户的 `bhl://` 协议把结果交回本工具，再由结果来源浏览器及 Profile 打开。这样不会经过 Windows 默认浏览器。

## 图形安装器（推荐）

运行 `ListaryBrowserPlugin-Setup-x64.exe` 后，安装向导会提供：

- 自定义安装位置；
- 开机自动启动和桌面快捷方式；
- 安装完成后立即启动主程序的“配置 Listary”页面。

主窗口右上角和托盘菜单都提供“配置 Listary”。页面每次打开都会根据 `BrowserHistoryLauncher.ini` 重新检测浏览器；内置定义包括 Chrome、Edge、Brave、Vivaldi 和 Chromium。可在下拉框中依次选择多个浏览器并设置是否启用、Listary 关键字及 `.ico` 图标；切换选择会保留尚未应用的编辑，最后点击“保存并应用全部”。本次所有浏览器会一起写入，所以只关闭、重启一次 Listary。以后新安装浏览器不需要重装本工具。

快速配置通过配置结构探测兼容 Listary 6/7，不按版本号强行写入。它只修改 `WebSearch.Items.Insertions/Updates` 兼容结构，同时更新当前用户 `bhl://` 注册表，修改前创建带时间戳的 `Preferences.json.bhl-backup-*`。主程序会自动关闭后台空闲的 Listary，写入后重启；如果 Listary 正在显示搜索或设置窗口，或进程无法关闭，才显示重试/取消。如果结构未知或关键字被其他自定义项目占用，则停止自动写入。

选择 `g` 时，Listary 内置 Google 网页搜索可能占用相同关键字。快速配置会禁用该内置项，并记录是否由本工具完成；卸载时只在确认为本工具修改的情况下恢复。卸载不会删除配置备份。

## 首次注册

把完整 portable 目录解压到一个不会随意移动的位置，然后执行一次：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\setup-current-user.ps1
```

该脚本会注册当前用户的 `bhl://` 协议、创建开机启动快捷方式并启动程序；不需要管理员权限。随后可从主窗口右上角完成 Listary 配置；下方 Chrome/Edge 两节保留为手动回退参考。

需要取消注册和开机启动时执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\setup-current-user.ps1 -Remove
```

也可以只注册协议。保持 `BrowserHistoryLauncher.exe` 与 INI 在同一目录，执行：

```powershell
.\BrowserHistoryLauncher.exe --register-listary-protocol
```

注册位置为 `HKEY_CURRENT_USER\Software\Classes\bhl`，不需要管理员权限。移动 EXE 后需要从新位置重新执行注册。撤销时执行：

```powershell
.\BrowserHistoryLauncher.exe --unregister-listary-protocol
```

## Chrome 历史

在 Listary“选项 → 网页搜索”中新增：

- 关键字：`g`
- 标题：`Chrome 历史`
- Url：`bhl://open?prefix=g&selection={query}`
- 搜索提示：`Custom`
- 自定义提示 URL：`http://127.0.0.1:32119/suggest?prefix=g&q={query}`
- 启用：勾选

输入 `g github` 后，第一行 `github` 按 Enter 会交给 Chrome 当前 Profile 的地址栏搜索；选择下面的历史结果则打开该结果自己的 URL。

## Edge 历史

在 Listary“选项 → 网页搜索”中新增：

- 关键字：`e`
- 标题：`Edge 历史`
- Url：`bhl://open?prefix=e&selection={query}`
- 搜索提示：`Custom`
- 自定义提示 URL：`http://127.0.0.1:32119/suggest?prefix=e&q={query}`
- 启用：勾选

输入 `e microsoft` 后，第一行 `microsoft` 按 Enter 会交给 Edge 当前 Profile 的地址栏搜索；选择下面的历史结果则打开该结果自己的 URL。

## 蓝牙设备

自动配置会新增：

- 关键字：`ly`
- 标题：`蓝牙设备`
- Url：`bhl://bluetooth?prefix=ly&selection={query}`
- 搜索提示：`Custom`
- 自定义提示 URL：`http://127.0.0.1:32119/suggest?prefix=ly&q={query}`

输入 `ly` 显示全部已配对物理设备；输入 `ly air` 按名称过滤。选择音频设备后，工具向 Windows 蓝牙音频驱动发送重连请求，并在 worker 退出后重新枚举系统状态。驱动接受请求不等于最终成功，只有独立 AEP 状态变为已连接才报告成功。休眠 HID 设备可能需要先物理唤醒。

蓝牙设置可在现有“配置 Listary 插件”窗口中修改，无需独立蓝牙页面；也对应 `BrowserHistoryLauncher.ini`：

```ini
[bluetooth]
Enabled=true
Keyword=ly
CacheSeconds=8
```

## 开机自动启动

本项目使用当前用户“启动”目录中的快捷方式，不安装 Windows 服务，也不需要管理员权限。在项目根目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\configure-startup.ps1
```

脚本会创建 `Listary浏览器插件.lnk`，目标为 `build\portable\BrowserHistoryLauncher.exe`。需要取消时执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\configure-startup.ps1 -Remove
```

在 portable 发布包中，同一个脚本会自动使用其上级目录里的 EXE。

## 交付给其他人

优先发送 `ListaryBrowserPlugin-Setup-x64.exe`。如果需要免安装版本，不要只发送单个 EXE；Listary 集成至少需要完整 portable 目录，其中包括：

- `BrowserHistoryLauncher.exe`
- `BrowserHistoryLauncher.ini`
- `scripts\setup-current-user.ps1` 和 `scripts\configure-startup.ps1`
- `docs\LISTARY_INTEGRATION.md`

接收者解压后运行一次 `setup-current-user.ps1`，再从主程序右上角配置浏览器即可。两个 PowerShell 脚本只是安装/移除辅助工具，不是运行时依赖；配置完成后，程序日常运行仍只有原生 EXE。

## 配置备份与恢复

在项目根目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\backup-listary-config.ps1
```

脚本会把 Listary 的 `Preferences.json` 以及 `g/e` 自定义网页搜索使用的图标复制到 `local-backup\Listary`。该目录包含个人 Listary 设置，已被 Git 忽略，不会提交到仓库。

恢复时：

1. 完全退出 Listary；
2. 把备份的 `Preferences.json` 复制到 `%APPDATA%\Listary\UserProfile\Settings\Preferences.json`；
3. 把备份的 `UserFiles\Images` 中的图标复制到 `%APPDATA%\Listary\UserProfile\UserFiles\Images`；
4. 重新启动 Listary 和 BrowserHistoryLauncher；
5. 如果移动过 EXE，再从新位置执行 `--register-listary-protocol`。

## 运行要求与边界

- 本工具必须保持运行，否则 Listary 无法访问本地建议接口，也没有本次查询的“显示项 → URL/Profile”临时映射。
- 开机启动快捷方式可由 `scripts\configure-startup.ps1` 创建；本项目不安装 Windows 服务，也不需要管理员权限。
- 建议映射只保存在内存中。工具重启后，需要重新在 Listary 输入查询再选择结果。
- 监听套接字只绑定 IPv4 loopback `127.0.0.1`，不接受局域网或公网连接。
- 普通 Listary“命令”模式和 `--query` 仍保留，作为原生窗口回退入口。
- Listary 保持默认的双击 `Ctrl` 呼出；本工具保留 INI 中的 `Ctrl+Shift+Space` 作为独立回退入口，两者互不冲突。

更多外接能力的职责划分和候选扩展见 `docs\DESKTOP_KEYBOARD_WORKFLOW.md`。

参考：

- Listary 官方命令文档：https://help.listary.com/options-commands
- Listary V7 Browser history plugin 路线图：https://www.listary.com/v7
