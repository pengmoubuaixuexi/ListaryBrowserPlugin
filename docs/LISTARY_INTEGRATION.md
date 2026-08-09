# Listary 原生结果集成

本机 Listary 6.3.5.94 可以把自定义网页搜索提示显示为 Listary 自己的下拉结果。本工具仅在 `127.0.0.1:32119` 提供按需建议接口；它不建立后台索引，也不持续扫描浏览器数据库。

选择结果时使用当前用户的 `bhl://` 协议把结果交回本工具，再由结果来源浏览器及 Profile 打开。这样不会经过 Windows 默认浏览器。

## 首次注册

保持 `BrowserHistoryLauncher.exe` 与 INI 在同一目录，执行：

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

输入 `g github`，选择历史结果并按 Enter，即使用来源 Chrome Profile 打开。

## Edge 历史

在 Listary“选项 → 网页搜索”中新增：

- 关键字：`e`
- 标题：`Edge 历史`
- Url：`bhl://open?prefix=e&selection={query}`
- 搜索提示：`Custom`
- 自定义提示 URL：`http://127.0.0.1:32119/suggest?prefix=e&q={query}`
- 启用：勾选

输入 `e microsoft`，选择历史结果并按 Enter，即使用来源 Edge Profile 打开。

## 开机自动启动

本项目使用当前用户“启动”目录中的快捷方式，不安装 Windows 服务，也不需要管理员权限。在项目根目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\configure-startup.ps1
```

脚本会创建 `Listary浏览器插件.lnk`，目标为 `build\portable\BrowserHistoryLauncher.exe`。需要取消时执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\configure-startup.ps1 -Remove
```

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
- 当前保留 INI 中的 `Alt+Shift+Space` 作为回退入口；Listary 使用 `Ctrl+Shift+Space`，两者互不冲突。

更多外接能力的职责划分和候选扩展见 `docs\DESKTOP_KEYBOARD_WORKFLOW.md`。

参考：

- Listary 官方命令文档：https://help.listary.com/options-commands
- Listary V7 Browser history plugin 路线图：https://www.listary.com/v7
